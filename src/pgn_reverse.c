#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "pgn_scan.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define COPY_BUFFER_SIZE (1024u * 1024u)

typedef struct {
    int have_head;
    int have_tail;
    int have_skip;
    int force;
    int verbose;
    int help;
    int version;
    uint64_t head;
    uint64_t tail;
    uint64_t skip;
    const char *input_path;
    const char *output_path;
} Options;

typedef struct {
    dev_t device;
    ino_t inode;
    off_t size;
    mode_t mode;
    time_t modified_seconds;
    long modified_nanoseconds;
    time_t changed_seconds;
    long changed_nanoseconds;
} FileStamp;

typedef struct {
    char *directory;
    char *temporary;
} OutputPaths;

static void usage(FILE *stream)
{
    fputs("Usage: pgn-reverse [OPTIONS] INPUT OUTPUT\n"
          "Reverse the order of PGN chess games while preserving each game's\n"
          "original bytes.\n\n"
          "Options:\n"
          "  --head N     Select the first N games, then reverse them.\n"
          "  --tail N     Select the last N games, then reverse them.\n"
          "  --skip M     With --head, skip the first M selected games. With\n"
          "               --tail, skip the last M selected games.\n"
          "  --force      Atomically replace an existing output file.\n"
          "  --verbose    Report concise progress on stderr.\n"
          "  --version    Show version information.\n"
          "  --help, -?   Show this help.\n\n"
          "--head and --tail are mutually exclusive. --skip requires exactly\n"
          "one of them. A zero-game selection succeeds without creating or\n"
          "changing OUTPUT. Without --force, an existing OUTPUT is refused.\n\n"
          "Examples:\n"
          "  pgn-reverse games.pgn reversed.pgn\n"
          "  pgn-reverse --head 4 games.pgn first-four.pgn\n"
          "  pgn-reverse --tail 6 --skip 2 games.pgn middle-four.pgn\n"
          "  pgn-reverse --force --verbose games.pgn reversed.pgn\n",
          stream);
}

static void report_error(const char *format, ...)
{
    va_list arguments;
    fputs("pgn-reverse: ", stderr);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

static void report_scan_warnings(const PgnScanReport *report)
{
    size_t index;
    for (index = 0; index < report->warning_count; ++index) {
        const PgnScanWarning *warning = &report->warnings[index];
        fprintf(stderr,
                "pgn-reverse: warning: game %zu, line %" PRIu64
                ": malformed closing quote in %s tag; preserving original "
                "text\n",
                warning->game_number, warning->line_number,
                warning->tag_name);
    }
    if (report->repaired_tag_count > report->warning_count) {
        fprintf(stderr,
                "pgn-reverse: warning: %" PRIu64 " additional repaired "
                "tag warning(s) suppressed\n",
                report->repaired_tag_count - report->warning_count);
    }
    if (report->repaired_tag_count != 0) {
        fprintf(stderr,
                "pgn-reverse: recovered %" PRIu64 " malformed tag closing "
                "quote(s)\n",
                report->repaired_tag_count);
    }
}

static int parse_count(const char *text, uint64_t *value)
{
    const unsigned char *cursor = (const unsigned char *)text;
    uint64_t parsed = 0;
    if (*cursor == '\0')
        return 0;
    while (*cursor != '\0') {
        unsigned int digit;
        if (*cursor < '0' || *cursor > '9')
            return 0;
        digit = (unsigned int)(*cursor - '0');
        if (parsed > (UINT64_MAX - digit) / 10u)
            return 0;
        parsed = parsed * 10u + digit;
        ++cursor;
    }
    *value = parsed;
    return 1;
}

static int take_count(int argc, char **argv, int *index,
                      const char *option, uint64_t *value)
{
    if (*index + 1 >= argc) {
        report_error("%s requires a non-negative integer", option);
        return 0;
    }
    ++*index;
    if (!parse_count(argv[*index], value)) {
        report_error("%s requires a non-negative integer: %s",
                     option, argv[*index]);
        return 0;
    }
    return 1;
}

static int parse_options(int argc, char **argv, Options *options)
{
    int index;
    int options_done = 0;
    memset(options, 0, sizeof(*options));
    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (!options_done && strcmp(argument, "--") == 0) {
            options_done = 1;
        } else if (!options_done &&
                   (strcmp(argument, "--help") == 0 ||
                    strcmp(argument, "-?") == 0)) {
            options->help = 1;
        } else if (!options_done && strcmp(argument, "--version") == 0) {
            options->version = 1;
        } else if (!options_done && strcmp(argument, "--force") == 0) {
            options->force = 1;
        } else if (!options_done && strcmp(argument, "--verbose") == 0) {
            options->verbose = 1;
        } else if (!options_done && strcmp(argument, "--head") == 0) {
            if (options->have_head) {
                report_error("--head may be specified only once");
                return 0;
            }
            if (!take_count(argc, argv, &index, argument,
                            &options->head))
                return 0;
            options->have_head = 1;
        } else if (!options_done && strcmp(argument, "--tail") == 0) {
            if (options->have_tail) {
                report_error("--tail may be specified only once");
                return 0;
            }
            if (!take_count(argc, argv, &index, argument,
                            &options->tail))
                return 0;
            options->have_tail = 1;
        } else if (!options_done && strcmp(argument, "--skip") == 0) {
            if (options->have_skip) {
                report_error("--skip may be specified only once");
                return 0;
            }
            if (!take_count(argc, argv, &index, argument,
                            &options->skip))
                return 0;
            options->have_skip = 1;
        } else if (!options_done && argument[0] == '-') {
            report_error("unknown option: %s", argument);
            return 0;
        } else if (options->input_path == NULL) {
            options->input_path = argument;
        } else if (options->output_path == NULL) {
            options->output_path = argument;
        } else {
            report_error("too many file arguments");
            return 0;
        }
    }
    if (options->help || options->version)
        return 1;
    if (options->input_path == NULL || options->output_path == NULL) {
        report_error("INPUT and OUTPUT are required; use --help for usage");
        return 0;
    }
    if (options->have_head && options->have_tail) {
        report_error("--head and --tail cannot be used together");
        return 0;
    }
    if (options->have_skip && !options->have_head && !options->have_tail) {
        report_error("--skip requires --head or --tail");
        return 0;
    }
    return 1;
}

static void stamp_from_stat(FileStamp *stamp, const struct stat *status)
{
    stamp->device = status->st_dev;
    stamp->inode = status->st_ino;
    stamp->size = status->st_size;
    stamp->mode = status->st_mode;
    stamp->modified_seconds = status->st_mtim.tv_sec;
    stamp->modified_nanoseconds = status->st_mtim.tv_nsec;
    stamp->changed_seconds = status->st_ctim.tv_sec;
    stamp->changed_nanoseconds = status->st_ctim.tv_nsec;
}

static int stamps_equal(const FileStamp *left, const FileStamp *right)
{
    return left->device == right->device && left->inode == right->inode &&
           left->size == right->size &&
           (left->mode & S_IFMT) == (right->mode & S_IFMT) &&
           left->modified_seconds == right->modified_seconds &&
           left->modified_nanoseconds == right->modified_nanoseconds &&
           left->changed_seconds == right->changed_seconds &&
           left->changed_nanoseconds == right->changed_nanoseconds;
}

static int get_stamp(int descriptor, FileStamp *stamp)
{
    struct stat status;
    if (fstat(descriptor, &status) != 0)
        return 0;
    stamp_from_stat(stamp, &status);
    return 1;
}

static int output_path_is_safe(const Options *options,
                               const FileStamp *input_stamp,
                               int *exists, mode_t *existing_mode)
{
    struct stat link_status;
    struct stat target_status;
    if (strcmp(options->input_path, options->output_path) == 0) {
        report_error("INPUT and OUTPUT name the same path");
        return 0;
    }
    if (lstat(options->output_path, &link_status) != 0) {
        if (errno == ENOENT) {
            *exists = 0;
            return 1;
        }
        report_error("cannot inspect OUTPUT '%s': %s", options->output_path,
                     strerror(errno));
        return 0;
    }
    if (S_ISLNK(link_status.st_mode)) {
        report_error("symbolic links are not supported as OUTPUT");
        return 0;
    }
    if (!S_ISREG(link_status.st_mode)) {
        report_error("OUTPUT exists but is not a regular file");
        return 0;
    }
    if (stat(options->output_path, &target_status) != 0) {
        report_error("cannot inspect OUTPUT '%s': %s", options->output_path,
                     strerror(errno));
        return 0;
    }
    if (target_status.st_dev == input_stamp->device &&
        target_status.st_ino == input_stamp->inode) {
        report_error("INPUT and OUTPUT refer to the same file");
        return 0;
    }
    *exists = 1;
    *existing_mode = target_status.st_mode & 07777;
    return 1;
}

static int input_output_are_distinct(const Options *options,
                                     const FileStamp *input_stamp)
{
    struct stat output_status;
    if (strcmp(options->input_path, options->output_path) == 0) {
        report_error("INPUT and OUTPUT name the same path");
        return 0;
    }
    if (stat(options->output_path, &output_status) == 0) {
        if (output_status.st_dev == input_stamp->device &&
            output_status.st_ino == input_stamp->inode) {
            report_error("INPUT and OUTPUT refer to the same file");
            return 0;
        }
        return 1;
    }
    if (errno == ENOENT || errno == ENOTDIR)
        return 1;
    report_error("cannot inspect OUTPUT '%s': %s", options->output_path,
                 strerror(errno));
    return 0;
}

static int file_write_all(int descriptor, const unsigned char *data,
                          size_t length)
{
    while (length != 0) {
        ssize_t written = write(descriptor, data, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (written == 0) {
            errno = EIO;
            return 0;
        }
        data += written;
        length -= (size_t)written;
    }
    return 1;
}

static int copy_range(int input_fd, int output_fd, off_t begin, off_t end,
                      unsigned char *buffer)
{
    off_t position = begin;
    while (position < end) {
        off_t remaining = end - position;
        size_t wanted = remaining > (off_t)COPY_BUFFER_SIZE
            ? COPY_BUFFER_SIZE : (size_t)remaining;
        ssize_t received = pread(input_fd, buffer, wanted, position);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (received == 0) {
            errno = EIO;
            return 0;
        }
        if (!file_write_all(output_fd, buffer, (size_t)received))
            return 0;
        position += received;
    }
    return 1;
}

static int split_output_path(const char *path, OutputPaths *parts)
{
    const char *slash = strrchr(path, '/');
    size_t directory_length;
    const char *base_name;
    size_t base_length;
    size_t template_length;
    if (slash == NULL) {
        parts->directory = strdup(".");
        base_name = path;
    } else {
        directory_length = (size_t)(slash - path);
        if (directory_length == 0)
            parts->directory = strdup("/");
        else {
            parts->directory = (char *)malloc(directory_length + 1);
            if (parts->directory != NULL) {
                memcpy(parts->directory, path, directory_length);
                parts->directory[directory_length] = '\0';
            }
        }
        base_name = slash + 1;
    }
    if (parts->directory == NULL || base_name[0] == '\0')
        return 0;
    directory_length = strlen(parts->directory);
    base_length = strlen(base_name);
    if (directory_length > SIZE_MAX - base_length ||
        directory_length + base_length > SIZE_MAX - 32)
        return 0;
    template_length = directory_length + base_length + 32;
    parts->temporary = (char *)malloc(template_length);
    if (parts->temporary == NULL)
        return 0;
    if (snprintf(parts->temporary, template_length,
                 "%s/.%s.pgn-reverse.XXXXXX", parts->directory,
                 base_name) < 0)
        return 0;
    return 1;
}

static void free_output_path(OutputPaths *parts)
{
    free(parts->temporary);
    free(parts->directory);
    memset(parts, 0, sizeof(*parts));
}

static mode_t default_output_mode(void)
{
    mode_t mask = umask(0);
    (void)umask(mask);
    return (mode_t)(0666 & ~mask);
}

static int publish_output(int temporary_fd, const char *temporary_path,
                          const char *output_path, const char *directory,
                          int force)
{
    int directory_fd;
    int committed;
    if (fsync(temporary_fd) != 0) {
        int saved_error = errno;
        (void)close(temporary_fd);
        errno = saved_error;
        return 0;
    }
    if (close(temporary_fd) != 0)
        return 0;
    if (force) {
        committed = rename(temporary_path, output_path) == 0;
    } else {
        committed = link(temporary_path, output_path) == 0;
        if (committed && unlink(temporary_path) != 0) {
            report_error("output was published, but temporary link cleanup failed: %s",
                         strerror(errno));
        }
    }
    if (!committed)
        return 0;
    directory_fd = open(directory, O_RDONLY);
    if (directory_fd >= 0) {
        if (fsync(directory_fd) != 0)
            report_error("output was published, but directory sync failed: %s",
                         strerror(errno));
        (void)close(directory_fd);
    }
    return 1;
}

static void choose_selection(const Options *options, size_t total,
                             size_t *first, size_t *count)
{
    size_t selected = total;
    *first = 0;
    if (options->have_head) {
        if (options->head < (uint64_t)total)
            selected = (size_t)options->head;
        *first = options->skip < (uint64_t)selected
            ? (size_t)options->skip : selected;
        *count = selected - *first;
        return;
    }
    if (options->have_tail) {
        if (options->tail < (uint64_t)total)
            selected = (size_t)options->tail;
        *first = total - selected;
        *count = options->skip < (uint64_t)selected
            ? selected - (size_t)options->skip : 0;
        return;
    }
    *count = total;
}

static int write_selection(int input_fd, int output_fd,
                           const GameIndex *index, size_t first,
                           size_t count, unsigned char *buffer)
{
    size_t reverse_index;
    int full_selection = first == 0 && count == index->count;
    int separator_written = 0;
    const char *separator = index->newline_is_crlf ? "\r\n\r\n" : "\n\n";
    size_t separator_length = index->newline_is_crlf ? 4 : 2;
    if (full_selection && index->leading_end > 0) {
        if (!copy_range(input_fd, output_fd, 0, index->leading_end, buffer))
            return 0;
    } else if (index->has_utf8_bom) {
        const unsigned char bom[] = { 0xef, 0xbb, 0xbf };
        if (!file_write_all(output_fd, bom, sizeof(bom)))
            return 0;
    }
    reverse_index = first + count;
    while (reverse_index > first) {
        const GameRange *range;
        --reverse_index;
        range = &index->games[reverse_index];
        if (separator_written &&
            !file_write_all(output_fd,
                            (const unsigned char *)separator,
                            separator_length))
            return 0;
        if (!copy_range(input_fd, output_fd, range->begin, range->body_end,
                        buffer))
            return 0;
        if (range->trailing_comment_begin >= 0) {
            const char *newline = index->newline_is_crlf ? "\r\n" : "\n";
            size_t newline_length = index->newline_is_crlf ? 2 : 1;
            if (!file_write_all(output_fd,
                                (const unsigned char *)newline,
                                newline_length) ||
                !copy_range(input_fd, output_fd,
                            range->trailing_comment_begin, range->end,
                            buffer))
                return 0;
        }
        separator_written = 1;
    }
    /* Every emitted game, including the final one, ends with one blank line. */
    if (separator_written &&
        !file_write_all(output_fd, (const unsigned char *)separator,
                        separator_length))
        return 0;
    return 1;
}

static int verify_input_unchanged(int input_fd, const char *input_path,
                                  const FileStamp *baseline)
{
    FileStamp current;
    FileStamp pathname_stamp;
    struct stat pathname_status;
    if (!get_stamp(input_fd, &current) ||
        !stamps_equal(baseline, &current) ||
        stat(input_path, &pathname_status) != 0)
        return 0;
    stamp_from_stat(&pathname_stamp, &pathname_status);
    return stamps_equal(baseline, &pathname_stamp);
}

static int run(const Options *options)
{
    int input_fd = -1;
    FILE *input_stream = NULL;
    int temporary_fd = -1;
    char *temporary_path = NULL;
    OutputPaths output_parts;
    struct stat input_status;
    FileStamp input_stamp;
    FileStamp after_scan;
    GameIndex index;
    PgnScanError scan_error;
    PgnScanReport scan_report;
    int output_exists = 0;
    mode_t existing_mode = 0;
    size_t first = 0;
    size_t count = 0;
    unsigned char *copy_buffer = NULL;
    int result = 2;

    memset(&output_parts, 0, sizeof(output_parts));
    game_index_init(&index);
    input_fd = open(options->input_path, O_RDONLY);
    if (input_fd < 0) {
        report_error("cannot open INPUT '%s': %s", options->input_path,
                     strerror(errno));
        goto cleanup;
    }
    if (fstat(input_fd, &input_status) != 0 ||
        !S_ISREG(input_status.st_mode)) {
        report_error("INPUT must be a readable regular file");
        goto cleanup;
    }
    stamp_from_stat(&input_stamp, &input_status);
    if (!input_output_are_distinct(options, &input_stamp))
        goto cleanup;
    input_stream = fdopen(input_fd, "rb");
    if (input_stream == NULL) {
        report_error("cannot create input stream: %s", strerror(errno));
        goto cleanup;
    }
    if (options->verbose) {
        fprintf(stderr, "pgn-reverse: reading %s\n", options->input_path);
        fputs("pgn-reverse: indexing PGN game boundaries\n", stderr);
    }
    if (!pgn_scan(input_stream, &index, &scan_error, &scan_report)) {
        report_error("game %zu, line %" PRIu64 ": %s",
                     scan_error.game_number, scan_error.line_number,
                     scan_error.message);
        goto cleanup;
    }
    if (!get_stamp(fileno(input_stream), &after_scan) ||
        !stamps_equal(&input_stamp, &after_scan) ||
        !verify_input_unchanged(fileno(input_stream), options->input_path,
                                &input_stamp)) {
        report_error("INPUT changed while its PGN index was being built");
        goto cleanup;
    }
    if (options->verbose)
        report_scan_warnings(&scan_report);
    choose_selection(options, index.count, &first, &count);
    if (options->verbose) {
        fprintf(stderr, "pgn-reverse: found %zu games; selected %zu\n",
                index.count, count);
    }
    if (count == 0) {
        if (options->verbose)
            fputs("pgn-reverse: no games selected; no output written\n",
                  stderr);
        result = 0;
        goto cleanup;
    }
    if (!output_path_is_safe(options, &input_stamp, &output_exists,
                             &existing_mode))
        goto cleanup;
    if (output_exists && !options->force) {
        report_error("OUTPUT already exists (use --force to replace it)");
        goto cleanup;
    }
    if (!verify_input_unchanged(fileno(input_stream), options->input_path,
                                &input_stamp)) {
        report_error("INPUT changed after indexing; no output was published");
        goto cleanup;
    }
    if (!split_output_path(options->output_path, &output_parts)) {
        report_error("cannot prepare a temporary output path");
        goto cleanup;
    }
    temporary_path = output_parts.temporary;
    temporary_fd = mkstemp(temporary_path);
    if (temporary_fd < 0) {
        report_error("cannot create temporary output beside OUTPUT: %s",
                     strerror(errno));
        goto cleanup;
    }
    if (fchmod(temporary_fd, output_exists
                   ? existing_mode : default_output_mode()) != 0) {
        report_error("cannot set output permissions: %s", strerror(errno));
        goto cleanup;
    }
    copy_buffer = (unsigned char *)malloc(COPY_BUFFER_SIZE);
    if (copy_buffer == NULL) {
        report_error("not enough memory for the copy buffer");
        goto cleanup;
    }
    if (!write_selection(fileno(input_stream), temporary_fd, &index, first,
                         count, copy_buffer)) {
        report_error("failed while writing temporary OUTPUT: %s",
                     strerror(errno));
        goto cleanup;
    }
    if (!verify_input_unchanged(fileno(input_stream), options->input_path,
                                &input_stamp)) {
        report_error("INPUT changed while output was being prepared; "
                     "no output was published");
        goto cleanup;
    }
    if (!publish_output(temporary_fd, temporary_path, options->output_path,
                        output_parts.directory, options->force)) {
        temporary_fd = -1;
        report_error("cannot publish OUTPUT: %s", strerror(errno));
        goto cleanup;
    }
    temporary_fd = -1;
    if (options->verbose)
        fprintf(stderr, "pgn-reverse: published %zu reversed games to %s\n",
                count, options->output_path);
    result = 0;

cleanup:
    if (temporary_fd >= 0)
        (void)close(temporary_fd);
    if (temporary_path != NULL)
        (void)unlink(temporary_path);
    free(copy_buffer);
    if (input_stream != NULL)
        (void)fclose(input_stream);
    else if (input_fd >= 0)
        (void)close(input_fd);
    game_index_destroy(&index);
    free_output_path(&output_parts);
    return result;
}

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        fputs("Try 'pgn-reverse --help' for usage.\n", stderr);
        return 2;
    }
    if (options.version) {
        puts("pgn-reverse 1.0.0");
        return 0;
    }
    if (options.help) {
        usage(stdout);
        return 0;
    }
    return run(&options);
}
