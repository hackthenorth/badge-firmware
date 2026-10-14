#!/usr/bin/env python3
"""Drive TWO badges at once, for the multiplayer flows a single badge cannot test.

badge.py is deliberately one device: it takes one --port and refuses to guess
when two are plugged in. That is the right call for it. But pairing, a move
crossing the air, and the duty-cycle tiers are all statements about a *pair* of
badges, and checking them one badge at a time cannot see the interesting part.

This is not a fork. badge.py's Badge class takes an explicit path, its verb
handlers are free functions, and it has an `if __name__ == "__main__"` guard —
so everything below is a thin scheduler over the real thing. Fixes to the
transport land here for free.

    duet.py a:console "chess bump" b:console "chess bump" settle 2000 \
            a:console "chess" a:expect "state=Live" \
            b:console "chess" b:expect "state=Live" \
            agree session differ seat

Every verb except `settle` needs a target prefix — `a:`, `b:` or `both:`.
`settle` is a host-side sleep and is always global. The explicitness is the
point: in a two-badge test, "which badge" is the thing you are most likely to
get wrong, and a default would hide it.

Two verbs exist here that make no sense on one badge:

    agree <key>    both badges' last output has <key>=<value>, and they match
    differ <key>   ...and they do not

Those are what turn "the session ids are the same and the seats are opposite"
into an actual comparison rather than a substring match that passes by accident.
"""

import argparse
import contextlib
import glob
import io
import json
import os
import re
import shlex
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import badge as B  # noqa: E402


TARGETS = ("a", "b", "both")

# badge.py's verbs, plus the two cross-badge assertions.
VERBS = dict(B.VERBS)
VERBS.update({"agree": 1, "differ": 1})

# Verbs that are host-side only and therefore have no target.
GLOBAL_VERBS = {"settle", "agree", "differ"}


class Side:
    """One badge, plus the per-badge state badge.py's handlers expect to find
    on `args`. Each side needs its own `last_output` or `expect` on one badge
    would match the other badge's reply."""

    def __init__(self, label, path, opts):
        self.label = label
        self.path = path
        self.badge = B.Badge(path, debug=opts.debug)
        self.args = argparse.Namespace(
            timeout=opts.timeout,
            invert=opts.invert,
            out_dir=opts.out_dir,
            last_output=None,
        )

    def close(self):
        self.badge.close()


def is_verb_token(tok):
    return tok.split(":", 1)[-1] in VERBS


def parse_program(tokens, seen=frozenset()):
    """[(target|None, verb, arg), ...]. Mirrors badge.py's parse_program, with
    the target prefix split off first."""
    program = []
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        target, verb = (tok.split(":", 1) if ":" in tok else (None, tok))
        if target is not None and target not in TARGETS:
            raise SystemExit(
                f"unknown target {target!r} in {tok!r}; one of: {', '.join(TARGETS)}"
            )
        if verb not in VERBS:
            raise SystemExit(
                f"unknown verb {verb!r}; one of: {', '.join(sorted(VERBS))}"
            )

        need = VERBS[verb]
        arg = None
        if need == "?":
            # Same rule as badge.py: the next token is an argument only if it
            # is not itself a verb (prefixed or not).
            if i + 1 < len(tokens) and not is_verb_token(tokens[i + 1]):
                arg = tokens[i + 1]
                i += 1
        elif need:
            if i + 1 >= len(tokens):
                raise SystemExit(f"{verb} needs an argument")
            arg = tokens[i + 1]
            i += 1
        i += 1

        if verb == "run":
            program += read_script(arg, seen)
            continue

        if verb in GLOBAL_VERBS:
            if target is not None:
                raise SystemExit(f"{verb!r} is host-side; drop the {target!r} prefix")
        elif target is None:
            raise SystemExit(
                f"{verb!r} needs a target prefix: a:{verb}, b:{verb} or both:{verb}"
            )
        program.append((target, verb, arg))
    return program


def read_script(path, seen):
    real = os.path.realpath(path)
    if real in seen:
        raise SystemExit(f"run: {path} includes itself")
    seen = seen | {real}
    try:
        with open(path) as f:
            lines = f.readlines()
    except OSError as e:
        raise SystemExit(f"run: {e}")

    tokens = []
    for n, line in enumerate(lines, 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            tokens += shlex.split(line)
        except ValueError as e:
            raise SystemExit(f"{path}:{n}: {e}")
    return parse_program(tokens, seen)


def label_path(path, label):
    """`both:shot x.png` must not have the two badges overwrite each other."""
    head, tail = os.path.split(path)
    return os.path.join(head, f"{label}-{tail}")


def run_on(side, verb, arg, disambiguate):
    if verb in ("shot", "tree") and arg and disambiguate:
        arg = label_path(arg, side.label)
    # badge.py's handlers print straight to stdout. Capture and re-emit with a
    # label so interleaved two-badge output stays readable.
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            B.HANDLERS[verb](side.badge, arg, side.args)
    finally:
        for line in buf.getvalue().rstrip("\n").split("\n"):
            if line:
                print(f"[{side.label}] {line}")



def field(text, key):
    """Pull `key=value` out of a console dump. The badge's dumps are
    space-separated key=value throughout, which is exactly so this works.

    NOTE the value stops at the first space, so `fen=` reads back as only the
    piece placement. That is why the chess dump also carries a space-free
    `hash=` — compare that, not the FEN."""
    if not text:
        return None
    m = re.search(rf"(?:^|\s){re.escape(key)}=(\S+)", text)
    return m.group(1) if m else None


def do_compare(sides, key, want_equal):
    va = field(sides["a"].args.last_output, key)
    vb = field(sides["b"].args.last_output, key)
    missing = [lbl for lbl, v in (("a", va), ("b", vb)) if v is None]
    if missing:
        raise SystemExit(
            f"{key}: not present in the last output of "
            f"{' and '.join(missing)} — run a console/tree verb on both first"
        )
    same = va == vb
    verdict = "equal" if same else "differ"
    if same != want_equal:
        raise SystemExit(
            f"expected {key} to {'match' if want_equal else 'differ'}, "
            f"but a={va} b={vb} ({verdict})"
        )
    print(f"ok: {key} a={va} b={vb} ({verdict})")


def find_ports(a, b):
    if a and b:
        return a, b
    env_a = os.environ.get("BADGE_PORT_A")
    env_b = os.environ.get("BADGE_PORT_B")
    if not a:
        a = env_a
    if not b:
        b = env_b
    if a and b:
        return a, b

    candidates = sorted(
        glob.glob("/dev/tty.usbmodem*") + glob.glob("/dev/ttyACM*")
    )
    # Anything the caller pinned is not also a candidate for the other side.
    taken = {p for p in (a, b) if p}
    free = [p for p in candidates if p not in taken]
    if not a:
        a = free.pop(0) if free else None
    if not b:
        b = free.pop(0) if free else None
    if not a or not b:
        listing = "\n  ".join(candidates) or "(none)"
        raise SystemExit(
            "duet needs two badges. Found:\n  " + listing +
            "\nPass --port-a/--port-b, or set BADGE_PORT_A/BADGE_PORT_B."
        )
    if a == b:
        raise SystemExit(f"--port-a and --port-b are the same device: {a}")
    return a, b


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="verbs: " + "  ".join(sorted(VERBS)) +
               "\ntargets: a:  b:  both:   (settle/agree/differ take none)",
    )
    parser.add_argument("--port-a", help="serial device for badge A")
    parser.add_argument("--port-b", help="serial device for badge B")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="seconds to wait for a command (default 30)")
    parser.add_argument("--invert", action="store_true",
                        help="invert captured colours")
    parser.add_argument("--out-dir", default=".", metavar="DIR",
                        help="where shot/tree write their files (default .)")
    parser.add_argument("--debug", action="store_true",
                        help="echo raw console traffic to stderr")
    parser.add_argument("--json", metavar="PATH",
                        help="write both badges' final output as JSON, for "
                             "assertions too structured for `expect`")
    parser.add_argument("tokens", nargs="+", metavar="VERB")
    opts = parser.parse_args()

    program = parse_program(opts.tokens)
    if not os.path.isdir(opts.out_dir):
        raise SystemExit(f"--out-dir {opts.out_dir!r} is not a directory")

    path_a, path_b = find_ports(opts.port_a, opts.port_b)
    print(f"a: {path_a}\nb: {path_b}")

    sides = {}
    try:
        sides["a"] = Side("a", path_a, opts)
        sides["b"] = Side("b", path_b, opts)
        for s in sides.values():
            s.badge.sync()

        for target, verb, arg in program:
            if verb == "settle":
                B.do_settle(None, arg, None)
            elif verb in ("agree", "differ"):
                do_compare(sides, arg, verb == "agree")
            elif target == "both":
                # Sequential, not parallel. Two console writes land about 50 ms
                # apart over USB, which is comfortably inside the badge's
                # 500 ms bump window — so `both:console "chess bump"` exercises
                # the real mutual-bump path rather than bypassing it.
                for lbl in ("a", "b"):
                    run_on(sides[lbl], verb, arg, disambiguate=True)
            else:
                run_on(sides[target], verb, arg, disambiguate=False)

        if opts.json:
            blob = {lbl: s.args.last_output for lbl, s in sides.items()}
            with open(opts.json, "w") as f:
                json.dump(blob, f, indent=2)
            print(f"wrote {opts.json}")
    finally:
        for s in sides.values():
            s.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
