#include "pgn_scan.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_BUFFER_SIZE 65536u

typedef struct {
    FILE *file;
    unsigned char data[SCAN_BUFFER_SIZE];
    size_t used;
    size_t cursor;
    off_t offset;
    uint64_t line_number;
    int previous_byte;
    int byte_before_previous;
    int at_line_start;
    int line_is_blank;
    int previous_line_blank;
    int failed;
} Reader;

static void set_error(PgnScanError *error, size_t game_number,
                      uint64_t line_number, const char *format, ...)
{
    va_list arguments;
    error->game_number = game_number;
    error->line_number = line_number;
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

static void set_read_error(PgnScanError *error, size_t game_number,
                           const Reader *reader)
{
    set_error(error, game_number, reader->line_number,
              "input read failed: %s",
              strerror(reader->failed != 0 ? reader->failed : EIO));
}

static int reader_fill(Reader *reader)
{
    if (reader->cursor < reader->used)
        return 1;
    reader->cursor = 0;
    errno = 0;
    reader->used = fread(reader->data, 1, sizeof(reader->data), reader->file);
    if (reader->used != 0)
        return 1;
    if (ferror(reader->file))
        reader->failed = errno != 0 ? errno : EIO;
    return 0;
}

static int reader_peek(Reader *reader)
{
    if (!reader_fill(reader))
        return EOF;
    return reader->data[reader->cursor];
}

static int reader_get(Reader *reader)
{
    int byte = reader_peek(reader);
    if (byte != EOF) {
        ++reader->cursor;
        ++reader->offset;
        reader->byte_before_previous = reader->previous_byte;
        reader->previous_byte = byte;
        if (byte == '\n') {
            reader->previous_line_blank = reader->line_is_blank;
            reader->line_is_blank = 1;
            ++reader->line_number;
            reader->at_line_start = 1;
        } else {
            if (byte != ' ' && byte != '\t' && byte != '\r')
                reader->line_is_blank = 0;
            reader->at_line_start = 0;
        }
    }
    return byte;
}

static int is_space_byte(int byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' ||
           byte == '\n' || byte == '\f' || byte == '\v';
}

static int reserve_game(GameIndex *index)
{
    GameRange *replacement;
    size_t capacity;
    if (index->count < index->capacity)
        return 1;
    capacity = index->capacity == 0 ? 256 : index->capacity * 2;
    if (capacity < index->capacity ||
        capacity > SIZE_MAX / sizeof(*index->games))
        return 0;
    replacement = (GameRange *)realloc(index->games,
                                        capacity * sizeof(*index->games));
    if (replacement == NULL)
        return 0;
    index->games = replacement;
    index->capacity = capacity;
    return 1;
}

static int append_game(GameIndex *index, off_t begin, off_t body_end,
                       off_t trailing_comment_begin, off_t end,
                       PgnGameResult result, const PgnGameTags *tags)
{
    if (begin < 0 || body_end <= begin || end < body_end ||
        (trailing_comment_begin >= 0 &&
         (trailing_comment_begin < body_end ||
          trailing_comment_begin > end)) ||
        index->count == SIZE_MAX ||
        (index->collect_results &&
         (result < PGN_RESULT_UNDECIDED || result >= PGN_RESULT_COUNT ||
          index->results[result] == UINT64_MAX)) ||
        (index->retain_ranges && !reserve_game(index)))
        return 0;
    if (index->count == 0)
        index->leading_end = begin;
    if (index->retain_ranges) {
        index->games[index->count].begin = begin;
        index->games[index->count].body_end = body_end;
        index->games[index->count].trailing_comment_begin =
            trailing_comment_begin;
        index->games[index->count].end = end;
    }
    ++index->count;
    if (index->collect_results)
        ++index->results[result];
    if (index->game_callback != NULL)
        index->game_callback(index->count, tags,
                             index->game_callback_context);
    return 1;
}

static int need_result_metadata(const GameIndex *index)
{
    return index->collect_results ||
           (index->capture_tags & PGN_CAPTURE_RESULT) != 0;
}

static int consume_brace_comment(Reader *reader, uint64_t *error_line,
                                 const char **reason)
{
    int byte;
    uint64_t opening_line = reader->line_number;
    (void)reader_get(reader);
    while ((byte = reader_get(reader)) != EOF) {
        if (byte == '{') {
            *error_line = reader->line_number;
            *reason = "nested brace comment opener";
            return 0;
        }
        if (byte == '}')
            return 1;
    }
    *error_line = opening_line;
    *reason = "unterminated brace comment (opened here and reaches EOF)";
    return 0;
}

static int consume_line_comment(Reader *reader)
{
    int byte;
    (void)reader_get(reader);
    while ((byte = reader_peek(reader)) != EOF && byte != '\r' &&
           byte != '\n')
        (void)reader_get(reader);
    return 1;
}

static int consume_escape_line(Reader *reader)
{
    int byte;
    (void)reader_get(reader);
    while ((byte = reader_peek(reader)) != EOF && byte != '\r' &&
           byte != '\n')
        (void)reader_get(reader);
    return 1;
}

static int is_tag_name_byte(int byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
}

enum {
    TAG_PAIR_INVALID = 0,
    TAG_PAIR_VALID = 1,
    TAG_PAIR_RECOVERED = 2
};

static int consume_terminal_escaped_quote(Reader *reader)
{
    int byte;
    if (reader_peek(reader) != ']')
        return 0;
    (void)reader_get(reader);
    while ((byte = reader_peek(reader)) == ' ' || byte == '\t' ||
           byte == '\r')
        (void)reader_get(reader);
    return byte == '\n' || byte == EOF;
}

static PgnGameResult result_from_text(const char *value, size_t length)
{
    if (length == 3 && memcmp(value, "1-0", 3) == 0)
        return PGN_RESULT_WHITE_WIN;
    if (length == 3 && memcmp(value, "0-1", 3) == 0)
        return PGN_RESULT_BLACK_WIN;
    if (length == 7 && memcmp(value, "1/2-1/2", 7) == 0)
        return PGN_RESULT_DRAW;
    return PGN_RESULT_UNDECIDED;
}

static PgnTagValue *tag_value_slot(PgnGameTags *tags, uint32_t requested,
                                   const char *tag_name, uint32_t *tag_bit)
{
    if (tags == NULL || requested == 0)
        return NULL;
    if ((requested & PGN_CAPTURE_WHITE) != 0 &&
        strcmp(tag_name, "White") == 0)
        *tag_bit = PGN_CAPTURE_WHITE;
    else if ((requested & PGN_CAPTURE_WHITE_ELO) != 0 &&
             strcmp(tag_name, "WhiteElo") == 0)
        *tag_bit = PGN_CAPTURE_WHITE_ELO;
    else if ((requested & PGN_CAPTURE_BLACK) != 0 &&
             strcmp(tag_name, "Black") == 0)
        *tag_bit = PGN_CAPTURE_BLACK;
    else if ((requested & PGN_CAPTURE_BLACK_ELO) != 0 &&
             strcmp(tag_name, "BlackElo") == 0)
        *tag_bit = PGN_CAPTURE_BLACK_ELO;
    else if ((requested & PGN_CAPTURE_SITE) != 0 &&
             strcmp(tag_name, "Site") == 0)
        *tag_bit = PGN_CAPTURE_SITE;
    else if ((requested & PGN_CAPTURE_DATE) != 0 &&
             strcmp(tag_name, "Date") == 0)
        *tag_bit = PGN_CAPTURE_DATE;
    else if ((requested & PGN_CAPTURE_ECO) != 0 &&
             strcmp(tag_name, "ECO") == 0)
        *tag_bit = PGN_CAPTURE_ECO;
    else
        return NULL;
    switch (*tag_bit) {
    case PGN_CAPTURE_WHITE: return &tags->white;
    case PGN_CAPTURE_WHITE_ELO: return &tags->white_elo;
    case PGN_CAPTURE_BLACK: return &tags->black;
    case PGN_CAPTURE_BLACK_ELO: return &tags->black_elo;
    case PGN_CAPTURE_SITE: return &tags->site;
    case PGN_CAPTURE_DATE: return &tags->date;
    case PGN_CAPTURE_ECO: return &tags->eco;
    default: return NULL;
    }
}

static size_t tag_value_limit(uint32_t tag_bit)
{
    if (tag_bit == PGN_CAPTURE_WHITE_ELO ||
        tag_bit == PGN_CAPTURE_BLACK_ELO || tag_bit == PGN_CAPTURE_DATE)
        return 4;
    return PGN_SCAN_TAG_TEXT_LIMIT;
}

static void append_tag_byte(PgnTagValue *value, PgnGameTags *tags,
                            uint32_t tag_bit, size_t limit, int byte)
{
    if (value->length < limit) {
        value->text[value->length++] = (char)byte;
        value->text[value->length] = '\0';
    } else {
        tags->truncated |= tag_bit;
    }
}

static void copy_tag_value(PgnGameTags *target, const PgnGameTags *source,
                           uint32_t tag_bit)
{
    const PgnTagValue *source_value;
    PgnTagValue *target_value;

    if ((source->present & tag_bit) == 0)
        return;
    switch (tag_bit) {
    case PGN_CAPTURE_WHITE:
        source_value = &source->white;
        target_value = &target->white;
        break;
    case PGN_CAPTURE_WHITE_ELO:
        source_value = &source->white_elo;
        target_value = &target->white_elo;
        break;
    case PGN_CAPTURE_BLACK:
        source_value = &source->black;
        target_value = &target->black;
        break;
    case PGN_CAPTURE_BLACK_ELO:
        source_value = &source->black_elo;
        target_value = &target->black_elo;
        break;
    case PGN_CAPTURE_SITE:
        source_value = &source->site;
        target_value = &target->site;
        break;
    case PGN_CAPTURE_DATE:
        source_value = &source->date;
        target_value = &target->date;
        break;
    case PGN_CAPTURE_ECO:
        source_value = &source->eco;
        target_value = &target->eco;
        break;
    default:
        return;
    }
    target_value->length = source_value->length;
    memcpy(target_value->text, source_value->text,
           source_value->length + 1);
    target->present |= tag_bit;
    if ((source->truncated & tag_bit) != 0)
        target->truncated |= tag_bit;
}

static void copy_pending_tags(PgnGameTags *target,
                              const PgnGameTags *source)
{
    static const uint32_t text_tags[] = {
        PGN_CAPTURE_WHITE, PGN_CAPTURE_WHITE_ELO,
        PGN_CAPTURE_BLACK, PGN_CAPTURE_BLACK_ELO, PGN_CAPTURE_SITE,
        PGN_CAPTURE_DATE, PGN_CAPTURE_ECO
    };
    size_t index;
    for (index = 0; index < sizeof(text_tags) / sizeof(text_tags[0]); ++index)
        copy_tag_value(target, source, text_tags[index]);
    if ((source->present & PGN_CAPTURE_RESULT) != 0) {
        target->present |= PGN_CAPTURE_RESULT;
        target->result = source->result;
    }
}

static int consume_tag_pair(Reader *reader, char *tag_name,
                            size_t tag_name_size,
                            PgnGameTags *tags, uint32_t capture_tags,
                            PgnGameResult *tag_result)
{
    int byte;
    int have_name = 0;
    int have_space = 0;
    int is_result_tag;
    int value_valid = 1;
    int escaped = 0;
    int previous_was_backslash = 0;
    int escaped_by_one_backslash = 0;
    size_t tag_name_length = 0;
    size_t value_length = 0;
    uint32_t tag_bit = 0;
    PgnTagValue *capture_value;
    char value[8];

    if (tag_name_size == 0)
        return TAG_PAIR_INVALID;
    if (tag_result != NULL)
        *tag_result = PGN_RESULT_UNDECIDED;
    tag_name[0] = '\0';
    (void)reader_get(reader);
    while ((byte = reader_peek(reader)) != EOF && is_tag_name_byte(byte)) {
        have_name = 1;
        if (tag_name_length + 1 < tag_name_size)
            tag_name[tag_name_length] = (char)byte;
        ++tag_name_length;
        (void)reader_get(reader);
    }
    tag_name[tag_name_length < tag_name_size ? tag_name_length :
             tag_name_size - 1] = '\0';
    if (!have_name)
        return TAG_PAIR_INVALID;
    capture_value = tag_value_slot(tags, capture_tags, tag_name, &tag_bit);
    if (capture_value != NULL) {
        capture_value->length = 0;
        capture_value->text[0] = '\0';
        tags->truncated &= ~tag_bit;
    }
    is_result_tag = tag_result != NULL &&
                    strcmp(tag_name, "Result") == 0;
    while ((byte = reader_peek(reader)) != EOF && is_space_byte(byte)) {
        have_space = 1;
        (void)reader_get(reader);
    }
    if (!have_space || reader_get(reader) != '"')
        return TAG_PAIR_INVALID;
    while ((byte = reader_get(reader)) != EOF) {
        if (byte == '\\') {
            if (capture_value != NULL && escaped)
                append_tag_byte(capture_value, tags, tag_bit,
                                tag_value_limit(tag_bit), byte);
            if (is_result_tag)
                value_valid = 0;
            if (escaped) {
                escaped = 0;
                escaped_by_one_backslash = 0;
            } else {
                escaped = 1;
                escaped_by_one_backslash = !previous_was_backslash;
            }
            previous_was_backslash = 1;
            continue;
        }
        if (escaped) {
            if (byte == '"' && escaped_by_one_backslash &&
                consume_terminal_escaped_quote(reader)) {
                if (capture_value != NULL)
                    tags->present |= tag_bit;
                return TAG_PAIR_RECOVERED;
            }
            if (capture_value != NULL)
                append_tag_byte(capture_value, tags, tag_bit,
                                tag_value_limit(tag_bit), byte);
            escaped = 0;
            escaped_by_one_backslash = 0;
            previous_was_backslash = 0;
            continue;
        }
        previous_was_backslash = 0;
        if (byte == '"')
            break;
        if (capture_value != NULL)
            append_tag_byte(capture_value, tags, tag_bit,
                            tag_value_limit(tag_bit), byte);
        if (is_result_tag && value_valid) {
            if (value_length + 1 < sizeof(value))
                value[value_length++] = (char)byte;
            else
                value_valid = 0;
        }
    }
    if (byte == EOF)
        return TAG_PAIR_INVALID;
    while ((byte = reader_peek(reader)) != EOF &&
           (byte == ' ' || byte == '\t' || byte == '\r'))
        (void)reader_get(reader);
    if (reader_get(reader) != ']')
        return TAG_PAIR_INVALID;
    if (is_result_tag && tag_result != NULL && value_valid)
        *tag_result = result_from_text(value, value_length);
    if (capture_value != NULL)
        tags->present |= tag_bit;
    return TAG_PAIR_VALID;
}

static void record_tag_recovery(PgnScanReport *report, size_t game_number,
                                uint64_t line_number, const char *tag_name)
{
    if (report->repaired_tag_count != UINT64_MAX)
        ++report->repaired_tag_count;
    if (report->warning_count < PGN_SCAN_WARNING_LIMIT) {
        PgnScanWarning *warning =
            &report->warnings[report->warning_count++];
        warning->game_number = game_number;
        warning->line_number = line_number;
        (void)snprintf(warning->tag_name, sizeof(warning->tag_name), "%s",
                       tag_name);
    }
}

static int is_token_delimiter(int byte)
{
    return byte == EOF || is_space_byte(byte) || byte == '{' || byte == '}' ||
           byte == '(' || byte == ')' || byte == ';' || byte == '[' ||
           byte == ']';
}

static int is_result_token(const char *token, size_t length)
{
    return (length == 3 && memcmp(token, "1-0", 3) == 0) ||
           (length == 3 && memcmp(token, "0-1", 3) == 0) ||
           (length == 7 && memcmp(token, "1/2-1/2", 7) == 0) ||
           (length == 1 && token[0] == '*');
}

static int consume_token(Reader *reader, char *token, size_t token_size,
                         size_t *token_length)
{
    size_t length = 0;
    int byte;
    while (!is_token_delimiter(reader_peek(reader))) {
        byte = reader_get(reader);
        if (length + 1 < token_size)
            token[length] = (char)byte;
        ++length;
    }
    *token_length = length;
    if (token_size != 0)
        token[length < token_size ? length : token_size - 1] = '\0';
    return length != 0;
}

static int begin_game(int *active, int *header_allowed, int *result_seen,
                      PgnGameResult *game_result, PgnGameTags *tags,
                      int *rav_depth,
                      off_t *begin, off_t offset,
                      off_t *body_end, off_t *trailing_comment_begin,
                      off_t *end)
{
    *active = 1;
    *header_allowed = 1;
    *result_seen = 0;
    *game_result = PGN_RESULT_UNDECIDED;
    if (tags != NULL) {
        tags->present = 0;
        tags->truncated = 0;
        tags->result = PGN_RESULT_UNDECIDED;
    }
    *rav_depth = 0;
    *begin = offset;
    *body_end = 0;
    *trailing_comment_begin = -1;
    *end = 0;
    return 1;
}

static size_t next_game_number(const GameIndex *index)
{
    return index->count + 1;
}

int pgn_scan(FILE *input, GameIndex *index, PgnScanError *error,
             PgnScanReport *report)
{
    Reader reader;
    int active = 0;
    int header_allowed = 1;
    int result_seen = 0;
    PgnGameResult game_result = PGN_RESULT_UNDECIDED;
    PgnGameTags game_tags;
    PgnGameTags *game_tags_ptr = NULL;
    int rav_depth = 0;
    int byte;
    off_t game_begin = 0;
    off_t body_end = 0;
    off_t trailing_comment_begin = -1;
    off_t game_end = 0;
    int first_bytes[3];
    size_t first_count = 0;
    size_t index_byte;
    char token[16];

    memset(&reader, 0, sizeof(reader));
    if (index->capture_tags != 0 || index->game_callback != NULL) {
        memset(&game_tags, 0, sizeof(game_tags));
        game_tags_ptr = &game_tags;
    }
    memset(report, 0, sizeof(*report));
    reader.file = input;
    reader.line_number = 1;
    reader.at_line_start = 1;
    reader.line_is_blank = 1;
    for (index_byte = 0; index_byte < 3; ++index_byte) {
        byte = reader_get(&reader);
        if (byte == EOF)
            break;
        first_bytes[first_count++] = byte;
    }
    if (first_count == 3 && first_bytes[0] == 0xef &&
        first_bytes[1] == 0xbb && first_bytes[2] == 0xbf) {
        index->has_utf8_bom = 1;
        reader.at_line_start = 1;
        reader.line_is_blank = 1;
    } else {
        /* Replay the BOM probe from the reader buffer, including on pipes. */
        reader.cursor = 0;
        reader.offset = 0;
        reader.line_number = 1;
        reader.at_line_start = 1;
        reader.line_is_blank = 1;
        reader.previous_line_blank = 0;
        reader.previous_byte = 0;
        reader.byte_before_previous = 0;
    }

    while ((byte = reader_peek(&reader)) != EOF) {
        off_t byte_offset = reader.offset;
        size_t game_number = next_game_number(index);

        if (result_seen) {
            if (is_space_byte(byte)) {
                (void)reader_get(&reader);
                if (byte == '\n' && index->newline_is_crlf < 0)
                    index->newline_is_crlf =
                        reader.byte_before_previous == '\r';
                continue;
            }
            if (byte == '{' || byte == ';') {
                uint64_t error_line = reader.line_number;
                const char *reason = "invalid brace comment";
                if (trailing_comment_begin < 0)
                    trailing_comment_begin = byte_offset;
                if (byte == '{') {
                    if (!consume_brace_comment(&reader, &error_line,
                                               &reason)) {
                        if (reader.failed != 0)
                            set_read_error(error, game_number, &reader);
                        else
                            set_error(error, game_number, error_line, "%s",
                                      reason);
                        return 0;
                    }
                } else {
                    (void)consume_line_comment(&reader);
                }
                game_end = reader.offset;
                continue;
            }
            if (byte == '%' && reader.at_line_start) {
                if (trailing_comment_begin < 0)
                    trailing_comment_begin = byte_offset;
                (void)consume_escape_line(&reader);
                game_end = reader.offset;
                continue;
            }
            if (byte == '[') {
                uint64_t tag_line = reader.line_number;
                char tag_name[PGN_SCAN_TAG_NAME_SIZE];
                PgnGameTags pending_tags;
                PgnGameResult next_result = PGN_RESULT_UNDECIDED;
                int tag_result;

                if (index->capture_tags != 0) {
                    pending_tags.present = 0;
                    pending_tags.truncated = 0;
                    pending_tags.result = PGN_RESULT_UNDECIDED;
                }
                if (!reader.at_line_start) {
                    set_error(error, game_number + 1, tag_line,
                              "tag block after a result must start on a new "
                              "line after a blank line");
                    return 0;
                }
                tag_result = consume_tag_pair(
                    &reader, tag_name, sizeof(tag_name), &pending_tags,
                    index->capture_tags,
                    need_result_metadata(index) ? &next_result : NULL);
                if (tag_result == TAG_PAIR_INVALID) {
                    if (reader.failed != 0)
                        set_read_error(error, game_number + 1, &reader);
                    else
                        set_error(error, game_number + 1, tag_line,
                                  "malformed tag pair at a game boundary");
                    return 0;
                }
                if (tag_result == TAG_PAIR_RECOVERED)
                    record_tag_recovery(report, game_number + 1, tag_line,
                                        tag_name);
                if ((index->capture_tags & PGN_CAPTURE_RESULT) != 0 &&
                    strcmp(tag_name, "Result") == 0) {
                    pending_tags.result = next_result;
                    pending_tags.present |= PGN_CAPTURE_RESULT;
                }
                if (!reader.previous_line_blank) {
                    set_error(error, game_number + 1, tag_line,
                              "new tag block after a result requires a "
                              "blank line");
                    return 0;
                }
                if (!append_game(index, game_begin, body_end,
                                 trailing_comment_begin, game_end,
                                 game_result, game_tags_ptr)) {
                    set_error(error, game_number, tag_line,
                              "out of memory while indexing PGN games");
                    return 0;
                }
                begin_game(&active, &header_allowed, &result_seen,
                           &game_result, game_tags_ptr, &rav_depth, &game_begin,
                           byte_offset, &body_end,
                           &trailing_comment_begin, &game_end);
                if (index->capture_tags != 0)
                    copy_pending_tags(game_tags_ptr, &pending_tags);
                if (need_result_metadata(index) &&
                    strcmp(tag_name, "Result") == 0)
                    game_result = next_result;
                game_end = reader.offset;
                continue;
            }
            if (!append_game(index, game_begin, body_end,
                             trailing_comment_begin, game_end,
                             game_result, game_tags_ptr)) {
                set_error(error, game_number, reader.line_number,
                          "out of memory while indexing PGN games");
                return 0;
            }
            begin_game(&active, &header_allowed, &result_seen,
                       &game_result, game_tags_ptr, &rav_depth, &game_begin,
                       byte_offset, &body_end,
                       &trailing_comment_begin, &game_end);
        }

        if (is_space_byte(byte)) {
            (void)reader_get(&reader);
            if (byte == '\n' && index->newline_is_crlf < 0)
                index->newline_is_crlf =
                    reader.byte_before_previous == '\r';
            continue;
        }
        if (byte == '{' || byte == ';') {
            uint64_t error_line = reader.line_number;
            const char *reason = "invalid brace comment";
            if (!active)
                begin_game(&active, &header_allowed, &result_seen,
                           &game_result, game_tags_ptr, &rav_depth, &game_begin,
                           byte_offset, &body_end,
                           &trailing_comment_begin, &game_end);
            if (byte == '{') {
                if (!consume_brace_comment(&reader, &error_line, &reason)) {
                    if (reader.failed != 0)
                        set_read_error(error, next_game_number(index),
                                       &reader);
                    else
                        set_error(error, next_game_number(index), error_line,
                                  "%s", reason);
                    return 0;
                }
            } else {
                (void)consume_line_comment(&reader);
            }
            game_end = reader.offset;
            continue;
        }
        if (byte == '%' && reader.at_line_start) {
            if (!active)
                begin_game(&active, &header_allowed, &result_seen,
                           &game_result, game_tags_ptr, &rav_depth, &game_begin,
                           byte_offset, &body_end,
                           &trailing_comment_begin, &game_end);
            (void)consume_escape_line(&reader);
            game_end = reader.offset;
            continue;
        }
        if (byte == '[') {
            if (!header_allowed) {
                if (rav_depth != 0) {
                    set_error(error, game_number, reader.line_number,
                              "tag pair encountered inside a RAV variation");
                    return 0;
                }
                if (active && reader.at_line_start) {
                    size_t next_number = game_number + 1;
                    uint64_t tag_line = reader.line_number;
                    char tag_name[PGN_SCAN_TAG_NAME_SIZE];
                    PgnGameTags pending_tags;
                    PgnGameResult next_result = PGN_RESULT_UNDECIDED;
                    int tag_result;
                    if (!reader.previous_line_blank) {
                        tag_result = consume_tag_pair(&reader, tag_name,
                                                      sizeof(tag_name),
                                                      NULL, 0, NULL);
                        if (tag_result == TAG_PAIR_INVALID) {
                            if (reader.failed != 0)
                                set_read_error(error, next_number, &reader);
                            else
                                set_error(error, next_number, tag_line,
                                          "malformed tag pair; a blank line "
                                          "is required before a new game");
                        } else {
                            set_error(error, next_number, tag_line,
                                      "tag pair after incomplete movetext "
                                      "requires a blank line before the next "
                                      "game");
                        }
                        return 0;
                    }
                    if (index->capture_tags != 0) {
                        pending_tags.present = 0;
                        pending_tags.truncated = 0;
                        pending_tags.result = PGN_RESULT_UNDECIDED;
                    }
                    tag_result = consume_tag_pair(
                        &reader, tag_name, sizeof(tag_name), &pending_tags,
                        index->capture_tags,
                        need_result_metadata(index) ? &next_result : NULL);
                    if (tag_result == TAG_PAIR_INVALID) {
                        if (reader.failed != 0)
                            set_read_error(error, next_number, &reader);
                        else
                            set_error(error, next_number, tag_line,
                                      "malformed tag pair at a possible "
                                      "game boundary");
                        return 0;
                    }
                    if (tag_result == TAG_PAIR_RECOVERED)
                        record_tag_recovery(report, next_number, tag_line,
                                            tag_name);
                    if ((index->capture_tags & PGN_CAPTURE_RESULT) != 0 &&
                        strcmp(tag_name, "Result") == 0) {
                        pending_tags.result = next_result;
                        pending_tags.present |= PGN_CAPTURE_RESULT;
                    }
                    if (!append_game(index, game_begin, game_end,
                                     trailing_comment_begin, game_end,
                                     game_result, game_tags_ptr)) {
                        set_error(error, game_number, tag_line,
                                  "out of memory while indexing PGN games");
                        return 0;
                    }
                    begin_game(&active, &header_allowed, &result_seen,
                               &game_result, game_tags_ptr, &rav_depth, &game_begin,
                               byte_offset,
                               &body_end, &trailing_comment_begin,
                               &game_end);
                    if (index->capture_tags != 0)
                        copy_pending_tags(game_tags_ptr, &pending_tags);
                    if (need_result_metadata(index) &&
                        strcmp(tag_name, "Result") == 0)
                        game_result = next_result;
                    game_end = reader.offset;
                    continue;
                }
                set_error(error, game_number, reader.line_number,
                          "unexpected tag opener inside movetext; a new "
                          "game requires a tag line after a blank line");
                return 0;
            }
            if (!active)
                begin_game(&active, &header_allowed, &result_seen,
                           &game_result, game_tags_ptr, &rav_depth, &game_begin,
                           byte_offset, &body_end,
                           &trailing_comment_begin, &game_end);
            {
                uint64_t tag_line = reader.line_number;
                char tag_name[PGN_SCAN_TAG_NAME_SIZE];
                PgnGameResult tag_value_result = PGN_RESULT_UNDECIDED;
                int tag_result = consume_tag_pair(&reader, tag_name,
                                                  sizeof(tag_name),
                                                  game_tags_ptr,
                                                  index->capture_tags,
                                                  need_result_metadata(index)
                                                      ? &tag_value_result
                                                      : NULL);
                if (tag_result == TAG_PAIR_INVALID) {
                    if (reader.failed != 0)
                        set_read_error(error, game_number, &reader);
                    else
                        set_error(error, game_number, tag_line,
                                  "malformed PGN tag pair");
                    return 0;
                }
                if (tag_result == TAG_PAIR_RECOVERED)
                    record_tag_recovery(report, game_number, tag_line,
                                        tag_name);
                if (need_result_metadata(index) &&
                    strcmp(tag_name, "Result") == 0) {
                    game_result = tag_value_result;
                    if ((index->capture_tags & PGN_CAPTURE_RESULT) != 0) {
                        game_tags_ptr->result = tag_value_result;
                        game_tags_ptr->present |= PGN_CAPTURE_RESULT;
                    }
                }
            }
            game_end = reader.offset;
            continue;
        }
        if (byte == ']') {
            set_error(error, game_number, reader.line_number,
                      "unexpected closing bracket in movetext");
            return 0;
        }
        if (byte == '(') {
            if (!active)
                begin_game(&active, &header_allowed, &result_seen,
                           &game_result, game_tags_ptr, &rav_depth, &game_begin,
                           byte_offset, &body_end,
                           &trailing_comment_begin, &game_end);
            header_allowed = 0;
            ++rav_depth;
            (void)reader_get(&reader);
            game_end = reader.offset;
            continue;
        }
        if (byte == ')') {
            if (rav_depth == 0) {
                set_error(error, game_number, reader.line_number,
                          "unmatched closing RAV parenthesis");
                return 0;
            }
            --rav_depth;
            (void)reader_get(&reader);
            game_end = reader.offset;
            continue;
        }
        if (byte == '}') {
            set_error(error, game_number, reader.line_number,
                      "unexpected closing brace in movetext");
            return 0;
        }
        if (!active)
            begin_game(&active, &header_allowed, &result_seen,
                       &game_result, game_tags_ptr, &rav_depth, &game_begin,
                       byte_offset, &body_end,
                       &trailing_comment_begin, &game_end);
        header_allowed = 0;
        {
            size_t token_length = 0;
            if (!consume_token(&reader, token, sizeof(token), &token_length)) {
                set_error(error, game_number, reader.line_number,
                          "cannot tokenize movetext");
                return 0;
            }
            game_end = reader.offset;
            if (rav_depth == 0 && is_result_token(token, token_length)) {
                result_seen = 1;
                body_end = reader.offset;
                if (need_result_metadata(index))
                    game_result = result_from_text(token, token_length);
                if ((index->capture_tags & PGN_CAPTURE_RESULT) != 0)
                    game_tags_ptr->result = game_result;
            }
        }
    }

    if (reader.failed != 0) {
        set_error(error, next_game_number(index), reader.line_number,
                  "input read failed: %s", strerror(reader.failed));
        return 0;
    }
    if (active) {
        size_t game_number = next_game_number(index);
        if (rav_depth != 0) {
            set_error(error, game_number, reader.line_number,
                      "EOF inside an unclosed RAV variation");
            return 0;
        }
        if (!result_seen)
            body_end = game_end;
        if (!append_game(index, game_begin, body_end,
                         trailing_comment_begin, game_end, game_result,
                         game_tags_ptr)) {
            set_error(error, game_number, reader.line_number,
                      "could not index this PGN game");
            return 0;
        }
    }
    return 1;
}

void game_index_init(GameIndex *index)
{
    memset(index, 0, sizeof(*index));
    index->retain_ranges = 1;
    index->newline_is_crlf = -1;
    index->leading_end = -1;
}

void game_index_init_count_only(GameIndex *index, int collect_results)
{
    game_index_init(index);
    index->retain_ranges = 0;
    index->collect_results = collect_results;
}

void game_index_destroy(GameIndex *index)
{
    free(index->games);
    game_index_init(index);
}
