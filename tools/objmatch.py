"""Per function section of every .obj under a directory: mask relocated bytes and report
whether it occurs verbatim in the firmware images. Usage: objmatch.py DIR OUT.tsv FW..."""
import sys, re, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from libmatch_core import funcs

root = pathlib.Path(sys.argv[1])
fw = b''.join(pathlib.Path(p).read_bytes() for p in sys.argv[3:])
rows = []
for obj in sorted(root.rglob('*.obj')):
    data = obj.read_bytes()
    for sname, code, mask in funcs(data):
        pat = b''.join(re.escape(bytes([c])) if m else b'.' for c, m in zip(code, mask))
        found = re.search(pat, fw, re.S) is not None
        rows.append((obj.relative_to(root).as_posix(), sname, len(code), found))
with open(sys.argv[2], 'w') as f:
    for r in rows:
        f.write('\t'.join(map(str, r)) + '\n')
hit = [r for r in rows if r[3]]
print(root, 'functions', len(rows), 'found', len(hit), 'bytes', sum(r[2] for r in hit))
