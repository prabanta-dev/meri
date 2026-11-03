#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
"""ops_table.py - the table of the instructions of the Meri VM, in Markdown.

Usage: ops_table.py [-o FILE] [--check FILE] [--root DIR]

Reads src/vm/ops.def (the number of an instruction is the order of its
MERI_OP line) and, from src/vm/code.h, the formats (enum meri_fmt, with
their comments) and the words of each (the switch of meri_fmt_words).
Writes one table for each group of ops.def (a group begins after a blank
line); a comment of its own line opens a part of the group and is written
before it, the comment at the end of a MERI_OP line is the meaning of
that instruction.

  -o FILE       write the table there instead of to standard output
  --check FILE  write nothing: exit 1 if FILE is not what would be
                written (a net of the documents), 0 if it is
  --root DIR    the root of Meri (by default, the parent of tools/)

Exit status: 0, 1 for a FILE that differs, 2 for a source not understood.
"""
import os
import re
import sys

OP = re.compile(r'MERI_OP\((\w+),\s*"([^"]*)",\s*MERI_FMT_(\w+)\)\s*(.*)$')


def die(msg):
    sys.stderr.write("ops_table: %s\n" % msg)
    sys.exit(2)


def comment_text(lines):
    """the words of a C comment given as its lines"""
    text = " ".join(lines)
    text = re.sub(r"^\s*/\*+", "", text)
    text = re.sub(r"\*+/\s*$", "", text)
    text = re.sub(r"\s*\n?\s*\*(?!/)\s", " ", text)
    return re.sub(r"\s+", " ", text).strip()


def cell(text):
    return text.replace("|", "\\|")


def read_formats(path):
    """[(name, comment)] in the order of enum meri_fmt, and {name: words}"""
    src = open(path, encoding="utf-8").read()
    m = re.search(r"enum meri_fmt \{(.*?)\};", src, re.S)
    if not m:
        die("%s: no enum meri_fmt" % path)
    formats, pending = [], []
    for line in m.group(1).split("\n"):
        s = line.strip()
        if not s:
            continue
        f = re.match(r"MERI_FMT_(\w+),?\s*(/\*.*\*/)?\s*$", s)
        if f:
            note = comment_text(pending + ([f.group(2)] if f.group(2) else []))
            formats.append((f.group(1), note))
            pending = []
        elif s.startswith("/*") or s.startswith("*") or pending:
            pending.append(s)
        else:
            die("%s: not understood in enum meri_fmt: %s" % (path, s))
    m = re.search(r"meri_fmt_words\(unsigned fmt\)\s*\{(.*?)\n\}", src, re.S)
    if not m:
        die("%s: no meri_fmt_words" % path)
    words, cases = {}, []
    default = None
    for line in m.group(1).split("\n"):
        s = line.strip()
        c = re.match(r"case MERI_FMT_(\w+):$", s)
        r = re.match(r"return (\d+);$", s)
        if c:
            cases.append(c.group(1))
        elif s == "default:":
            cases.append(None)
        elif r:
            for c in cases:
                if c is None:
                    default = int(r.group(1))
                else:
                    words[c] = int(r.group(1))
            cases = []
    if default is None:
        die("%s: meri_fmt_words has no default" % path)
    known = {n for n, _ in formats}
    for n in words:
        if n not in known:
            die("%s: meri_fmt_words names MERI_FMT_%s, not a format" % (path, n))
    return formats, {n: words.get(n, default) for n in known}


def read_ops(path):
    """[(note or None, [ops])] for each group, an op (number, name, text,
    format, meaning)"""
    lines = open(path, encoding="utf-8").read().split("\n")
    i, n = 0, len(lines)
    # past the comment at the head of the file, the first to end alone
    while i < n and lines[i].strip() != "*/":
        i += 1
    if i == n:
        die("%s: no comment at the head" % path)
    i += 1
    groups, parts, number = [], None, 0
    comment = None
    while i < n:
        s = lines[i].strip()
        i += 1
        if comment is not None:
            comment.append(s)
            if "*/" in s:
                parts.append((comment_text(comment), []))
                comment = None
            continue
        if not s:
            parts = None
            continue
        if parts is None:
            parts = []
            groups.append(parts)
        if s.startswith("/*"):
            comment = [s]
            if "*/" in s:
                parts.append((comment_text(comment), []))
                comment = None
            continue
        m = OP.match(s)
        if not m:
            die("%s:%d: not understood: %s" % (path, i, s))
        rest = m.group(4)
        if rest and not (rest.startswith("/*") and rest.endswith("*/")):
            die("%s:%d: a comment that does not end on its line" % (path, i))
        if not parts:
            parts.append((None, []))
        parts[-1][1].append((number, m.group(1), m.group(2), m.group(3),
                             comment_text([rest]) if rest else ""))
        number += 1
    if comment is not None:
        die("%s: a comment that does not end" % path)
    return groups, number


def table(root):
    ops_def = os.path.join(root, "src", "vm", "ops.def")
    code_h = os.path.join(root, "src", "vm", "code.h")
    formats, words = read_formats(code_h)
    groups, count = read_ops(ops_def)
    if count > 256:
        die("%d instructions: an opcode is 8 bits" % count)
    out = []
    w = out.append
    w("# The instructions of the Meri VM")
    w("")
    w("Generated by `tools/ops_table.py` from `src/vm/ops.def` and "
      "`src/vm/code.h`: do not edit by hand. %d instructions; the number "
      "is the opcode, the low byte of the first word." % count)
    w("")
    w("A register is R[x] of the window of the running function; K[x] is "
      "the constant x of the function. Words: the 32-bit words of the "
      "instruction, the first included, as meri_fmt_words gives them; the "
      "calls of `ABxN` and `ABxND` have the words of their arguments after "
      "these (meri_n_words).")
    w("")
    w("## Formats")
    w("")
    w("| Format | Words | Note |")
    w("|---|---|---|")
    for name, note in formats:
        w("| `%s` | %d | %s |" % (name, words[name], cell(note)))
    used = {op[3] for g in groups for _, ops in g for op in ops}
    for f in sorted(used - set(words)):
        die("an instruction of format MERI_FMT_%s, not in code.h" % f)
    for g in groups:
        ops = [op for _, part in g for op in part]
        if not ops:
            continue
        w("")
        w("## Opcodes %d-%d" % (ops[0][0], ops[-1][0]))
        for note, part in g:
            if not part:
                continue
            w("")
            if note:
                w(note + ("" if note.endswith(".") else "."))
                w("")
            w("| # | Opcode | Text | Format | Words | Meaning |")
            w("|---|---|---|---|---|---|")
            for number, name, text, fmt, meaning in part:
                w("| %d | `%s` | `%s` | `%s` | %d | %s |" %
                  (number, name, text, fmt, words[fmt], cell(meaning)))
    return "\n".join(out) + "\n"


def main(argv):
    out_path = check = None
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            sys.stdout.write(__doc__)
            return 0
        if a in ("-o", "--check", "--root") and i + 1 < len(argv):
            if a == "-o":
                out_path = argv[i + 1]
            elif a == "--check":
                check = argv[i + 1]
            else:
                root = argv[i + 1]
            i += 2
            continue
        sys.stderr.write("ops_table: unknown argument %s (see --help)\n" % a)
        return 2
    text = table(root)
    if check is not None:
        try:
            have = open(check, encoding="utf-8").read()
        except OSError as e:
            sys.stderr.write("ops_table: %s: %s\n" % (check, e.strerror))
            return 1
        if have != text:
            sys.stderr.write("ops_table: %s is not the table of ops.def\n" %
                             check)
            return 1
        return 0
    if out_path is not None:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
