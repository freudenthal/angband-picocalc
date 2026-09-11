#!/usr/bin/env python3
"""Re-wrap lib/help/*.txt so that no line exceeds 64 columns (stage 060).

    cd angband-pico && python tools/rewrap-help.py lib/help/*.txt
    cd angband-pico && python tools/rewrap-help.py --check lib/help/*.txt

The port's term is 64x32 (specifications.md section 4).  ui-output.c's file browser does
not wrap: Term_putstr() clips at Term->wid and reports nothing, so anything past column 63
is simply gone.  Two kinds of over-long line occur in the vendored help, and each gets a
different treatment.  Everything already within 64 columns is left byte-identical, so the
script is idempotent and a second run is a no-op.

1.  Prose.  A run of consecutive unindented text lines is one paragraph.  If any line in it
    is over 64 columns the whole paragraph is re-flowed at 64; otherwise it is untouched.

2.  Indented key/description tables -- the two-column command summaries in commands.txt and
    r_comm.txt,

        a    Aim a wand                      A    Activate an object

    and the three-column browser-command table in index.txt and r_index.txt -- become one
    entry per line.  They cannot stay in two columns: the longest left description is 29
    characters and the longest right one 28, and 2 + 5 + 29 + 2 + 5 + 28 is 71, so no
    single position for the right-hand column fits every row.  Shortening the wording would
    fit, but it would mean rewriting upstream's help text to save vertical space in a pager
    that scrolls anyway.  The split is lossless; that is why it was taken.

    A table is parsed as a *block*, not a line at a time, because the column boundaries are
    only visible across the whole block: in index.txt the key is separated from its
    description by a single space on the "SPACE" row and by five on the "#" row, and only
    the column that is blank in every row of the block is a real boundary.  A block whose
    rows are all within 64 columns is left alone.

Only lib/help/ is in scope.  lib/screens/*.txt is art and is trimmed by hand.
"""

import re
import sys

WIDTH = 64
MAX_KEY = 5          # "SPACE" is the longest key in these files


def is_prose(line):
    """True if the line belongs to a re-flowable paragraph."""
    if not line or line[0].isspace():
        return False
    if re.match(r"^[=*~^-]{3,}\s*$", line):      # an RST underline
        return False
    if line.startswith(".."):                    # an RST directive, e.g. ".. menu::"
        return False
    return True


def is_table_row(line):
    return line.startswith("  ") and line.strip() != ""


def wrap(text, width):
    """Greedy wrap that keeps the blank run between two words when they stay together.

    Upstream's help puts two spaces after a full stop.  Splitting on whitespace and
    re-joining with one would rewrite every sentence boundary in the file for no reason,
    so the separators are carried along and only the one at a line break is dropped.
    """
    parts = [p for p in re.split(r"( +)", text.strip()) if p != ""]
    out, cur, prev_sep = [], "", ""
    i = 0
    while i < len(parts):
        word = parts[i]
        sep = parts[i + 1] if i + 1 < len(parts) else ""
        i += 2
        if not cur:
            cur = word
        elif len(cur) + len(prev_sep) + len(word) <= width:
            cur += prev_sep + word
        else:
            out.append(cur)
            cur = word
        prev_sep = sep
    if cur:
        out.append(cur)
    return out


def column_cells(block):
    """Split each row of a table block on the runs of blanks common to every row.

    Returns a list of per-row cell lists, or None if the block does not look like a
    column table.
    """
    width = max(len(l) for l in block)

    def blank_everywhere(c):
        return all(c >= len(l) or l[c] == " " for l in block)

    # Separator runs: two or more adjacent columns blank in every row.  The leading
    # indent is a separator too, which is why the scan starts at column 0.
    seps, run = [], []
    for c in range(width + 1):
        if c < width and blank_everywhere(c):
            run.append(c)
        else:
            if len(run) >= 2:
                seps.append((run[0], run[-1] + 1))
            run = []

    rows = []
    for line in block:
        cells, prev = [], 0
        for a, b in seps:
            piece = line[prev:a].strip()
            if piece:
                cells.append(piece)
            prev = b
        piece = line[prev:].strip()
        if piece:
            cells.append(piece)
        rows.append(cells)

    if any(not c for c in rows):
        return None
    return rows


def pairs_from_cells(cells):
    """Turn one row's cells into (key, description) pairs, or None.

    A row can be either shape, and index.txt is both at once: its first column separates
    the key from its description by a single space, which is not a column boundary, while
    its other two columns use two spaces, which is.  So each cell is taken as a whole
    pair when it already contains blanks, and as a bare key -- consuming the cell after
    it -- when it does not.
    """
    out = []
    i = 0
    while i < len(cells):
        c = cells[i]
        head = re.split(r"\s+", c, 1)
        if len(head) == 2 and len(head[0]) <= MAX_KEY:
            out.append((head[0], head[1]))
            i += 1
        elif i + 1 < len(cells) and len(c) <= MAX_KEY:
            out.append((c, cells[i + 1]))
            i += 2
        else:
            return None
    return out


def split_table(block):
    """One line per key/description pair for a whole table block, or None."""
    rows = column_cells(block)
    if not rows:
        return None

    parsed = [pairs_from_cells(c) for c in rows]
    if any(p is None for p in parsed):
        return None
    if all(len(p) < 2 for p in parsed):
        return None                      # already one entry per line

    kw = max(len(k) for p in parsed for k, _ in p) + 1
    out = []
    for p in parsed:
        for k, d in p:
            out.append("  %-*s%s" % (kw, k, d))
    return out


def run_at(lines, i, pred):
    run = []
    while i < len(lines) and pred(lines[i].rstrip()):
        run.append(lines[i].rstrip())
        i += 1
    return run


def rewrap(text):
    lines = text.split("\n")
    out = []
    i = 0
    while i < len(lines):
        line = lines[i].rstrip()

        if is_prose(line):
            para = run_at(lines, i, is_prose)
            i += len(para)
            if any(len(l) > WIDTH for l in para):
                out.extend(wrap(" ".join(l.strip() for l in para), WIDTH))
            else:
                out.extend(para)
            continue

        if is_table_row(line):
            block = run_at(lines, i, is_table_row)
            i += len(block)
            if any(len(l) > WIDTH for l in block):
                split = split_table(block)
                if split:
                    out.extend(split)
                    continue
            out.extend(block)
            continue

        out.append(line)
        i += 1

    return "\n".join(out)


def main(argv):
    check = "--check" in argv
    paths = [a for a in argv if a != "--check"]

    if not paths:
        print(__doc__)
        return 2

    bad = 0
    for p in paths:
        with open(p, encoding="utf-8") as f:
            text = f.read()

        new = text if check else rewrap(text)

        over = [(n + 1, len(l)) for n, l in enumerate(new.split("\n")) if len(l) > WIDTH]
        for n, w in over:
            print("%s:%d: %d columns" % (p, n, w))
        bad += len(over)

        if check:
            continue

        if new != text:
            with open(p, "w", encoding="utf-8", newline="") as f:
                f.write(new)
            print("rewrap-help: %s rewritten" % p)
        else:
            print("rewrap-help: %s unchanged" % p)

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
