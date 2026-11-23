#define _POSIX_C_SOURCE 200809L

#include "pgn_scan.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PGN_LIST_VERSION "1.0.0"
#define PGN_LIST_TAGS (PGN_CAPTURE_WHITE | PGN_CAPTURE_WHITE_ELO | \
                       PGN_CAPTURE_BLACK | PGN_CAPTURE_BLACK_ELO | \
                       PGN_CAPTURE_SITE | PGN_CAPTURE_DATE | \
                       PGN_CAPTURE_ECO | PGN_CAPTURE_RESULT)

typedef struct {
    const char *file_name;
    int no_filename;
} OutputContext;

typedef struct {
    int no_filename;
    int strict;
    int help;
    int version;
    const char **inputs;
    size_t input_count;
} Options;

static void usage(FILE *stream)
{
    fputs("Usage: pgn-list [OPTIONS] FILE [FILE ...]\n"
          "List the players and basic details for each PGN game.\n\n"
          "Options:\n"
          "  -n, --no-filename  Omit filenames from output lines.\n"
          "      --strict       Stop after the first file that cannot be listed.\n"
          "      --help, -?     Show this help.\n"
          "      --version      Show version information.\n\n"
          "Use FILE '-' to read standard input.\n",
          stream);
}

static void report_error(const char *file, const char *message)
{
    fprintf(stderr, "pgn-list: %s: %s\n", file, message);
}

static int parse_options(int argc, char **argv, Options *options)
{
    int index;
    int options_done = 0;

    memset(options, 0, sizeof(*options));
    options->inputs = (const char **)calloc((size_t)argc,
                                             sizeof(*options->inputs));
    if (options->inputs == NULL) {
        report_error("command line", "not enough memory for input names");
        return 0;
    }

    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (!options_done && strcmp(argument, "--") == 0) {
            options_done = 1;
        } else if (!options_done &&
                   (strcmp(argument, "--no-filename") == 0 ||
                    strcmp(argument, "-n") == 0)) {
            options->no_filename = 1;
        } else if (!options_done && strcmp(argument, "--strict") == 0) {
            options->strict = 1;
        } else if (!options_done &&
                   (strcmp(argument, "--help") == 0 ||
                    strcmp(argument, "-?") == 0)) {
            options->help = 1;
        } else if (!options_done && strcmp(argument, "--version") == 0) {
            options->version = 1;
        } else if (!options_done && argument[0] == '-' &&
                   argument[1] != '\0' && strcmp(argument, "-") != 0) {
            fprintf(stderr, "pgn-list: unknown option: %s\n", argument);
            return 0;
        } else {
            options->inputs[options->input_count++] = argument;
        }
    }

    if (options->help || options->version)
        return 1;
    if (options->input_count == 0) {
        report_error("command line",
                     "at least one FILE is required (use '-' for standard input)");
        return 0;
    }

    {
        size_t input_index;
        int stdin_seen = 0;
        for (input_index = 0; input_index < options->input_count;
             ++input_index) {
            if (strcmp(options->inputs[input_index], "-") == 0) {
                if (stdin_seen) {
                    report_error("command line",
                                 "standard input may be specified only once");
                    return 0;
                }
                stdin_seen = 1;
            }
        }
    }
    return 1;
}

static void print_text(const char *text, size_t length, int truncated)
{
    size_t index;
    for (index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)text[index];
        if (byte == '\n' || byte == '\r' || byte == '\t')
            putchar(' ');
        else if (byte == '|')
            fputs("\\|", stdout);
        else if (byte < 0x20)
            putchar('?');
        else
            putchar(byte);
    }
    if (truncated)
        fputs("...", stdout);
}

static void print_tag(const PgnGameTags *tags, uint32_t tag_bit,
                      const PgnTagValue *value)
{
    if ((tags->present & tag_bit) == 0 || value->length == 0) {
        putchar('?');
        return;
    }
    print_text(value->text, value->length,
               (tags->truncated & tag_bit) != 0);
}

static void print_rating(const PgnGameTags *tags, uint32_t tag_bit,
                         const PgnTagValue *value)
{
    if ((tags->present & tag_bit) == 0 || value->length == 0) {
        putchar('?');
        return;
    }
    print_text(value->text, value->length, 0);
}

static void print_file_name(const char *file_name)
{
    print_text(file_name, strlen(file_name), 0);
}

static void print_player(const PgnGameTags *tags, uint32_t name_bit,
                         const PgnTagValue *name, uint32_t elo_bit,
                         const PgnTagValue *elo)
{
    size_t length = name->length;
    size_t start = 0;
    int name_was_truncated = (tags->truncated & name_bit) != 0;
    const char *comma;

    if ((tags->present & name_bit) == 0) {
        putchar('?');
    } else {
        comma = (const char *)memchr(name->text, ',', length);
        if (comma != NULL) {
            length = (size_t)(comma - name->text);
            name_was_truncated = 0;
        }
        while (start < length && isspace((unsigned char)name->text[start]))
            ++start;
        while (length > start &&
               isspace((unsigned char)name->text[length - 1]))
            --length;
        if (length == start)
            putchar('?');
        else
            print_text(name->text + start, length - start,
                       name_was_truncated);
    }
    if ((tags->present & elo_bit) != 0 && elo->length != 0) {
        putchar(' ');
        print_rating(tags, elo_bit, elo);
    }
}

static void print_year(const PgnGameTags *tags)
{
    size_t index;
    if ((tags->present & PGN_CAPTURE_DATE) == 0 || tags->date.length < 4) {
        putchar('?');
        return;
    }
    for (index = 0; index < 4; ++index) {
        if (tags->date.text[index] < '0' || tags->date.text[index] > '9') {
            putchar('?');
            return;
        }
    }
    print_text(tags->date.text, 4, 0);
}

static const char *result_text(PgnGameResult result)
{
    switch (result) {
    case PGN_RESULT_WHITE_WIN: return "1-0";
    case PGN_RESULT_BLACK_WIN: return "0-1";
    case PGN_RESULT_DRAW: return "1/2-1/2";
    default: return "*";
    }
}

static void print_game(size_t game_number, const PgnGameTags *tags,
                       void *context)
{
    const OutputContext *output = (const OutputContext *)context;
    (void)game_number;

    if (!output->no_filename) {
        print_file_name(output->file_name);
        fputs(": ", stdout);
    }
    print_player(tags, PGN_CAPTURE_WHITE, &tags->white,
                 PGN_CAPTURE_WHITE_ELO, &tags->white_elo);
    fputs(" - ", stdout);
    print_player(tags, PGN_CAPTURE_BLACK, &tags->black,
                 PGN_CAPTURE_BLACK_ELO, &tags->black_elo);
    fputs(" | ", stdout);
    print_tag(tags, PGN_CAPTURE_SITE, &tags->site);
    fputs(" | ", stdout);
    print_year(tags);
    fputs(" | ", stdout);
    print_tag(tags, PGN_CAPTURE_ECO, &tags->eco);
    printf(" | %s\n", result_text(tags->result));
}

static int list_file(const char *path, int no_filename)
{
    FILE *input;
    int close_input;
    GameIndex index;
    PgnScanError scan_error;
    PgnScanReport scan_report;
    OutputContext output;
    int scan_ok;

    if (strcmp(path, "-") == 0) {
        input = stdin;
        close_input = 0;
    } else {
        input = fopen(path, "rb");
        close_input = 1;
        if (input == NULL) {
            char message[256];
            (void)snprintf(message, sizeof(message), "cannot open: %s",
                           strerror(errno));
            report_error(path, message);
            return 0;
        }
    }

    output.file_name = path;
    output.no_filename = no_filename;
    game_index_init_count_only(&index, 0);
    index.capture_tags = PGN_LIST_TAGS;
    index.game_callback = print_game;
    index.game_callback_context = &output;
    scan_ok = pgn_scan(input, &index, &scan_error, &scan_report);
    if (!scan_ok) {
        fprintf(stderr, "pgn-list: %s: game %zu, line %" PRIu64 ": %s\n",
                path, scan_error.game_number, scan_error.line_number,
                scan_error.message);
    }
    if (close_input && fclose(input) != 0) {
        if (scan_ok) {
            char message[256];
            (void)snprintf(message, sizeof(message), "read failed: %s",
                           strerror(errno));
            report_error(path, message);
        }
        scan_ok = 0;
    }
    game_index_destroy(&index);
    return scan_ok;
}

int main(int argc, char **argv)
{
    Options options;
    size_t index;
    int failed = 0;

    if (!parse_options(argc, argv, &options)) {
        fputs("Try 'pgn-list --help' for usage.\n", stderr);
        free(options.inputs);
        return 2;
    }
    if (options.version) {
        puts("pgn-list " PGN_LIST_VERSION);
        free(options.inputs);
        return 0;
    }
    if (options.help) {
        usage(stdout);
        free(options.inputs);
        return 0;
    }

    for (index = 0; index < options.input_count; ++index) {
        if (!list_file(options.inputs[index], options.no_filename)) {
            failed = 1;
            if (options.strict)
                break;
        }
    }
    if (ferror(stdout)) {
        report_error("standard output", "write failed");
        failed = 1;
    }
    free(options.inputs);
    return failed ? 2 : 0;
}
