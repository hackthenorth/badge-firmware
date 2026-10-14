#!/usr/bin/env python3
"""Assert every copy of bbx_proto.h declares the same protocol.

The badge and each station are separate ESP-IDF projects that cannot include
from one another, so bbx_proto.h is duplicated per project. Editing one copy
and not the others fails silently as "the badge never gets a response" —
docs/REMAINING-WORK.md lists it as a day-costing trap.

This compares the *code* of each copy, not the bytes. The copies deliberately
carry different prose (each points at the others), so a plain `diff` would fail
on day one and get ignored. Comments and blank lines are stripped; everything
else must match exactly.

Copies are discovered by glob, so a new project's copy is covered the moment it
lands — nothing to register here.

    python3 tools/check-bbx-proto.py          # exit 1 on drift
"""

import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SKIP_DIRS = {"build", "managed_components", "node_modules", ".git"}


def find_copies():
    out = []
    for p in sorted(REPO.rglob("bbx_proto.h")):
        if SKIP_DIRS.isdisjoint(part for part in p.relative_to(REPO).parts):
            out.append(p)
    return out


def strip_comments(src):
    """Remove // and /* */ comments, respecting string and char literals.

    A naive regex would corrupt any literal containing a slash. None exist
    today, but this file's whole job is catching silent drift.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            i = src.find("\n", i)
            if i < 0:
                break
        elif c == "/" and nxt == "*":
            end = src.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        elif c in "\"'":
            quote, j = c, i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == quote:
                    j += 1
                    break
                j += 1
            out.append(src[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def code_lines(path):
    stripped = strip_comments(path.read_text())
    return [ln.rstrip() for ln in stripped.splitlines() if ln.strip()]


def main():
    copies = find_copies()
    if len(copies) < 2:
        print(f"only {len(copies)} copy of bbx_proto.h found; nothing to compare")
        return 0

    ref = copies[0]
    ref_lines = code_lines(ref)
    failed = False

    for other in copies[1:]:
        other_lines = code_lines(other)
        if other_lines == ref_lines:
            continue
        failed = True
        print(f"DRIFT: {other.relative_to(REPO)} vs {ref.relative_to(REPO)}")
        import difflib

        diff = difflib.unified_diff(
            ref_lines, other_lines,
            fromfile=str(ref.relative_to(REPO)),
            tofile=str(other.relative_to(REPO)),
            lineterm="", n=2,
        )
        for line in diff:
            print("  " + line)

    if failed:
        print("\nThe copies must declare identical protocol. Prose may differ;")
        print("declarations may not. Sync them and re-run.")
        return 1

    names = ", ".join(str(p.relative_to(REPO)) for p in copies)
    print(f"ok: {len(copies)} copies agree ({len(ref_lines)} code lines) — {names}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
