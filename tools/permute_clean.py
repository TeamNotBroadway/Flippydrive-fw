#!/usr/bin/env python3
"""Strip permuter noise: temp blocks, !-chains; __builtin_expect becomes likely()/unlikely().

    python3 tools/permute_clean.py FILE...

Rewrites in place.
"""
import re, sys

TEMP = re.compile(r"\{ __typeof__\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\) t\d+ = (.*?); (.+?) = t\d+; \}")
NOTS = re.compile(r"(?<!!)(!\(){2,}")


def balanced(s, i):
    """Index just past the parenthesised group starting at s[i] == '('."""
    d = 0
    for j in range(i, len(s)):
        d += s[j] == "("
        d -= s[j] == ")"
        if d == 0:
            return j + 1
    raise ValueError


def strip_expect(s):
    out = ""
    while True:
        i = s.find("__builtin_expect(!!(")
        if i < 0:
            return out + s
        inner_start = i + len("__builtin_expect(!!")
        inner_end = balanced(s, inner_start)
        call_end = balanced(s, i + len("__builtin_expect"))
        hint = s[inner_end:call_end - 1].strip(" ,")
        name = {"0": "unlikely", "1": "likely"}.get(hint)
        out += s[:i] + (name if name else "") + s[inner_start:inner_end]
        s = s[call_end:]


def strip_nots(s):
    while True:
        m = NOTS.search(s)
        if not m:
            return s
        n = (m.end() - m.start()) // 2
        # count the full run, then remove pairs of "!(" ... ")"
        start = m.start()
        k = 0
        while s.startswith("!(", start + 2 * k):
            k += 1
        end = balanced(s, start + 2 * k - 1)  # past innermost group
        inner = s[start + 2 * k:end - 1]
        tail_close = end + (k - 1)
        assert s[end:tail_close] == ")" * (k - 1), "unbalanced not-chain"
        rep = ("!(" + inner + ")") if k % 2 else ("(" + inner + ")")
        s = s[:start] + rep + s[tail_close:]


def clean(s):
    s = TEMP.sub(lambda m: f"{m.group(2)} = {m.group(1)};", s)
    s = strip_expect(s)  # -> likely()/unlikely()
    s = strip_nots(s)
    return s


for p in sys.argv[1:]:
    t = open(p).read()
    open(p, "w", newline="\n").write(clean(t))
