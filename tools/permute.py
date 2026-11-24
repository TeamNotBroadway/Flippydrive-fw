#!/usr/bin/env python3
"""Random source permuter for near-matching functions. Run under WSL.

    python3 tools/permute.py IMAGE SRC.c FUNC ADDR [--iters N] [--seed S] [--jobs J]

Mutates only FUNC's body in a copy of SRC.c, compiles it with the image's flags from
build.ninja, scores it with funcdiff.py (matched windows at ADDR) and hill-climbs. The best
variant goes to permute_out/FUNC.best.c with its score; SRC.c is never touched. A match
found this way still needs reading: a mutation can change behaviour and keep the bytes.
"""
import argparse, os, random, re, subprocess, sys, tempfile, multiprocessing as mp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = "/mnt/c/Tools/arch-arm-none-eabi-14.2.0/bin/arm-none-eabi-gcc"
BASE = {"main-ode": "0x10080000", "bootloader": "0x10000000"}


# A copy of the toolchain and SDK on the WSL filesystem (~/tc) compiles several times faster
# than reading them through /mnt/c.
FAST = os.path.expanduser("~/tc")
if os.path.isdir(FAST):
    CC = CC.replace("/mnt/c/Tools", FAST)
    os.environ.setdefault("OBJDUMP", f"{FAST}/arch-arm-none-eabi-14.2.0/bin/arm-none-eabi-objdump")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import callseq  # noqa: E402  (after OBJDUMP is set: funcdiff reads it on import)


def build_flags(img, srcpath=None):
    """Compile flags of SRCPATH's object in build.ninja, else of <img>/main.c.o."""
    nin = open(os.path.expanduser("~/fd-build/fw/build.ninja")).read().split("\n")
    src = " " + os.path.abspath(srcpath) if srcpath else None
    i = next((n for n, l in enumerate(nin) if src and l.startswith("build ")
              and l.split(" || ")[0].endswith(src)), None)
    if i is None:
        key = f"build {img}/CMakeFiles/flippydrive-{img}.dir/main.c.o"
        i = next(n for n, l in enumerate(nin) if l.startswith(key))
    out = {}
    for l in nin[i + 1:i + 8]:
        m = re.match(r"\s+(DEFINES|INCLUDES|FLAGS) = (.*)", l)
        if m:
            out[m.group(1)] = m.group(2)
    flags = " ".join(out.get(k, "") for k in ("DEFINES", "INCLUDES", "FLAGS"))
    return flags.replace("/mnt/c/Tools", FAST) if os.path.isdir(FAST) else flags


def find_body(src, func):
    """(start, end) of FUNC's body braces, end exclusive."""
    for m in re.finditer(r"\b%s\b\)?\s*\([^;{]*\)\s*\{" % re.escape(func), src):
        start = m.end() - 1
        depth = 0
        for j in range(start, len(src)):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    return start, j + 1
    raise SystemExit(f"{func} not found")


def score(img, flags, text, func, addr, tmpdir, srcdir):
    c = os.path.join(tmpdir, "p.c")
    o = os.path.join(tmpdir, "p.o")
    with open(c, "w") as f:
        f.write(text)
    r = subprocess.run(f"{CC} -I{srcdir} {flags} -w -c {c} -o {o}", shell=True, capture_output=True,
                       cwd=srcdir)
    if r.returncode:
        return None
    env = dict(os.environ, AT=f"{func}={addr}")
    binp = os.environ.get("BIN") or os.path.join(ROOT, f"original/extracted/flippydrive-{img}.bin")
    dist = callseq.check(binp, int(BASE[img], 0), o, func, int(addr, 0), img)[0]
    r = subprocess.run([sys.executable, "-I", "funcdiff.py", binp, BASE[img], o], env=env,
                       capture_output=True, text=True, cwd=os.path.join(ROOT, "tools"))
    for l in r.stdout.splitlines():
        p = l.split()
        if len(p) >= 4 and p[-1] == func and "/" in p[0]:
            a, b = p[0].split("/")
            return (int(a), int(b), "EXACT" in l and dist == 0, dist)
    return None


def key(sc):
    """Call order first (windows cannot see it), then exactness, then matched windows."""
    return (-sc[3], sc[2], sc[0])


# ---- mutations on the body text -------------------------------------------------------

STMT = re.compile(r"[^;{}]*;")
COMM = re.compile(r"(\b[\w\.\->\[\]]+\b)\s*(==|!=|\+|\*|&|\||\^)\s*(\b[\w\.\->\[\]]+\b)")
TYPES = ["int", "unsigned", "uint32_t", "int32_t", "uint"]   # same width only
CALL = re.compile(r"\b(?!(?:if|while|for|switch|return|sizeof)\b)\w+\s*\(")
MEMREAD = re.compile(r"->|\.\s*[A-Za-z_]|\[|(?:^|[=(,+\-*/&|!~<>?:])\s*\*")
STORE = re.compile(r"(->|\.\s*\w+|\]|^\s*\*[^=]*)\s*([+\-*/&|^]|<<|>>)?=(?!=)", re.M)
ASSIGN = re.compile(r"^\s*(?:[A-Za-z_][\w 	]*?[\s*]+)?\**([A-Za-z_]\w*)\s*(?:[+\-*/&|^]|<<|>>)?=(?!=)\s*(.*?);\s*$", re.S)
DECL = re.compile(r"^\s*(?!(?:return|goto|break|continue|case|else|do)\b)[A-Za-z_][\w 	]*[\s*]+\**[A-Za-z_]\w*\s*(\[[^\]]*\])?\s*;\s*$")
JUMP = re.compile(r"\b(return|break|continue|goto|case|default)\b|^\s*\w+\s*:(?!:)", re.M)
CWORDS = {"if", "else", "while", "for", "do", "return", "sizeof", "int", "unsigned", "char", "void",
          "const", "volatile", "struct", "uint", "bool", "true", "false", "NULL"}
LOCALS = set()   # parameters and locals of the function being permuted, set by worker()


def idents(t):
    return set(re.findall(r"\b[A-Za-z_]\w*\b", t)) - CWORDS


def movable(a, between):
    """True if statement A can be moved across BETWEEN without changing behaviour.

    Only declarations without initialisers, and assignments to locals with no calls, move.
    """
    if JUMP.search(between):
        return False
    if DECL.match(a) and not CALL.search(a):
        return True
    m = ASSIGN.match(a)
    if not m or CALL.search(a) or m.group(1) not in LOCALS:
        return False
    lhs, rhs = m.groups()
    if re.search(r"\b%s\b" % lhs, between):
        return False
    reads = idents(rhs)
    for r in reads & LOCALS:
        if re.search(r"(\b%s\b\s*([+\-*/&|^]|<<|>>)?=(?!=)|\b%s\s*(\+\+|--)|(\+\+|--)\s*%s\b|&\s*%s\b)"
                     % (r, r, r, r), between):
            return False
    mem = MEMREAD.search(rhs) or any(r not in LOCALS and not r.isupper() for r in reads)
    if mem and (CALL.search(between) or STORE.search(between)):
        return False
    return True


def statements(body):
    """Simple statements at any depth: (start, end) spans ending in ';', no braces inside."""
    out = []
    for m in STMT.finditer(body):
        s = m.group(0)
        if s.strip() and not re.match(r"\s*(for|return|break|continue|case|default)\b", s) \
                and "(" not in s.split("=")[0] if "=" in s else True:
            out.append((m.start(), m.end()))
    return out


def m_swap_adjacent(body, rnd):
    st = [s for s in statements(body) if body[s[0]:s[1]].strip()]
    pairs = [(a, b) for a, b in zip(st, st[1:]) if body[a[1]:b[0]].strip() == ""
             and (movable(body[a[0]:a[1]], body[b[0]:b[1]]) or movable(body[b[0]:b[1]], body[a[0]:a[1]]))]
    if not pairs:
        return None
    a, b = rnd.choice(pairs)
    return body[:a[0]] + body[b[0]:b[1]] + body[a[1]:b[0]] + body[a[0]:a[1]] + body[b[1]:]


def m_commute(body, rnd):
    def isolated(m):   # no operator binds to either operand from outside
        before = body[:m.start()].rstrip()
        after = body[m.end():].lstrip()
        return (before.endswith(("(", ",", "=", ";")) or before.endswith("return")) and             after[:1] in (")", ",", ";") and not before.endswith(("==", "!=", "<=", ">="))
    ms = [m for m in COMM.finditer(body) if isolated(m)]
    if not ms:
        return None
    m = rnd.choice(ms)
    return body[:m.start()] + f"{m.group(3)} {m.group(2)} {m.group(1)}" + body[m.end():]


def m_retype(body, rnd):
    ms = list(re.finditer(r"(?m)^(\s*)(%s)(\s+\w+\s*[=;])" % "|".join(TYPES), body))
    if not ms:
        return None
    m = rnd.choice(ms)
    t = rnd.choice([t for t in TYPES if t != m.group(2)])
    return body[:m.start(2)] + t + body[m.end(2):]


def m_expect(body, rnd):
    ms = list(re.finditer(r"\bif \(", body))
    if not ms:
        return None
    m = rnd.choice(ms)
    j, depth = m.end(), 1
    while depth and j < len(body):
        depth += {"(": 1, ")": -1}.get(body[j], 0)
        j += 1
    cond = body[m.end():j - 1]
    if "__builtin_expect" in cond:
        return None
    return body[:m.end()] + f"__builtin_expect(!!({cond}), {rnd.randint(0, 1)})" + body[j - 1:]


def m_temp(body, rnd):
    """Hoist the right-hand side of an assignment into a fresh local."""
    ms = list(re.finditer(r"(?m)^(\s*)([\w\.\->\[\]]+) = ([^;]+);", body))
    if not ms:
        return None
    m = rnd.choice(ms)
    n = f"t{rnd.randint(0, 999)}"
    return body[:m.start()] + f"{m.group(1)}{{ __typeof__({m.group(3)}) {n} = {m.group(3)}; " \
        f"{m.group(2)} = {n}; }}" + body[m.end():]


def m_volatile_reload(body, rnd):
    """Swap the order of two adjacent local declarations."""
    ms = list(re.finditer(r"(?m)^(\s*\w[\w \*]*\s\**\w+(?: = [^;]+)?;\n)(\s*\w[\w \*]*\s\**\w+(?: = [^;]+)?;\n)", body))
    if not ms:
        return None
    ms = [m for m in ms if movable(m.group(1), m.group(2)) or movable(m.group(2), m.group(1))]
    if not ms:
        return None
    m = rnd.choice(ms)
    return body[:m.start()] + m.group(2) + m.group(1) + body[m.end():]


def _block_end(text, i):
    """Index just past the brace block starting at text[i] == '{'."""
    depth = 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return j + 1
    return None


def m_ifelse(body, rnd):
    """if (c) {A} else {B}  ->  if (!(c)) {B} else {A}"""
    ms = list(re.finditer(r"\bif \(", body))
    rnd.shuffle(ms)
    for m in ms:
        j, depth = m.end(), 1
        while depth and j < len(body):
            depth += {"(": 1, ")": -1}.get(body[j], 0)
            j += 1
        cond = body[m.end():j - 1]
        k = j
        while k < len(body) and body[k].isspace():
            k += 1
        if k >= len(body) or body[k] != "{":
            continue
        a_end = _block_end(body, k)
        rest = body[a_end:]
        e = re.match(r"\s*else\s*\{", rest)
        if not a_end or not e:
            continue
        b_start = a_end + e.end() - 1
        b_end = _block_end(body, b_start)
        if not b_end:
            continue
        return (body[:m.end()] + f"!({cond})) " + body[b_start:b_end] + " else " +
                body[k:a_end] + body[b_end:])
    return None


def m_cmpzero(body, rnd):
    pats = [(r"\(([\w\.\->\[\]]+) != 0\)", r"(\1)"), (r"\(!([\w\.\->\[\]]+)\)", r"(\1 == 0)"),
            (r"\(([\w\.\->\[\]]+) == 0\)", r"(!\1)"), (r"if \(([\w\.\->\[\]]+)\)", r"if (\1 != 0)")]
    rnd.shuffle(pats)
    for a, b in pats:
        ms = list(re.finditer(a, body))
        if ms:
            m = rnd.choice(ms)
            return body[:m.start()] + re.sub(a, b, m.group(0)) + body[m.end():]
    return None


def m_move_stmt(body, rnd):
    """Move one simple statement to another position within the same brace block."""
    st = statements(body)
    if len(st) < 2:
        return None
    a = rnd.choice(st)
    def ok(b):
        lo, hi = (a[1], b[1]) if b[0] > a[0] else (b[0], a[0])
        span = body[lo:hi]
        return span.count("{") == span.count("}") == 0 and movable(body[a[0]:a[1]], span)
    same = [b for b in st if b != a and ok(b)]
    if not same:
        return None
    b = rnd.choice(same)
    text = body[a[0]:a[1]]
    rest = body[:a[0]] + body[a[1]:]
    pos = b[1] if b[0] > a[0] else b[0]
    if b[0] > a[0]:
        pos -= len(text)
    return rest[:pos] + text + rest[pos:]


def m_cast(body, rnd):
    """Add or drop an integer cast on a function-call argument."""
    ms = list(re.finditer(r"\((uint32_t|int32_t|unsigned|int|uint)\)\s*", body))
    if ms and rnd.random() < 0.5:
        m = rnd.choice(ms)
        return body[:m.start()] + body[m.end():]
    ms = list(re.finditer(r"([(,]\s*)([a-z_]\w*)(\s*[,)])", body))
    if not ms:
        return None
    m = rnd.choice(ms)
    return body[:m.start(2)] + f"({rnd.choice(['int', 'unsigned', 'uint32_t', 'int32_t'])}){m.group(2)}" + body[m.end(2):]


MUTATORS = [m_swap_adjacent, m_swap_adjacent, m_move_stmt, m_move_stmt, m_commute, m_retype,
            m_expect, m_temp, m_volatile_reload, m_ifelse, m_ifelse, m_cmpzero, m_cast]


def worker(args):
    img, srcpath, func, addr, iters, seed = args
    rnd = random.Random(seed)
    flags = build_flags(img, srcpath)
    src = open(srcpath).read()
    srcdir = os.path.dirname(os.path.abspath(srcpath))
    s, e = find_body(src, func)
    pre, body, post = src[:s], src[s:e], src[e:]
    params = pre[pre.rfind("(", 0, pre.rfind(")")):]
    LOCALS.update(re.findall(r"(\w+)\s*(?:\[[^\]]*\])?\s*[,)]", params))
    LOCALS.update(re.findall(r"(?m)^\s*(?:const\s+)?(?:unsigned\s+|struct\s+)?\w+[\s*]+\**(\w+)\s*(?:\[[^\]]*\])?\s*[=;]", body))
    LOCALS.difference_update(CWORDS)
    with tempfile.TemporaryDirectory() as td:
        best = score(img, flags, src, func, addr, td, srcdir)
        if best is None:
            return (None, None)
        cur_body, cur = body, best
        best_body = body
        tried = ok = 0
        for _ in range(iters):
            nb = cur_body
            for _ in range(rnd.randint(1, 3)):
                r = rnd.choice(MUTATORS)(nb, rnd)
                if r:
                    nb = r
            if nb == cur_body:
                continue
            tried += 1
            sc = score(img, flags, pre + nb + post, func, addr, td, srcdir)
            if sc is None or sc[1] == 0:
                continue
            ok += 1
            if key(sc) >= key(cur):
                cur_body, cur = nb, sc
                if key(sc) > key(best):
                    best, best_body = sc, nb
                    if sc[2]:
                        break
            elif rnd.random() < 0.02:   # occasional restart from the best
                cur_body, cur = best_body, best
        print(f"seed {seed}: {ok}/{tried} variants compiled", file=sys.stderr)
        return (best, pre + best_body + post)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("img"); ap.add_argument("src"); ap.add_argument("func"); ap.add_argument("addr")
    ap.add_argument("--iters", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=max(1, os.cpu_count() // 2))
    a = ap.parse_args()
    jobs = [(a.img, a.src, a.func, a.addr, a.iters, a.seed + i) for i in range(a.jobs)]
    with mp.Pool(a.jobs) as p:
        res = [r for r in p.map(worker, jobs) if r[0]]
    if not res:
        raise SystemExit("baseline does not compile or the function is not found")
    best = max(res, key=lambda r: key(r[0]))
    os.makedirs(os.path.join(ROOT, "permute_out"), exist_ok=True)
    out = os.path.join(ROOT, "permute_out", f"{a.func}.best.c")
    with open(out, "w") as f:
        f.write(best[1])
    print(f"{a.func} {best[0][0]}/{best[0][1]} calls+{best[0][3]}{' EXACT' if best[0][2] else ''} -> {out}")


if __name__ == "__main__":
    main()
