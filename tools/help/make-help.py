#!/usr/bin/env python3
"""Build the app's Help into C: data/help/help.md as topics of blocks of
styled spans (core/include/apple2help.h), which every frontend lays out with
its own toolkit.

    make-help.py --src data/help/help.md --out-c FILE
    make-help.py --src FILE --check

help.md is Markdown that GitHub shows as it is. Only what the frontends can
draw is taken; anything else fails the build, so the app and GitHub never
show different things:

  # Topic            one per sidebar entry; ## and ### are its sections
  paragraphs, "- " bullets (and "  - " under them), "1." steps,
  pipe tables, ``` blocks, "> " notes ("> [!NOTE]" and the like too)
  **bold**  *italic*  `code`  [text](#slug)  [text](https://...)
  <kbd>Ctrl+Shift+P</kbd>   one app shortcut: macOS shows its own (KBD_MAC)
  <kbd>Ctrl</kbd>+<kbd>C</kbd>   PC keys, the same everywhere
  <!-- only: macos -->...<!-- end -->   blocks for some frontends
                               (gnome kde windows macos linux)
  <!-- context: debugger/cpu -->      a fixed name for the next heading,
                                      which the frontends' Help buttons use

Headings are named by GitHub's slug of their text; [text](#slug) works in
both. Text before the first topic is GitHub's only.

Copyright (C) 2026 Thomas Cherryhomes
SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import os
import re
import sys

GNOME, KDE, WINDOWS, MACOS = 1, 2, 4, 8
ALL = GNOME | KDE | WINDOWS | MACOS
TAGS = {"gnome": GNOME, "kde": KDE, "windows": WINDOWS, "macos": MACOS,
        "linux": GNOME | KDE}

# Block and span kinds, in apple2help.h's order.
H2, H3, PARA, BULLET, BULLET2, NUMBERED, NOTE, PRE, TABLE_HEAD, TABLE_ROW = range(10)
BLOCK_NAMES = ("H2", "H3", "PARA", "BULLET", "BULLET2", "NUMBERED", "NOTE", "PRE",
               "TABLE_HEAD", "TABLE_ROW")
TEXT, BOLD, ITALIC, CODE, KBD, LINK, CELL = range(7)
SPAN_NAMES = ("TEXT", "BOLD", "ITALIC", "CODE", "KBD", "LINK", "CELL")

# The app's shortcuts as a Mac shows them (the Mac frontend's menus): Ctrl is
# Command there, and Help is the Help menu's own key.
KBD_MAC_FIXED = {
    "Ctrl+,": "⌘,",
    "Ctrl+Shift+H": "⌘?",
    "Shift+F8": "⇧F8",
    "Shift+F7": "⇧F7",
}

# A non-last table column wider than this squeezes the last one.
MAX_CELL = 30
MAX_STOPS_EM = 34

NOTE_MARKERS = ("[!NOTE]", "[!TIP]", "[!IMPORTANT]", "[!WARNING]", "[!CAUTION]")


class HelpError(Exception):
    pass


def kbd_mac(text, line):
    if "+" not in text:
        return None
    if text in KBD_MAC_FIXED:
        return KBD_MAC_FIXED[text]
    m = re.fullmatch(r"Ctrl\+([A-Z])", text)
    if m:
        return "⌘" + m.group(1)
    m = re.fullmatch(r"Ctrl\+Shift\+([A-Z])", text)
    if m:
        return "⌘⇧" + m.group(1)
    raise HelpError(f"line {line}: <kbd>{text}</kbd> has no Mac shortcut; "
                    "add it to KBD_MAC_FIXED, or write PC keys one <kbd> each")


def slug(text):
    """GitHub's anchor for a heading."""
    out = []
    for c in text.lower():
        if c.isalnum() or c in "-_":
            out.append(c)
        elif c == " ":
            out.append("-")
    return "".join(out)


# ---- inline ------------------------------------------------------------------

ESCAPABLE = "\\`*_[]()#+-.!|<>"
RAW_HTML = re.compile(r"</?[A-Za-z!]")
ENTITY = re.compile(r"&(#\d+|#x[0-9a-fA-F]+|[A-Za-z]+);")


def check_plain(text, line):
    """Text outside code: nothing GitHub would read as markup we don't draw."""
    if RAW_HTML.search(text):
        raise HelpError(f"line {line}: raw HTML in {text!r}: put it in `code`")
    if ENTITY.search(text):
        raise HelpError(f"line {line}: HTML entity in {text!r}")
    if re.search(r"(^|[\s(\"'])_\S", text) or re.search(r"\S_($|[\s).,;:!?\"'])", text):
        raise HelpError(f"line {line}: _emphasis_ in {text!r}: use *italic*")
    if re.search(r"https?://", text):
        raise HelpError(f"line {line}: bare URL in {text!r}: write [text](url)")
    if "~~" in text:
        raise HelpError(f"line {line}: strikethrough in {text!r}")


def inline(text, line, platforms):
    """Spans of one paragraph, cell or item."""
    spans = []
    buf = []

    def flush():
        if buf:
            t = "".join(buf)
            check_plain(t, line)
            spans.append([TEXT, t, None, None])
            buf.clear()

    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "\\":
            if i + 1 < n and text[i + 1] in ESCAPABLE:
                buf.append(text[i + 1])
                i += 2
                continue
            raise HelpError(f"line {line}: stray backslash in {text!r}")
        if c == "`":
            if text.startswith("``", i):
                raise HelpError(f"line {line}: `` code spans are not supported")
            j = text.find("`", i + 1)
            if j < 0:
                raise HelpError(f"line {line}: unclosed ` in {text!r}")
            flush()
            code = text[i + 1:j]
            if not code.strip():
                raise HelpError(f"line {line}: empty code span")
            spans.append([CODE, code, None, None])
            i = j + 1
            continue
        if text.startswith("<kbd>", i):
            j = text.find("</kbd>", i)
            if j < 0:
                raise HelpError(f"line {line}: unclosed <kbd>")
            flush()
            k = text[i + 5:j]
            if not k or "<" in k or "*" in k or "`" in k:
                raise HelpError(f"line {line}: <kbd>{k}</kbd> must be plain keys")
            mac = kbd_mac(k, line) if platforms & MACOS else None
            spans.append([KBD, k, mac, None])
            i = j + 6
            continue
        if c == "[":
            m = re.match(r"\[([^\]]+)\]\(([^)\s]+)\)", text[i:])
            if not m:
                raise HelpError(f"line {line}: [ that is not a link in {text!r} (escape it)")
            label, target = m.group(1), m.group(2)
            if re.search(r"[*`<\[\\]", label):
                raise HelpError(f"line {line}: markup inside link text {label!r}")
            if not (target.startswith("#") or re.match(r"(https?|mailto):", target)):
                raise HelpError(f"line {line}: link to {target!r}: only #slug or a URL")
            flush()
            spans.append([LINK, label, None, target])
            i += m.end()
            continue
        if c == "*":
            if text.startswith("***", i):
                raise HelpError(f"line {line}: *** is not supported")
            bold = text.startswith("**", i)
            mark = "**" if bold else "*"
            j = text.find(mark, i + len(mark))
            if not bold and j >= 0 and text.startswith("**", j):
                raise HelpError(f"line {line}: nested emphasis in {text!r}")
            if j < 0:
                raise HelpError(f"line {line}: unclosed {mark} in {text!r}")
            inner = text[i + len(mark):j]
            if not inner or inner != inner.strip():
                raise HelpError(f"line {line}: {mark} must hug its text in {text!r}")
            if re.search(r"[*`<\[\\]", inner):
                raise HelpError(f"line {line}: nested markup in {mark}{inner}{mark}")
            check_plain(inner, line)
            flush()
            spans.append([BOLD if bold else ITALIC, inner, None, None])
            i = j + len(mark)
            continue
        buf.append(c)
        i += 1
    flush()
    return spans


def plain(spans, mac=False):
    return "".join((s[2] if mac and s[2] else s[1]) if s[0] != CELL else " | "
                   for s in spans)


# ---- blocks ------------------------------------------------------------------

class Block:
    def __init__(self, kind, line, platforms):
        self.kind = kind
        self.line = line
        self.platforms = platforms
        self.text = []          # source lines
        self.spans = []
        self.number = 0
        self.id = None
        self.context = None
        self.cols = []          # tables: ems of each column but the last
        self.table = None       # tables: every row's spans


class Topic:
    def __init__(self, title, line, context):
        self.title = title
        self.line = line
        self.id = slug(title)
        self.context = context
        self.blocks = []


def table_cells(row, line):
    r = row.strip()
    if not (r.startswith("|") and r.endswith("|")):
        raise HelpError(f"line {line}: a table row starts and ends with |")
    cells, cur, i = [], [], 1
    while i < len(r) - 1:
        if r[i] == "\\" and r[i + 1] == "|":
            cur.append("|")     # GitHub unescapes it in code spans too
            i += 2
            continue
        if r[i] == "|":
            cells.append("".join(cur).strip())
            cur = []
        else:
            cur.append(r[i])
        i += 1
    cells.append("".join(cur).strip())
    return cells


def em_width(spans):
    w = 0.0
    for kind, text, mac, _ in spans:
        t = mac if mac and len(mac) > len(text) else text
        if kind in (CODE, KBD):
            w += 0.62 * len(t) + 0.5
        elif kind == BOLD:
            w += 0.58 * len(t)
        else:
            w += 0.52 * len(t)
    return w


def parse(src_text):
    lines = src_text.split("\n")
    topics = []
    topic = None
    block = None
    platforms = ALL
    in_only = None
    context = None
    numbered = 0
    pre = None

    def close():
        nonlocal block
        if block is None:
            return
        b = block
        block = None
        if b.kind in (TABLE_HEAD,):
            finish_table(b)
            return
        if b.kind == PRE:
            b.spans = [[TEXT, "\n".join(b.text), None, None]]
        else:
            joined = " ".join(t.strip() for t in b.text)
            b.spans = inline(joined, b.line, b.platforms)
        topic.blocks.append(b)

    def finish_table(b):
        rows = b.text
        if len(rows) < 2:
            raise HelpError(f"line {b.line}: a table needs a header and a |---| row")
        head = table_cells(rows[0], b.line)
        sep = table_cells(rows[1], b.line + 1)
        if len(sep) != len(head) or not all(re.fullmatch(r":?-{3,}:?", c) for c in sep):
            raise HelpError(f"line {b.line + 1}: the second table row is |---|---|")
        parsed = []
        for k, r in enumerate([rows[0]] + rows[2:]):
            ln = b.line + (0 if k == 0 else k + 1)
            cells = table_cells(r, ln)
            if len(cells) != len(head):
                raise HelpError(f"line {ln}: {len(cells)} cells, the header has {len(head)}")
            parsed.append((ln, [inline(c, ln, b.platforms) for c in cells]))
        ncols = len(head)
        if ncols < 2:
            raise HelpError(f"line {b.line}: a table has two columns or more")
        cols = []
        for c in range(ncols - 1):
            widest = 0.0
            for ln, cells in parsed:
                if len(plain(cells[c])) > MAX_CELL or len(plain(cells[c], True)) > MAX_CELL:
                    raise HelpError(f"line {ln}: {plain(cells[c])!r} is too wide for a "
                                    f"column that is not the last ({MAX_CELL} characters)")
                widest = max(widest, em_width(cells[c]))
            cols.append(int(widest + 2.5))
        if sum(cols) > MAX_STOPS_EM:
            raise HelpError(f"line {b.line}: the columns before the last are too wide "
                            f"({sum(cols)} em, at most {MAX_STOPS_EM})")
        for k, (ln, cells) in enumerate(parsed):
            rb = Block(TABLE_HEAD if k == 0 else TABLE_ROW, ln, b.platforms)
            rb.cols = cols
            for c, cs in enumerate(cells):
                if c:
                    rb.spans.append([CELL, "", None, None])
                rb.spans.extend(cs)
            topic.blocks.append(rb)

    for ln0, raw in enumerate(lines):
        ln = ln0 + 1
        line = raw.rstrip()
        if "\t" in raw:
            raise HelpError(f"line {ln}: a tab (use spaces)")

        if pre is not None:
            if line.strip() == "```":
                close()
                pre = None
            else:
                block.text.append(raw.rstrip())
            continue

        stripped = line.strip()
        if stripped.startswith("<!--"):
            m = re.fullmatch(r"<!--\s*(only|context|end)\s*:?\s*(.*?)\s*-->", stripped)
            if not m:
                raise HelpError(f"line {ln}: unknown comment {stripped!r}")
            if block is not None:
                raise HelpError(f"line {ln}: a blank line goes before {stripped!r}")
            word, arg = m.group(1), m.group(2)
            if word == "only":
                if in_only:
                    raise HelpError(f"line {ln}: only: inside the only: at line {in_only}")
                mask = 0
                for t in arg.split():
                    if t not in TAGS:
                        raise HelpError(f"line {ln}: unknown frontend {t!r}")
                    mask |= TAGS[t]
                if not mask:
                    raise HelpError(f"line {ln}: only: needs a frontend")
                platforms, in_only = mask, ln
            elif word == "end":
                if not in_only or arg:
                    raise HelpError(f"line {ln}: end without only:")
                platforms, in_only = ALL, None
            else:
                if not re.fullmatch(r"[a-z]+(-[a-z]+)*(/[A-Za-z]+)?", arg):
                    raise HelpError(f"line {ln}: bad context {arg!r}")
                context = (arg, ln)
            numbered = 0
            continue

        if not stripped:
            close()
            if context and ln0 + 1 < len(lines) and not lines[ln0 + 1].startswith("#"):
                raise HelpError(f"line {context[1]}: context: goes right above a heading")
            continue

        if re.fullmatch(r"=+|-+", stripped) and block is not None and block.kind == PARA:
            raise HelpError(f"line {ln}: setext headings are not supported (use #)")
        if re.fullmatch(r"(\*\s*){3,}|(-\s*){3,}|(_\s*){3,}", stripped):
            raise HelpError(f"line {ln}: horizontal rules are not supported")

        m = re.match(r"(#{1,6})\s+(.*)$", line)
        if m:
            close()
            level, title = len(m.group(1)), m.group(2).strip()
            if level > 3:
                raise HelpError(f"line {ln}: only #, ## and ###")
            if re.search(r"[*`<\[\]\\_]", title):
                raise HelpError(f"line {ln}: no markup in headings")
            if title.endswith("#"):
                raise HelpError(f"line {ln}: no closing # on headings")
            ctx = context[0] if context else None
            context = None
            numbered = 0
            if level == 1:
                if in_only:
                    raise HelpError(f"line {ln}: a topic inside only: (line {in_only})")
                topic = Topic(title, ln, ctx)
                topics.append(topic)
                continue
            if topic is None:
                raise HelpError(f"line {ln}: a section before the first # topic")
            b = Block(H2 if level == 2 else H3, ln, platforms)
            b.text = [title]
            b.spans = [[TEXT, title, None, None]]
            b.id = slug(title)
            b.context = ctx
            topic.blocks.append(b)
            continue
        if context:
            raise HelpError(f"line {context[1]}: context: goes right above a heading")

        if topic is None:
            continue    # GitHub's preamble

        if stripped.startswith("```"):
            close()
            if re.search(r"[`~]", stripped[3:]):
                raise HelpError(f"line {ln}: bad fence")
            block = Block(PRE, ln, platforms)
            pre = ln
            continue

        if line.startswith("    ") and (block is None or block.kind not in
                                        (BULLET, BULLET2, NUMBERED)):
            raise HelpError(f"line {ln}: indented code is not supported (use ```)")

        if stripped.startswith(">"):
            body = stripped[1:].strip()
            if block is None or block.kind != NOTE:
                close()
                block = Block(NOTE, ln, platforms)
                if body in NOTE_MARKERS:
                    continue
            if body.startswith("[!"):
                raise HelpError(f"line {ln}: {body!r} goes alone on a note's first line")
            if not body:
                raise HelpError(f"line {ln}: one paragraph per note")
            block.text.append(body)
            continue
        if block is not None and block.kind == NOTE:
            raise HelpError(f"line {ln}: every line of a note starts with >")

        if stripped.startswith("|"):
            if block is not None and block.kind == PARA:
                raise HelpError(f"line {ln}: a blank line goes before a table")
            if block is None or block.kind != TABLE_HEAD:
                close()
                block = Block(TABLE_HEAD, ln, platforms)
            block.text.append(stripped)
            continue
        if block is not None and block.kind == TABLE_HEAD:
            raise HelpError(f"line {ln}: a blank line goes after a table")

        m = re.match(r"( *)([-*+]|\d+[.)])\s+(.*)$", line)
        if m:
            indent, mark, body = len(m.group(1)), m.group(2), m.group(3)
            close()
            if mark in ("*", "+"):
                raise HelpError(f"line {ln}: bullets are -")
            if mark[0].isdigit():
                if indent:
                    raise HelpError(f"line {ln}: steps are not nested")
                if not mark.endswith("."):
                    raise HelpError(f"line {ln}: steps are 1. 2. 3.")
                numbered += 1
                if int(mark[:-1]) != numbered:
                    raise HelpError(f"line {ln}: step {mark} should be {numbered}.")
                block = Block(NUMBERED, ln, platforms)
                block.number = numbered
            else:
                if indent not in (0, 2):
                    raise HelpError(f"line {ln}: a bullet under a bullet is indented 2")
                if indent == 0:
                    numbered = 0
                block = Block(BULLET if indent == 0 else BULLET2, ln, platforms)
            block.text.append(body)
            continue

        if block is not None and block.kind in (BULLET, BULLET2, NUMBERED):
            if not line.startswith(" "):
                raise HelpError(f"line {ln}: a list item's next line is indented "
                                "(or put a blank line before a paragraph)")
            block.text.append(stripped)
            continue
        if block is None:
            numbered = 0
            block = Block(PARA, ln, platforms)
        elif block.kind != PARA:
            raise HelpError(f"line {ln}: a blank line goes before this")
        if line.endswith("\\") or raw.endswith("  "):
            raise HelpError(f"line {ln}: hard line breaks are not supported")
        block.text.append(stripped)

    if pre is not None:
        raise HelpError(f"line {pre}: unclosed ```")
    close()
    if in_only:
        raise HelpError(f"line {in_only}: only: without end")
    if context:
        raise HelpError(f"line {context[1]}: context: at the end")
    if not topics:
        raise HelpError("no # topics")
    return topics


def check(topics):
    ids, contexts = {}, {}
    for t in topics:
        for ident, ln in [(t.id, t.line)] + [(b.id, b.line) for b in t.blocks if b.id]:
            if not ident:
                raise HelpError(f"line {ln}: a heading with no letters")
            if ident in ids:
                raise HelpError(f"line {ln}: #{ident} is also line {ids[ident]}")
            ids[ident] = ln
        for ctx, ln in [(t.context, t.line)] + [(b.context, b.line) for b in t.blocks]:
            if ctx:
                if ctx in contexts:
                    raise HelpError(f"line {ln}: context {ctx} is also line {contexts[ctx]}")
                contexts[ctx] = ln
        if not t.blocks or t.blocks[0].kind in (H2, H3):
            raise HelpError(f"line {t.line}: a topic starts with a paragraph")
    for t in topics:
        for b in t.blocks:
            for s in b.spans:
                if s[0] == LINK and s[3].startswith("#") and s[3][1:] not in ids:
                    raise HelpError(f"line {b.line}: no heading for {s[3]}")
    return contexts


# ---- C -------------------------------------------------------------------------

def c_str(s):
    if s is None:
        return "NULL"
    out = ['"']
    col = 0
    for ch in s.encode("utf-8"):
        if ch == 0x22:
            piece = '\\"'
        elif ch == 0x5C:
            piece = "\\\\"
        elif ch == 0x0A:
            piece = "\\n"
        elif ch == 0x3F:
            piece = "\\?"
        elif 0x20 <= ch < 0x7F:
            piece = chr(ch)
        else:
            piece = "\\%03o" % ch
        if col > 70 and ch == 0x20:
            out.append('"\n        "')
            col = 0
        out.append(piece)
        col += len(piece)
    out.append('"')
    return "".join(out)


def emit(topics, src_name):
    out = [f"/* Generated by tools/help/make-help.py from {src_name} -- do not edit. */",
           '#include "help_data.h"', ""]
    for ti, t in enumerate(topics):
        for bi, b in enumerate(t.blocks):
            out.append(f"static const apple2help_span t{ti}b{bi}[] = {{")
            for kind, text, mac, target in b.spans:
                out.append(f"    {{ APPLE2HELP_{SPAN_NAMES[kind]}, {c_str(text)}, "
                           f"{c_str(mac)}, {c_str(target)} }},")
            out.append("};")
            if b.cols:
                out.append(f"static const unsigned char t{ti}b{bi}c[] = {{ "
                           + ", ".join(str(c) for c in b.cols) + " };")
        out.append(f"static const apple2help_block t{ti}[] = {{")
        for bi, b in enumerate(t.blocks):
            cols = f"t{ti}b{bi}c, {len(b.cols)}" if b.cols else "NULL, 0"
            out.append(f"    {{ APPLE2HELP_{BLOCK_NAMES[b.kind]}, {b.platforms}u, {b.number}, "
                       f"{c_str(b.id)}, {c_str(b.context)}, t{ti}b{bi}, {len(b.spans)}, "
                       f"{cols} }},")
        out.append("};")
    out.append(f"const apple2help_topic apple2help_topics_data[{len(topics)}] = {{")
    for ti, t in enumerate(topics):
        out.append(f"    {{ {c_str(t.id)}, {c_str(t.title)}, {c_str(t.context)}, "
                   f"t{ti}, {len(t.blocks)} }},")
    out.append("};")
    out.append(f"const int apple2help_topics_count = {len(topics)};")
    out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out-c")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if not args.out_c and not args.check:
        ap.error("--out-c or --check")
    with open(args.src, encoding="utf-8") as f:
        text = f.read()
    try:
        topics = parse(text)
        check(topics)
    except HelpError as e:
        print(f"{args.src}: {e}", file=sys.stderr)
        sys.exit(1)
    if args.check:
        return
    os.makedirs(os.path.dirname(os.path.abspath(args.out_c)), exist_ok=True)
    tmp = args.out_c + ".tmp"
    with open(tmp, "w", encoding="ascii", newline="\n") as f:
        f.write(emit(topics, "data/help/help.md"))
    os.replace(tmp, args.out_c)


if __name__ == "__main__":
    main()
