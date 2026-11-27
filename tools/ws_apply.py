#!/usr/bin/env python3
"""Copy function bodies from tools/ws.sh workspaces into the tree's library source.

    python3 tools/ws_apply.py LIB FUNC[=WORKSPACE] ...      (run under WSL)

For each FUNC, the body of FUNC in permute_out/ws/WORKSPACE/<file> (WORKSPACE defaults to FUNC)
replaces FUNC's body in third_party/LIB/<file>, where <file> is the source defining FUNC.
Prints a unified diff of every change. Edits outside function bodies (macros, prototypes,
helpers) are not copied: name each changed helper as its own FUNC=WORKSPACE.
"""
import difflib, pathlib, re, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import permute as P

ROOT = pathlib.Path(P.ROOT)


def defining(d, func):
    pat = re.compile(r"^[A-Za-z_][^;\n]*\b%s\b\)?\s*\([^;\n]*$" % re.escape(func), re.M)  # not a prototype
    hits = [p for p in sorted(d.glob("*.c")) if pat.search(p.read_text(errors="replace"))]
    if len(hits) != 1:
        sys.exit(f"{func}: defined in {len(hits)} files under {d}")
    return hits[0]


def main():
    lib, items = sys.argv[1], sys.argv[2:]
    if not items:
        sys.exit(__doc__)
    tree = ROOT / "third_party" / lib
    for it in items:
        func, _, ws = it.partition("=")
        wsd = ROOT / "permute_out" / "ws" / (ws or func)
        dst = defining(tree, func)
        src = wsd / dst.name
        new_src = src.read_text()
        s, e = P.find_body(new_src, func)
        body = new_src[s:e]
        old = dst.read_bytes().decode()
        crlf = "\r\n" in old
        text = old.replace("\r\n", "\n")
        s2, e2 = P.find_body(text, func)
        if text[s2:e2] == body.replace("\r\n", "\n"):
            print(f"{func}: unchanged")
            continue
        out = text[:s2] + body.replace("\r\n", "\n") + text[e2:]
        sys.stdout.writelines(difflib.unified_diff(text.splitlines(True), out.splitlines(True),
                                                   f"a/{dst.relative_to(ROOT)}", f"b/{dst.relative_to(ROOT)}", n=2))
        dst.write_bytes((out.replace("\n", "\r\n") if crlf else out).encode())


if __name__ == "__main__":
    main()
