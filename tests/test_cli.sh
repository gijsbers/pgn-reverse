#!/bin/sh
set -eu

program=$(pwd)/pgn-reverse
test_tmpdir=${TMPDIR:-/tmp}
test_root=$(mktemp -d "${test_tmpdir%/}/pgn-reverse-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

expect_status()
{
    expected=$1
    shift
    if "$@" >"$test_root/stdout" 2>"$test_root/stderr"; then
        actual=0
    else
        actual=$?
    fi
    test "$actual" -eq "$expected" || fail "status $actual, expected $expected: $*"
}

expect_same()
{
    cmp "$1" "$2" || fail "files differ: $1 and $2"
}

expect_parser_error()
{
    expected_diagnostic=$1
    parser_input=$2
    parser_output=$3
    expect_status 2 "$program" "$parser_input" "$parser_output"
    grep -F "$expected_diagnostic" "$test_root/stderr" >/dev/null || \
        fail "parser diagnostic missing: $expected_diagnostic"
    test ! -e "$parser_output" || fail "parser error published output"
}

cat >"$test_root/input.pgn" <<'PGN'
[Event "A"]
[White "Alpha"]
[Result "1-0"]

1. e4 {comment has [Event "fake"] and 0-1} (1. d4 d5) 1-0

[White "Beta"]
[Result "0-1"]

1. d4 ; this result-like text 1-0 is a comment
d5 0-1

[Event "C"]
[Result "*"]

1. c4 $1 {UTF-8: ♟} *

[Event "D"]
[Result "1/2-1/2"]

1. Nf3 (1. g3 d5) 1/2-1/2

[White "E"]
[Result "*"]

% escape line contains a fake result 1-0 and fake tag [Event "ignored"]
1. b3 *
PGN

cat >"$test_root/expected-all.pgn" <<'PGN'
[White "E"]
[Result "*"]

% escape line contains a fake result 1-0 and fake tag [Event "ignored"]
1. b3 *

[Event "D"]
[Result "1/2-1/2"]

1. Nf3 (1. g3 d5) 1/2-1/2

[Event "C"]
[Result "*"]

1. c4 $1 {UTF-8: ♟} *

[White "Beta"]
[Result "0-1"]

1. d4 ; this result-like text 1-0 is a comment
d5 0-1

[Event "A"]
[White "Alpha"]
[Result "1-0"]

1. e4 {comment has [Event "fake"] and 0-1} (1. d4 d5) 1-0
PGN
printf '\n' >>"$test_root/expected-all.pgn"

"$program" "$test_root/input.pgn" "$test_root/all.pgn"
expect_same "$test_root/expected-all.pgn" "$test_root/all.pgn"
"$program" --head 1 "$test_root/input.pgn" "$test_root/head1.pgn"
grep -q '\[White "Alpha"\]' "$test_root/head1.pgn" || fail "head 1 selection"
if grep -q '\[White "Beta"\]' "$test_root/head1.pgn"; then fail "head selected too many"; fi
"$program" --tail 1 "$test_root/input.pgn" "$test_root/tail1.pgn"
grep -q '\[White "E"\]' "$test_root/tail1.pgn" || fail "tail 1 selection"
"$program" --head 3 --skip 1 "$test_root/input.pgn" "$test_root/head-skip.pgn"
grep -q '\[Event "C"\]' "$test_root/head-skip.pgn" || fail "head skip selected wrong games"
grep -q '\[White "Beta"\]' "$test_root/head-skip.pgn" || fail "head skip lost first retained game"
if grep -q '\[Event "A"\]' "$test_root/head-skip.pgn"; then fail "head skip retained skipped game"; fi
"$program" --tail 3 --skip 1 "$test_root/input.pgn" "$test_root/tail-skip.pgn"
grep -q '\[Event "D"\]' "$test_root/tail-skip.pgn" || fail "tail skip selection"
if grep -q '\[White "E"\]' "$test_root/tail-skip.pgn"; then fail "tail skip retained skipped tail"; fi
"$program" --head 99 "$test_root/input.pgn" "$test_root/head-large.pgn"
expect_same "$test_root/expected-all.pgn" "$test_root/head-large.pgn"
"$program" --tail 99 "$test_root/input.pgn" "$test_root/tail-large.pgn"
expect_same "$test_root/expected-all.pgn" "$test_root/tail-large.pgn"
"$program" --head 5 "$test_root/input.pgn" "$test_root/head-equal.pgn"
expect_same "$test_root/expected-all.pgn" "$test_root/head-equal.pgn"
"$program" --tail 5 "$test_root/input.pgn" "$test_root/tail-equal.pgn"
expect_same "$test_root/expected-all.pgn" "$test_root/tail-equal.pgn"
"$program" --head 3 --skip 0 "$test_root/input.pgn" "$test_root/head-skip-zero.pgn"
grep -q '\[Event "C"\]' "$test_root/head-skip-zero.pgn" || fail "head skip zero changed selection"
"$program" --head 3 --skip 2 "$test_root/input.pgn" "$test_root/one-selected.pgn"
grep -q '\[Event "C"\]' "$test_root/one-selected.pgn" || fail "one-game selection lost game"
if grep -q '\[White "Beta"\]' "$test_root/one-selected.pgn"; then fail "one-game selection included an extra game"; fi

expect_status 0 "$program" --head 0 "$test_root/input.pgn" "$test_root/empty.pgn"
test ! -e "$test_root/empty.pgn" || fail "empty selection created output"
expect_status 0 "$program" --tail 0 "$test_root/input.pgn" "$test_root/empty-tail-zero.pgn"
test ! -e "$test_root/empty-tail-zero.pgn" || fail "tail zero created output"
: >"$test_root/empty-skip.pgn"
expect_status 0 "$program" --head 5 --skip 5 "$test_root/input.pgn" "$test_root/empty-skip.pgn"
test ! -s "$test_root/empty-skip.pgn" || fail "head skip did not select zero games"
expect_status 0 "$program" --tail 3 --skip 10 "$test_root/input.pgn" "$test_root/empty-tail.pgn"
test ! -e "$test_root/empty-tail.pgn" || fail "oversized skip created output"
printf 'sentinel\n' >"$test_root/sentinel.pgn"
expect_status 0 "$program" --force --tail 0 "$test_root/input.pgn" "$test_root/sentinel.pgn"
test "$(cat "$test_root/sentinel.pgn")" = sentinel || fail "empty selection changed existing output"
: >"$test_root/empty-input.pgn"
expect_status 0 "$program" "$test_root/empty-input.pgn" "$test_root/no-games.pgn"
test ! -e "$test_root/no-games.pgn" || fail "empty input created output"

expect_status 2 "$program" --skip 1 "$test_root/input.pgn" "$test_root/bad.pgn"
expect_status 2 "$program" --head 1 --tail 1 "$test_root/input.pgn" "$test_root/bad.pgn"
expect_status 2 "$program" --head -1 "$test_root/input.pgn" "$test_root/bad.pgn"
expect_status 2 "$program" --head 18446744073709551616 "$test_root/input.pgn" "$test_root/bad.pgn"
expect_status 2 "$program" --unknown "$test_root/input.pgn" "$test_root/bad.pgn"
expect_status 2 "$program" "$test_root/input.pgn"
expect_status 0 "$program" --help
help_a=$(cat "$test_root/stdout")
expect_status 0 "$program" -?
test "$help_a" = "$(cat "$test_root/stdout")" || fail "help aliases differ"

expect_status 2 "$program" "$test_root/input.pgn" "$test_root/sentinel.pgn"
test "$(cat "$test_root/sentinel.pgn")" = sentinel || fail "refused output was changed"
expect_status 0 "$program" --force "$test_root/input.pgn" "$test_root/sentinel.pgn"
expect_same "$test_root/expected-all.pgn" "$test_root/sentinel.pgn"
expect_status 2 "$program" "$test_root/input.pgn" "$test_root/input.pgn"
ln "$test_root/input.pgn" "$test_root/hardlink.pgn"
expect_status 2 "$program" "$test_root/input.pgn" "$test_root/hardlink.pgn"
ln -s "$test_root/input.pgn" "$test_root/symlink.pgn"
expect_status 2 "$program" "$test_root/input.pgn" "$test_root/symlink.pgn"

printf '\357\273\277[White "BOM"]\r\n[Result "*"]\r\n\r\n1. e4 *\r\n\r\n[White "Second"]\r\n[Result "*"]\r\n\r\n1. d4 *' >"$test_root/crlf.pgn"
"$program" "$test_root/crlf.pgn" "$test_root/crlf-reversed.pgn"
printf '\357\273\277[White "Second"]\r\n[Result "*"]\r\n\r\n1. d4 *\r\n\r\n[White "BOM"]\r\n[Result "*"]\r\n\r\n1. e4 *' >"$test_root/crlf-expected.pgn"
printf '\r\n\r\n' >>"$test_root/crlf-expected.pgn"
expect_same "$test_root/crlf-expected.pgn" "$test_root/crlf-reversed.pgn"

printf '\357\273\277[Event "Only"]\n[Result "*"]\n\n1. e4 *\n\n' \
    >"$test_root/bom-single.pgn"
"$program" "$test_root/bom-single.pgn" "$test_root/bom-single-reversed.pgn"
expect_same "$test_root/bom-single.pgn" "$test_root/bom-single-reversed.pgn"

"$program" --head 1 "$test_root/crlf.pgn" "$test_root/bom-head.pgn"
printf '\357\273\277[White "BOM"]\r\n[Result "*"]\r\n\r\n1. e4 *\r\n\r\n' \
    >"$test_root/bom-head-expected.pgn"
expect_same "$test_root/bom-head-expected.pgn" "$test_root/bom-head.pgn"

"$program" --tail 1 "$test_root/crlf.pgn" "$test_root/bom-tail.pgn"
printf '\357\273\277[White "Second"]\r\n[Result "*"]\r\n\r\n1. d4 *\r\n\r\n' \
    >"$test_root/bom-tail-expected.pgn"
expect_same "$test_root/bom-tail-expected.pgn" "$test_root/bom-tail.pgn"

"$program" --head 2 --skip 1 "$test_root/crlf.pgn" \
    "$test_root/bom-skip.pgn"
expect_same "$test_root/bom-tail-expected.pgn" "$test_root/bom-skip.pgn"

printf '[Event "Plain"]\n[Result "*"]\n\n1. e4 *' \
    >"$test_root/no-bom.pgn"
"$program" "$test_root/no-bom.pgn" "$test_root/no-bom-reversed.pgn"
if od -An -tx1 -N3 "$test_root/no-bom-reversed.pgn" | grep -q 'ef bb bf'; then
    fail "BOM was added to input that did not have one"
fi

: >"$test_root/bom-empty-sentinel.pgn"
printf 'sentinel\n' >"$test_root/bom-empty-sentinel.pgn"
expect_status 0 "$program" --force --head 0 "$test_root/crlf.pgn" \
    "$test_root/bom-empty-sentinel.pgn"
test "$(cat "$test_root/bom-empty-sentinel.pgn")" = sentinel || \
    fail "empty BOM selection changed existing output"

printf '[Event "Left"]\n[Result "*"]\n\n1. e4 *\n\n\n\n{boundary [Event "fake"] 1-0}\n\n[Event "Right"]\n[Result "*"]\n\n1. d4 *' >"$test_root/boundary-comment.pgn"
"$program" "$test_root/boundary-comment.pgn" "$test_root/boundary-comment-reversed.pgn"
printf '[Event "Right"]\n[Result "*"]\n\n1. d4 *\n\n[Event "Left"]\n[Result "*"]\n\n1. e4 *\n{boundary [Event "fake"] 1-0}' >"$test_root/boundary-comment-expected.pgn"
printf '\n\n' >>"$test_root/boundary-comment-expected.pgn"
expect_same "$test_root/boundary-comment-expected.pgn" "$test_root/boundary-comment-reversed.pgn"
printf '[White "Before"]\r\n[Result "*"]\r\n\r\n1. e4 *\r\n; boundary 1-0\r\n\r\n[White "After"]\r\n[Result "*"]\r\n\r\n1. d4 *' >"$test_root/crlf-boundary-comment.pgn"
"$program" "$test_root/crlf-boundary-comment.pgn" "$test_root/crlf-boundary-reversed.pgn"
printf '[White "After"]\r\n[Result "*"]\r\n\r\n1. d4 *\r\n\r\n[White "Before"]\r\n[Result "*"]\r\n\r\n1. e4 *\r\n; boundary 1-0' >"$test_root/crlf-boundary-expected.pgn"
printf '\r\n\r\n' >>"$test_root/crlf-boundary-expected.pgn"
expect_same "$test_root/crlf-boundary-expected.pgn" "$test_root/crlf-boundary-reversed.pgn"

printf '[Event "A"]\n[Result "*"]\n\n1. e4 *\n\n[Event "B"]\n[Result "*"]\n\n1. d4 *' \
    >"$test_root/game-whitespace.pgn"
"$program" "$test_root/game-whitespace.pgn" \
    "$test_root/game-whitespace-reversed.pgn"
printf '[Event "B"]\n[Result "*"]\n\n1. d4 *\n\n[Event "A"]\n[Result "*"]\n\n1. e4 *\n\n' \
    >"$test_root/game-whitespace-expected.pgn"
expect_same "$test_root/game-whitespace-expected.pgn" \
    "$test_root/game-whitespace-reversed.pgn"

printf '[Event "A"]\n[Result "*"]\n\n1. e4 *\n[Event "B"]\n[Result "*"]\n\n1. d4 *' \
    >"$test_root/no-source-separator.pgn"
expect_status 2 "$program" "$test_root/no-source-separator.pgn" \
    "$test_root/no-source-separator-reversed.pgn"
grep -q 'game 2, line 5: new tag block after a result requires a blank line' \
    "$test_root/stderr" || fail "missing blank-line error location"
test ! -e "$test_root/no-source-separator-reversed.pgn" || \
    fail "ambiguous game boundary published output"

printf '[Event "A"]\n\n1. e4 e5 3.\n[White "B"]\n[Result "*"]\n\n1. d4 *' \
    >"$test_root/incomplete-no-blank.pgn"
expect_parser_error 'pgn-reverse: game 2, line 4: tag pair after incomplete movetext requires a blank line before the next game' \
    "$test_root/incomplete-no-blank.pgn" \
    "$test_root/incomplete-no-blank-out.pgn"

printf '[Event "A"]\n[Result "*"]\n\n1. e4 *\n\n\n[Event "B"]\n[Result "*"]\n\n1. d4 *' \
    >"$test_root/two-empty-lines.pgn"
"$program" "$test_root/two-empty-lines.pgn" \
    "$test_root/two-empty-lines-reversed.pgn"
printf '[Event "B"]\n[Result "*"]\n\n1. d4 *\n\n[Event "A"]\n[Result "*"]\n\n1. e4 *\n\n' \
    >"$test_root/two-empty-lines-expected.pgn"
expect_same "$test_root/two-empty-lines-expected.pgn" \
    "$test_root/two-empty-lines-reversed.pgn"

printf '[Event "A"]\r\n[Result "*"]\r\n\r\n1. e4 *\r\n\r\n\r\n[Event "B"]\r\n[Result "*"]\r\n\r\n1. d4 *' \
    >"$test_root/two-empty-lines-crlf.pgn"
"$program" "$test_root/two-empty-lines-crlf.pgn" \
    "$test_root/two-empty-lines-crlf-reversed.pgn"
printf '[Event "B"]\r\n[Result "*"]\r\n\r\n1. d4 *\r\n\r\n[Event "A"]\r\n[Result "*"]\r\n\r\n1. e4 *\r\n\r\n' \
    >"$test_root/two-empty-lines-crlf-expected.pgn"
expect_same "$test_root/two-empty-lines-crlf-expected.pgn" \
    "$test_root/two-empty-lines-crlf-reversed.pgn"

"$program" --head 1 "$test_root/game-whitespace.pgn" \
    "$test_root/game-whitespace-head.pgn"
printf '[Event "A"]\n[Result "*"]\n\n1. e4 *\n\n' \
    >"$test_root/game-whitespace-head-expected.pgn"
expect_same "$test_root/game-whitespace-head-expected.pgn" \
    "$test_root/game-whitespace-head.pgn"
"$program" --tail 2 --skip 1 "$test_root/game-whitespace.pgn" \
    "$test_root/game-whitespace-tail-skip.pgn"
expect_same "$test_root/game-whitespace-head-expected.pgn" \
    "$test_root/game-whitespace-tail-skip.pgn"
"$program" --head 2 --skip 1 "$test_root/game-whitespace.pgn" \
    "$test_root/game-whitespace-head-skip.pgn"
printf '[Event "B"]\n[Result "*"]\n\n1. d4 *' \
    >"$test_root/game-whitespace-head-skip-expected.pgn"
printf '\n\n' >>"$test_root/game-whitespace-head-skip-expected.pgn"
expect_same "$test_root/game-whitespace-head-skip-expected.pgn" \
    "$test_root/game-whitespace-head-skip.pgn"

printf '[Event "A"]\n[Result "*"]\n\n1. e4 *\n\n\n\n\n[Event "B"]\n[Result "*"]\n\n1. d4 *' \
    >"$test_root/irregular-separator.pgn"
"$program" "$test_root/irregular-separator.pgn" \
    "$test_root/irregular-reversed.pgn"
printf '[Event "B"]\n[Result "*"]\n\n1. d4 *\n\n[Event "A"]\n[Result "*"]\n\n1. e4 *\n\n' \
    >"$test_root/irregular-expected.pgn"
expect_same "$test_root/irregular-expected.pgn" \
    "$test_root/irregular-reversed.pgn"
"$program" "$test_root/irregular-reversed.pgn" \
    "$test_root/irregular-twice.pgn"
test "$(wc -c <"$test_root/irregular-twice.pgn")" -le \
    "$(wc -c <"$test_root/irregular-reversed.pgn")" || \
    fail "repeated reverse grew separator whitespace"
if cmp -s "$test_root/irregular-separator.pgn" \
    "$test_root/irregular-twice.pgn"; then
    fail "irregular separators unexpectedly remained byte-identical"
fi
"$program" "$test_root/irregular-twice.pgn" "$test_root/irregular-three.pgn"
"$program" "$test_root/irregular-three.pgn" "$test_root/irregular-four.pgn"
expect_same "$test_root/irregular-twice.pgn" "$test_root/irregular-four.pgn"

printf '[White "Only"]\n[Result "*"]\n\n1. e4 *' >"$test_root/one.pgn"
"$program" "$test_root/one.pgn" "$test_root/one-out.pgn"
printf '[White "Only"]\n[Result "*"]\n\n1. e4 *\n\n' \
    >"$test_root/one-expected.pgn"
expect_same "$test_root/one-expected.pgn" "$test_root/one-out.pgn"

write_single_game()
{
    suffix=$1
    input_path=$2
    case "$suffix" in
        crlf*)
            printf '[Event "Only"]\r\n[Result "*"]\r\n\r\n1. e4 {keep (variation text)} *' \
                >"$input_path"
            ;;
        *)
            printf '[Event "Only"]\n[Result "*"]\n\n1. e4 {keep (variation text)} *' \
                >"$input_path"
            ;;
    esac
    case "$suffix" in
        none) ;;
        lf1) printf '\n' >>"$input_path" ;;
        lf2) printf '\n\n' >>"$input_path" ;;
        lf5) printf '\n\n\n\n\n' >>"$input_path" ;;
        crlf1) printf '\r\n' >>"$input_path" ;;
        crlf2) printf '\r\n\r\n' >>"$input_path" ;;
        crlf5) printf '\r\n\r\n\r\n\r\n\r\n' >>"$input_path" ;;
        mixed) printf ' \t\r\n \n\t' >>"$input_path" ;;
        *) fail "unknown single-game suffix case: $suffix" ;;
    esac
}

for suffix in none lf1 lf2 lf5 crlf1 crlf2 crlf5 mixed; do
    input_path="$test_root/single-$suffix.pgn"
    output_path="$test_root/single-$suffix-out.pgn"
    expected_path="$test_root/single-$suffix-expected.pgn"
    write_single_game "$suffix" "$input_path"
    case "$suffix" in
        crlf*)
            printf '[Event "Only"]\r\n[Result "*"]\r\n\r\n1. e4 {keep (variation text)} *\r\n\r\n' \
                >"$expected_path"
            ;;
        *)
            printf '[Event "Only"]\n[Result "*"]\n\n1. e4 {keep (variation text)} *\n\n' \
                >"$expected_path"
            ;;
    esac
    "$program" "$input_path" "$output_path"
    expect_same "$expected_path" "$output_path"
    rm -f "$test_root/single-head-out.pgn" "$test_root/single-tail-out.pgn"
    "$program" --head 1 "$input_path" "$test_root/single-head-out.pgn"
    expect_same "$expected_path" "$test_root/single-head-out.pgn"
    "$program" --tail 1 "$input_path" "$test_root/single-tail-out.pgn"
    expect_same "$expected_path" "$test_root/single-tail-out.pgn"
done

printf '[Event "A"]\n[Result "*"]\n\n1. e4 (1. d4) *\n\n[Event "B"]\n[Result "*"]\n\n1. d4 {comment} *\n\n' \
    >"$test_root/reverse-twice.pgn"
"$program" "$test_root/reverse-twice.pgn" "$test_root/reverse-once.pgn"
"$program" "$test_root/reverse-once.pgn" "$test_root/reverse-twice-out.pgn"
expect_same "$test_root/reverse-twice.pgn" "$test_root/reverse-twice-out.pgn"
test "$(grep -c '^\[Event ' "$test_root/reverse-once.pgn")" -eq 2 || \
    fail "complete reverse lost or concatenated games"

"$program" --head 1 "$test_root/reverse-twice.pgn" \
    "$test_root/multi-head-one.pgn"
test "$(grep -c '^\[Event ' "$test_root/multi-head-one.pgn")" -eq 1 || \
    fail "partial selection did not contain exactly one game"
printf '[Event "A"]\n[Result "*"]\n\n1. e4 (1. d4) *' \
    >"$test_root/multi-head-one-expected.pgn"
printf '\n\n' >>"$test_root/multi-head-one-expected.pgn"
expect_same "$test_root/multi-head-one-expected.pgn" \
    "$test_root/multi-head-one.pgn"

printf '[White "Broken"]\n[Result "*"]\n\n1. e4 )' >"$test_root/broken.pgn"
expect_parser_error 'pgn-reverse: game 1, line 4: unmatched closing RAV parenthesis' \
    "$test_root/broken.pgn" "$test_root/broken-out.pgn"
printf 'unchanged on failed force\n' >"$test_root/force-preserved.pgn"
expect_status 2 "$program" --force "$test_root/broken.pgn" "$test_root/force-preserved.pgn"
test "$(cat "$test_root/force-preserved.pgn")" = 'unchanged on failed force' || fail "failed force changed existing output"

"$program" --verbose --head 2 "$test_root/input.pgn" "$test_root/verbose.pgn" >"$test_root/stdout" 2>"$test_root/verbose.log"
test ! -s "$test_root/stdout" || fail "verbose mode wrote progress to stdout"
grep -q 'found 5 games; selected 2' "$test_root/verbose.log" || fail "verbose summary missing"

"$program" "$test_root/input.pgn" "$test_root/silent.pgn" >"$test_root/stdout" 2>"$test_root/stderr"
test ! -s "$test_root/stdout" && test ! -s "$test_root/stderr" || fail "normal success was not silent"
expect_status 2 "$program" "$test_root/input.pgn" "$test_root/missing-dir/out.pgn"
test ! -e "$test_root/missing-dir/out.pgn" || fail "failed publication created output"
if find "$test_root" -name '*.pgn-reverse.*' -print | grep -q .; then
    fail "temporary output remained after a failed publication"
fi

printf '[Event "A"]\n[Result "*"]\n\n1. e4 e5 2. Nf3 Nc6 3.\n\n\n[White "B"]\n[Result "1-0"]\n\n1. d4 d5 1-0' \
    >"$test_root/incomplete-before-tag.pgn"
"$program" "$test_root/incomplete-before-tag.pgn" \
    "$test_root/incomplete-before-tag-reversed.pgn"
printf '[White "B"]\n[Result "1-0"]\n\n1. d4 d5 1-0\n\n[Event "A"]\n[Result "*"]\n\n1. e4 e5 2. Nf3 Nc6 3.\n\n' \
    >"$test_root/incomplete-before-tag-expected.pgn"
expect_same "$test_root/incomplete-before-tag-expected.pgn" \
    "$test_root/incomplete-before-tag-reversed.pgn"

printf '[Event "A"]\n\n1. e4 {stopped after a comment}\n\n[Event "B"]\n[Result "*"]\n\n1. d4 *' \
    >"$test_root/incomplete-after-comment.pgn"
"$program" --verbose "$test_root/incomplete-after-comment.pgn" \
    "$test_root/incomplete-after-comment-out.pgn" \
    >"$test_root/stdout" 2>"$test_root/incomplete-after-comment.log"
grep -q 'found 2 games; selected 2' \
    "$test_root/incomplete-after-comment.log" || \
    fail "comment-ended incomplete game was not separated"
grep -F '{stopped after a comment}' \
    "$test_root/incomplete-after-comment-out.pgn" >/dev/null || \
    fail "comment on incomplete game was lost"

printf '[Event "A"]\r\n\r\n1. e4 e5 3.\r\n \t\r\n[White "B"]\r\n[Result "1-0"]\r\n\r\n1. d4 d5 1-0' \
    >"$test_root/incomplete-crlf.pgn"
"$program" "$test_root/incomplete-crlf.pgn" \
    "$test_root/incomplete-crlf-reversed.pgn"
printf '[White "B"]\r\n[Result "1-0"]\r\n\r\n1. d4 d5 1-0\r\n\r\n[Event "A"]\r\n\r\n1. e4 e5 3.\r\n\r\n' \
    >"$test_root/incomplete-crlf-expected.pgn"
expect_same "$test_root/incomplete-crlf-expected.pgn" \
    "$test_root/incomplete-crlf-reversed.pgn"

for incomplete_kind in full-move move-number comment; do
    case "$incomplete_kind" in
        full-move) movetext='1. e4 e5' ;;
        move-number) movetext='1. e4 e5 2.' ;;
        comment) movetext='1. e4 {unfinished game, valid comment}' ;;
    esac
    printf '[Event "Incomplete"]\n\n%s' "$movetext" \
        >"$test_root/incomplete-$incomplete_kind.pgn"
    printf '[Event "Incomplete"]\n\n%s\n\n' "$movetext" \
        >"$test_root/incomplete-$incomplete_kind-expected.pgn"
    "$program" "$test_root/incomplete-$incomplete_kind.pgn" \
        "$test_root/incomplete-$incomplete_kind-out.pgn"
    expect_same "$test_root/incomplete-$incomplete_kind-expected.pgn" \
        "$test_root/incomplete-$incomplete_kind-out.pgn"
done

cat >"$test_root/false-tag-contexts.pgn" <<'PGN'
[Event "A"]
[Result "*"]

1. e4 {multiline text
[Event "inside brace"]
still comment} 1-0
; [Event "inside semicolon"]
% [Event "inside escape"]

[White "B"]
[Result "*"]

1. d4 *
PGN
"$program" --verbose "$test_root/false-tag-contexts.pgn" \
    "$test_root/false-tag-contexts-out.pgn" \
    >"$test_root/stdout" 2>"$test_root/false-tag-contexts.log"
grep -q 'found 2 games; selected 2' \
    "$test_root/false-tag-contexts.log" || \
    fail "comment/escape tag text was treated as a game boundary"

cat >"$test_root/quoted-tag-text.pgn" <<'PGN'
[Event "literal [Event \"not a tag\"]"]
[Result "*"]

1. e4 *
PGN
"$program" --verbose "$test_root/quoted-tag-text.pgn" \
    "$test_root/quoted-tag-text-out.pgn" \
    >"$test_root/stdout" 2>"$test_root/quoted-tag-text.log"
grep -q 'found 1 games; selected 1' "$test_root/quoted-tag-text.log" || \
    fail "quoted tag text was treated as a game boundary"

cat >"$test_root/tag-inside-rav.pgn" <<'PGN'
[Event "A"]
[Result "*"]

1. e4 (
[Event "not a game"]

1... d5) *
PGN
expect_parser_error 'pgn-reverse: game 1, line 5: tag pair encountered inside a RAV variation' \
    "$test_root/tag-inside-rav.pgn" "$test_root/tag-inside-rav-out.pgn"

cat >"$test_root/unclosed-comment.pgn" <<'PGN'
[Event "A"]
[Result "*"]

1. e4 {not closed
PGN
expect_parser_error 'pgn-reverse: game 1, line 4: unterminated brace comment (opened here and reaches EOF)' \
    "$test_root/unclosed-comment.pgn" "$test_root/unclosed-comment-out.pgn"

printf 'before\n{ outer { nested }\n' >"$test_root/nested-comment.pgn"
expect_parser_error 'pgn-reverse: game 1, line 2: nested brace comment opener' \
    "$test_root/nested-comment.pgn" "$test_root/nested-comment-out.pgn"

printf '[Event "A"]\n[Result "*"]\n\n1. e4 *\n\n[Event broken]\n' \
    >"$test_root/bad-second-tag.pgn"
expect_parser_error 'pgn-reverse: game 2, line 6: malformed tag pair at a game boundary' \
    "$test_root/bad-second-tag.pgn" "$test_root/bad-second-tag-out.pgn"

printf '[Event broken]\n' >"$test_root/bad-first-tag.pgn"
expect_parser_error 'pgn-reverse: game 1, line 1: malformed PGN tag pair' \
    "$test_root/bad-first-tag.pgn" "$test_root/bad-first-tag-out.pgn"

printf '\357\273\277[Event broken]\r\n' >"$test_root/bad-bom-tag.pgn"
expect_parser_error 'pgn-reverse: game 1, line 1: malformed PGN tag pair' \
    "$test_root/bad-bom-tag.pgn" "$test_root/bad-bom-tag-out.pgn"

printf '[Event "A"]\r\n[Result "*"]\r\n\r\n1. e4 *\r\n\r\n[Event broken]\r\n' \
    >"$test_root/bad-second-tag-crlf.pgn"
expect_parser_error 'pgn-reverse: game 2, line 6: malformed tag pair at a game boundary' \
    "$test_root/bad-second-tag-crlf.pgn" \
    "$test_root/bad-second-tag-crlf-out.pgn"

printf '[Event "A"]\n[Result "*"]\n\n1. e4 (1. d4\n' \
    >"$test_root/unclosed-rav.pgn"
expect_parser_error 'pgn-reverse: game 1, line 5: EOF inside an unclosed RAV variation' \
    "$test_root/unclosed-rav.pgn" "$test_root/unclosed-rav-out.pgn"

cat >"$test_root/repaired-quotes.pgn" <<'PGN'
[Event "14. HZ Open\"]
[Site "Vlissingen NED\"]
[Date "2010.08.10"]
[Round "5"]
[White "Van Eijk, Maurice\"]
[Black "Stange, Sven\"]
[Result "0-1"]
[BlackElo "2116"]
[ECO "C34"]
[PlyCount "72"]

1. e4 e5 2. Nf3 Nc6 0-1

[Event "Next game"]
[Result "1-0"]

1. d4 d5 1-0
PGN
"$program" --verbose "$test_root/repaired-quotes.pgn" \
    "$test_root/repaired-quotes-reversed.pgn" \
    >"$test_root/stdout" 2>"$test_root/repaired-quotes.log"
grep -F 'warning: game 1, line 1: malformed closing quote in Event tag; preserving original text' \
    "$test_root/repaired-quotes.log" >/dev/null || \
    fail "repaired Event tag warning missing or has wrong location"
grep -F 'warning: game 1, line 2: malformed closing quote in Site tag; preserving original text' \
    "$test_root/repaired-quotes.log" >/dev/null || \
    fail "repaired Site tag warning missing or has wrong location"
grep -F 'warning: game 1, line 5: malformed closing quote in White tag; preserving original text' \
    "$test_root/repaired-quotes.log" >/dev/null || \
    fail "repaired White tag warning missing or points to the next line"
grep -F 'warning: game 1, line 6: malformed closing quote in Black tag; preserving original text' \
    "$test_root/repaired-quotes.log" >/dev/null || \
    fail "repaired Black tag warning missing or has wrong location"
grep -q 'recovered 4 malformed tag closing quote(s)' \
    "$test_root/repaired-quotes.log" || \
    fail "repaired tag summary missing"
cat >"$test_root/repaired-quotes-expected.pgn" <<'PGN'
[Event "Next game"]
[Result "1-0"]

1. d4 d5 1-0

[Event "14. HZ Open\"]
[Site "Vlissingen NED\"]
[Date "2010.08.10"]
[Round "5"]
[White "Van Eijk, Maurice\"]
[Black "Stange, Sven\"]
[Result "0-1"]
[BlackElo "2116"]
[ECO "C34"]
[PlyCount "72"]

1. e4 e5 2. Nf3 Nc6 0-1

PGN
expect_same "$test_root/repaired-quotes-expected.pgn" \
    "$test_root/repaired-quotes-reversed.pgn"

"$program" --head 1 "$test_root/repaired-quotes.pgn" \
    "$test_root/repaired-quotes-head.pgn"
cat >"$test_root/repaired-quotes-head-expected.pgn" <<'PGN'
[Event "14. HZ Open\"]
[Site "Vlissingen NED\"]
[Date "2010.08.10"]
[Round "5"]
[White "Van Eijk, Maurice\"]
[Black "Stange, Sven\"]
[Result "0-1"]
[BlackElo "2116"]
[ECO "C34"]
[PlyCount "72"]

1. e4 e5 2. Nf3 Nc6 0-1

PGN
expect_same "$test_root/repaired-quotes-head-expected.pgn" \
    "$test_root/repaired-quotes-head.pgn"

"$program" "$test_root/repaired-quotes.pgn" \
    "$test_root/repaired-quotes-silent.pgn" \
    >"$test_root/stdout" 2>"$test_root/stderr"
test ! -s "$test_root/stderr" || \
    fail "repaired tag was not silent without --verbose"

cat >"$test_root/valid-tag-escapes.pgn" <<'PGN'
[Event "An escaped \"quote\" inside the value"]
[Site "C:\\Chess"]
[White "A trailing slash\\"]
[Result "*"]

1. e4 *
PGN
"$program" --verbose "$test_root/valid-tag-escapes.pgn" \
    "$test_root/valid-tag-escapes-out.pgn" \
    >"$test_root/stdout" 2>"$test_root/valid-tag-escapes.log"
if grep -q 'warning: game' "$test_root/valid-tag-escapes.log"; then
    fail "valid escaped quotes or backslashes were reported as malformed"
fi
printf '[Event "An escaped \\"quote\\" inside the value"]\n[Site "C:\\\\Chess"]\n[White "A trailing slash\\\\"]\n[Result "*"]\n\n1. e4 *\n\n' \
    >"$test_root/valid-tag-escapes-expected.pgn"
expect_same "$test_root/valid-tag-escapes-expected.pgn" \
    "$test_root/valid-tag-escapes-out.pgn"

printf '[Event "CRLF"]\r\n[White "Broken\\"]\r\n[Result "*"]\r\n\r\n1. e4 *' \
    >"$test_root/repaired-crlf.pgn"
"$program" --verbose "$test_root/repaired-crlf.pgn" \
    "$test_root/repaired-crlf-out.pgn" \
    >"$test_root/stdout" 2>"$test_root/repaired-crlf.log"
grep -F 'warning: game 1, line 2: malformed closing quote in White tag; preserving original text' \
    "$test_root/repaired-crlf.log" >/dev/null || \
    fail "CRLF repair used the wrong source line"
printf '[Event "CRLF"]\r\n[White "Broken\\"]\r\n[Result "*"]\r\n\r\n1. e4 *\r\n\r\n' \
    >"$test_root/repaired-crlf-expected.pgn"
expect_same "$test_root/repaired-crlf-expected.pgn" \
    "$test_root/repaired-crlf-out.pgn"

printf '[Event "A"]\n[White "ambiguous\\"] trailing\n[Result "*"]\n\n1. e4 *' \
    >"$test_root/ambiguous-terminal-quote.pgn"
expect_parser_error 'pgn-reverse: game 1, line 2: malformed PGN tag pair' \
    "$test_root/ambiguous-terminal-quote.pgn" \
    "$test_root/ambiguous-terminal-quote-out.pgn"

{
    index=1
    while test "$index" -le 12; do
        printf '[X%02d "bad\\"]\n' "$index"
        index=$((index + 1))
    done
    printf '[Result "*"]\n\n1. e4 *\n'
} >"$test_root/many-repaired-tags.pgn"
"$program" --verbose "$test_root/many-repaired-tags.pgn" \
    "$test_root/many-repaired-tags-out.pgn" \
    >"$test_root/stdout" 2>"$test_root/many-repaired-tags.log"
test "$(grep -c 'warning: game' "$test_root/many-repaired-tags.log")" -eq 10 || \
    fail "verbose tag warnings were not capped at ten"
grep -q '2 additional repaired tag warning(s) suppressed' \
    "$test_root/many-repaired-tags.log" || \
    fail "suppressed warning count is incorrect"
grep -q 'recovered 12 malformed tag closing quote(s)' \
    "$test_root/many-repaired-tags.log" || \
    fail "total repaired tag count is incorrect"

cat >"$test_root/recovery-contexts.pgn" <<'PGN'
[Event "Comment test"]
[Result "*"]

1. e4 { [White "not a real tag\"] }
% [Black "not a tag either\"]
PGN
"$program" --verbose "$test_root/recovery-contexts.pgn" \
    "$test_root/recovery-contexts-out.pgn" \
    >"$test_root/stdout" 2>"$test_root/recovery-contexts.log"
if grep -q 'warning: game' "$test_root/recovery-contexts.log"; then
    fail "malformed tag-like text in a comment triggered recovery"
fi

cat >"$test_root/recovery-rav.pgn" <<'PGN'
[Event "RAV test"]
[Result "*"]

1. e4 (
[White "not a game\"]
1... d5) *
PGN
expect_parser_error 'pgn-reverse: game 1, line 5: tag pair encountered inside a RAV variation' \
    "$test_root/recovery-rav.pgn" "$test_root/recovery-rav-out.pgn"
if grep -q 'warning: game' "$test_root/stderr"; then
    fail "malformed tag-like text in a RAV triggered recovery"
fi

echo "pgn-reverse CLI and PGN tests passed"
