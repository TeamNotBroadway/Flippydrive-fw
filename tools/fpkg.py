"""Unpack a FlippyDrive update.fpkg and verify each entry's MD5.

Layout (little-endian), inferred from update.fpkg 0.4.1:
  0x000  "FPK1", u32 ?, u32 count, u32 offset[16], u32 size[16]
  entry: 0x200-byte FIM1 header, then payload, zero-padded to size.
  FIM1:  0x00 "FIM1"  0x04 u32 version (0x00MMmmpp)  0x08 u32 group id
         0x0c u32 load address  0x10 u32 payload length  0x14 u32 type
         0x18 char name[64]  0x58 char version[16]  0x68 md5[16]
"""
import hashlib, pathlib, struct, sys

def unpack(pkg, out):
    d = pathlib.Path(pkg).read_bytes()
    assert d[:4] == b"FPK1"
    n = struct.unpack_from("<I", d, 8)[0]
    offs = struct.unpack_from("<%dI" % n, d, 0x0C)
    sizes = struct.unpack_from("<%dI" % n, d, 0x4C)
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    for o, s in zip(offs, sizes):
        h = d[o:o + 0x200]
        assert h[:4] == b"FIM1"
        _, ver, group, load, ln, typ = struct.unpack_from("<6I", h, 0)
        name = h[0x18:0x58].split(b"\0")[0].decode().lstrip("/")
        sver = h[0x58:0x68].split(b"\0")[0].decode()
        p = d[o + 0x200:o + 0x200 + ln]
        ok = hashlib.md5(p).digest() == h[0x68:0x78]
        fn = name if "." in name else name + ".bin"
        (out / fn).write_bytes(p)
        print(f"{fn:28} {sver:6} type=0x{typ:03x} load=0x{load:08x} len=0x{ln:06x} md5={'ok' if ok else 'BAD'}")

if __name__ == "__main__":
    unpack(sys.argv[1], sys.argv[2])
