# PGN tools

This project contains three small programs for working with chess-game
collections in PGN format.

`pgn-reverse` changes the order of games while keeping each game's moves,
comments, and variations together. Reverse a complete file, or select games
from its beginning or end to make a smaller file or bring recent games to the
top. The input file is left unchanged.

`pgn-count` reports the number of games in one or more files and can also
summarize their results.

`pgn-list` creates a compact, one-line overview of each game, including the
players, ratings, site, year, ECO code, and result.

## Build and install

The build needs a C99 compiler and POSIX file operations. No external library
is required.

```sh
make release
make test
make install PREFIX=/usr
```

The default compiler is `cc`; override it with `make CC=clang`. Strict C99
warnings are enabled. `make debug` and `make release` build debug and optimized
variants. `make clean` removes the executables and build directory.

The default install prefix is `/usr/local`. Set `PREFIX`, `BINDIR`, `MANDIR`,
or `DESTDIR` to change the install locations. The executables and their
manuals are installed under `$(BINDIR)` and `$(MANDIR)/man1`. `make uninstall`
removes all programs and manuals. After installation, read them with
`man pgn-reverse`, `man pgn-count`, and `man pgn-list`.

The manpages are `man/pgn-reverse.1`, `man/pgn-count.1`, and
`man/pgn-list.1`. The roff source is ASCII text. For plain text on systems
where grotty emits ANSI formatting, use
`groff -Tascii -man -P-c man/pgn-reverse.1 | col -b`.

## Command line

```text
pgn-reverse [OPTIONS] INPUT OUTPUT
```

`INPUT` is the existing PGN file. `OUTPUT` is the separate file to write.
Both are required except when requesting help. `--help` and `-?` show usage
without opening a PGN file.

| Option | Meaning |
| --- | --- |
| `--head N` | Take the first N games and write them in reverse order. |
| `--tail N` | Take the last N games and write them in reverse order. |
| `--skip M` | Skip part of a `--head` or `--tail` selection before reversing it. |
| `--force` | Replace an existing regular output file after successful processing. |
| `--verbose` | Print brief progress to stderr. |
| `--help`, `-?` | Print usage and examples. |

Counts are unsigned decimal integers and may be zero. `--head` and `--tail`
cannot be combined. `--skip` requires exactly one of them. With no selection
option, all games are selected.

## PGN content and boundaries

Tags, player names, moves, comments, and variations are kept; moves inside a
game are not reversed. The output separators are normalized so each written
game, including the last, ends with one empty line. This is separator
normalization, not full PGN canonicalization: games are not parsed and written
back in a new format.

The scanner recognizes tag pairs, brace and semicolon comments, nested
variations, PGN escape lines beginning with `%` in column one, and result
markers. A missing result marker is accepted when the game ends at EOF or a
blank physical line clearly separates it from a valid new tag section. No
result or missing move is invented. The scanner does not check whether the
moves are legal chess moves. It refuses malformed or ambiguous structures
rather than guessing where a game ends. Parser errors include the one-based
game number and physical input line.

One recognizable tag defect is tolerated for boundary detection: a tag's
closing quote is escaped by a single backslash immediately before its final
`]`, with only optional whitespace before the end of the line. The original
tag bytes are still copied unchanged. Normal escaped quotes and backslashes
remain valid. With `--verbose`, the first ten such repairs are reported and
the program summarizes any further repairs; without it, successful processing
remains silent.

Comments after a result and before the next game stay with the preceding game.
A UTF-8 byte-order mark, if present, is kept at the beginning. Whitespace
before the first game stays at the beginning on a complete reversal; partial
selections omit file-level prefix bytes other than the BOM. Comments before
the first record stay with that first game.

## Tests and exit status

Run the non-graphical regression suite with `make test`. It exercises selection
rules, malformed input, byte preservation, line endings, BOM handling, and
overwrite safety. Successful runs are silent unless `--verbose` is used.

Exit status is `0` for success, including an empty selection, and `2` for
invocation, input, parsing, or output errors.

## License

Copyright (C) 2026 Bert Gijsbers. The programs are licensed under the GNU
General Public License, version 3 only. See `COPYING` for the license text.
