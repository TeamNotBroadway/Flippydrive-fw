"""Count function sections of an ar archive found verbatim in the firmware.
Usage: libmatch.py LIB.a FW..."""
import sys, re, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from libmatch_core import ar_members, funcs

fw = b''.join(pathlib.Path(p).read_bytes() for p in sys.argv[2:])
hit = hitbytes = 0
for mname, obj in ar_members(pathlib.Path(sys.argv[1]).read_bytes()):
    if obj[:4] != b'\x7fELF':
        continue
    for sname, code, mask in funcs(obj):
        pat = b''.join(re.escape(bytes([c])) if m else b'.' for c, m in zip(code, mask))
        if re.search(pat, fw, re.S):
            hit += 1; hitbytes += len(code)
print(sys.argv[1], 'functions found in firmware:', hit, 'bytes', hitbytes)
