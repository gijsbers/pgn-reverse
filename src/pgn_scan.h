#ifndef PGN_REVERSE_PGN_SCAN_H
#define PGN_REVERSE_PGN_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

typedef enum {
    PGN_RESULT_UNDECIDED = 0,
    PGN_RESULT_WHITE_WIN,
    PGN_RESULT_BLACK_WIN,
    PGN_RESULT_DRAW,
    PGN_RESULT_COUNT
} PgnGameResult;

enum {
    PGN_CAPTURE_WHITE = 1u << 0,
    PGN_CAPTURE_WHITE_ELO = 1u << 1,
    PGN_CAPTURE_BLACK = 1u << 2,
    PGN_CAPTURE_BLACK_ELO = 1u << 3,
    PGN_CAPTURE_SITE = 1u << 4,
    PGN_CAPTURE_DATE = 1u << 5,
    PGN_CAPTURE_ECO = 1u << 6,
    PGN_CAPTURE_RESULT = 1u << 7
};

#define PGN_SCAN_TAG_DISPLAY_LIMIT 32u
#define PGN_SCAN_TAG_TEXT_LIMIT (PGN_SCAN_TAG_DISPLAY_LIMIT - 3u)
#define PGN_SCAN_TAG_VALUE_SIZE (PGN_SCAN_TAG_TEXT_LIMIT + 1u)

typedef struct {
    size_t length;
    /* Captured bytes are decoded tag contents, without the PGN quotes. */
    char text[PGN_SCAN_TAG_VALUE_SIZE];
} PgnTagValue;

typedef struct {
    uint32_t present;
    uint32_t truncated;
    PgnTagValue white;
    PgnTagValue white_elo;
    PgnTagValue black;
    PgnTagValue black_elo;
    PgnTagValue site;
    PgnTagValue date;
    PgnTagValue eco;
    PgnGameResult result;
} PgnGameTags;

typedef void (*PgnGameCallback)(size_t game_number,
                                const PgnGameTags *tags, void *context);

typedef struct {
    off_t begin;
    off_t body_end;
    off_t trailing_comment_begin;
    off_t end;
} GameRange;

typedef struct {
    GameRange *games;
    size_t count;
    size_t capacity;
    int has_utf8_bom;
    int newline_is_crlf;
    int retain_ranges;
    int collect_results;
    off_t leading_end;
    uint64_t results[PGN_RESULT_COUNT];
    /* Tag values are collected only when requested by the caller. */
    uint32_t capture_tags;
    PgnGameCallback game_callback;
    void *game_callback_context;
} GameIndex;

typedef struct {
    size_t game_number;
    uint64_t line_number;
    char message[192];
} PgnScanError;

#define PGN_SCAN_WARNING_LIMIT 10u
#define PGN_SCAN_TAG_NAME_SIZE 64u

typedef struct {
    size_t game_number;
    uint64_t line_number;
    char tag_name[PGN_SCAN_TAG_NAME_SIZE];
} PgnScanWarning;

typedef struct {
    uint64_t repaired_tag_count;
    size_t warning_count;
    PgnScanWarning warnings[PGN_SCAN_WARNING_LIMIT];
} PgnScanReport;

void game_index_init(GameIndex *index);
void game_index_init_count_only(GameIndex *index, int collect_results);
void game_index_destroy(GameIndex *index);
int pgn_scan(FILE *input, GameIndex *index, PgnScanError *error,
             PgnScanReport *report);

#endif
