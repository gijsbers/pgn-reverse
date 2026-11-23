#define _POSIX_C_SOURCE 200809L

#include "pgn_scan.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PGN_COUNT_VERSION "1.0.0"

typedef struct {
    int summary;
    int no_filename;
    int results;
    int strict;
    int help;
    int version;
    const char **inputs;
    size_t input_count;
} Options;

typedef struct {
    uint64_t games;
    uint64_t results[PGN_RESULT_COUNT];
} Counts;

static void usage(FILE *stream)
{
    fputs("Usage: pgn-count [OPTIONS] FILE [FILE ...]\n"
          "Count chess games in PGN files.\n\n"
          "Options:\n"
          "  -n, --no-filename  Omit filenames from per-file counts.\n"
          "  -r, --results      Also count wins, draws, and undecided games.\n"
          "  -s, --summary      Print only the combined total.\n"
          "      --strict       Stop after the first file that cannot be counted.\n"
          "      --version      Show version information.\n\n"
          "      --help, -?     Show this help.\n"
          "Use FILE '-' to read standard input.\n",
          stream);
}

static void report_error(const char *file, const char *message)
{
    fprintf(stderr, "pgn-count: %s: %s\n", file, message);
}

static int parse_options(int argc, char **argv, Options *options)
{
    int index;
    int options_done = 0;

    memset(options, 0, sizeof(*options));
    options->inputs = (const char **)calloc((size_t)argc, sizeof(*options->inputs));
    if (options->inputs == NULL) {
        report_error("command line", "not enough memory for input names");
        return 0;
    }

    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (!options_done && strcmp(argument, "--") == 0) {
            options_done = 1;
        } else if (!options_done && strcmp(argument, "--summary") == 0) {
            options->summary = 1;
        } else if (!options_done && strcmp(argument, "-s") == 0) {
            options->summary = 1;
        } else if (!options_done &&
                   (strcmp(argument, "--no-filename") == 0 ||
                    strcmp(argument, "-n") == 0)) {
            options->no_filename = 1;
        } else if (!options_done &&
                   (strcmp(argument, "--results") == 0 ||
                    strcmp(argument, "-r") == 0)) {
            options->results = 1;
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
            fprintf(stderr, "pgn-count: unknown option: %s\n", argument);
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

static int read_counts(const char *path, int collect_results, Counts *counts)
{
    FILE *input;
    int close_input;
    GameIndex index;
    PgnScanError scan_error;
    PgnScanReport scan_report;
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

    game_index_init_count_only(&index, collect_results);
    scan_ok = pgn_scan(input, &index, &scan_error, &scan_report);
    if (!scan_ok) {
        fprintf(stderr, "pgn-count: %s: game %zu, line %" PRIu64 ": %s\n",
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

    if (scan_ok) {
        counts->games = (uint64_t)index.count;
        memcpy(counts->results, index.results, sizeof(counts->results));
    }
    game_index_destroy(&index);
    return scan_ok;
}

static int add_counts(Counts *total, const Counts *part)
{
    size_t index;
    if (UINT64_MAX - total->games < part->games)
        return 0;
    for (index = 0; index < PGN_RESULT_COUNT; ++index) {
        if (UINT64_MAX - total->results[index] < part->results[index])
            return 0;
    }
    total->games += part->games;
    for (index = 0; index < PGN_RESULT_COUNT; ++index)
        total->results[index] += part->results[index];
    return 1;
}

static void print_counts(const char *name, int show_name, int show_results,
                         const Counts *counts)
{
    if (show_name)
        printf("%s: ", name);
    printf("%" PRIu64 " game%s", counts->games,
           counts->games == 1 ? "" : "s");
    if (show_results) {
        printf("; white wins: %" PRIu64 "; black wins: %" PRIu64
               "; draws: %" PRIu64 "; undecided: %" PRIu64,
               counts->results[PGN_RESULT_WHITE_WIN],
               counts->results[PGN_RESULT_BLACK_WIN],
               counts->results[PGN_RESULT_DRAW],
               counts->results[PGN_RESULT_UNDECIDED]);
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    Options options;
    Counts total;
    size_t input_count;
    size_t index;
    int failed = 0;

    memset(&total, 0, sizeof(total));
    if (!parse_options(argc, argv, &options)) {
        fputs("Try 'pgn-count --help' for usage.\n", stderr);
        free(options.inputs);
        return 2;
    }
    if (options.version) {
        puts("pgn-count " PGN_COUNT_VERSION);
        free(options.inputs);
        return 0;
    }
    if (options.help) {
        usage(stdout);
        free(options.inputs);
        return 0;
    }

    input_count = options.input_count;
    for (index = 0; index < input_count; ++index) {
        const char *path = options.inputs[index];
        const char *display_name = path;
        Counts part;

        memset(&part, 0, sizeof(part));
        if (!read_counts(path, options.results, &part)) {
            failed = 1;
            if (options.strict)
                break;
            continue;
        }
        if (!add_counts(&total, &part)) {
            report_error("total", "count exceeds the supported range");
            failed = 1;
            if (options.strict)
                break;
            continue;
        }
        if (!options.summary)
            print_counts(display_name, !options.no_filename,
                         options.results, &part);
    }

    if (!failed && (options.summary || input_count > 1))
        print_counts("Total", 1, options.results, &total);
    if (ferror(stdout)) {
        report_error("standard output", "write failed");
        failed = 1;
    }
    free(options.inputs);
    return failed ? 2 : 0;
}
