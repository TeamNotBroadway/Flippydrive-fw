"""Unpack a FlippyDrive update.fpkg and verify each entry's MD5, or repack one.

    python fpkg.py PKG OUTDIR                      unpack every entry into OUTDIR
    python fpkg.py --pack PKG OUT [FILE ...]       write PKG to OUT with entries replaced

--pack keeps PKG's entry order and headers and replaces the payload of each entry whose
unpacked file name (as unpack writes it, e.g. flippydrive-bootloader.bin) matches a FILE's
name, updating its length and MD5. With no FILE, OUT is a copy of PKG; this checks the layout
below.

Layout (little-endian), inferred from the 1.4.6 release's update.fpkg:
  0x000  "FPK1", u32 version (0x00MMmmpp), u32 count, u32 offset[16], u32 size[16];
         zero to 0x200.
  entry: 0x200-byte FIM1 header, then payload zero-padded to a multiple of 0x200; entries
         follow each other with no gap. size = 0x200 + padded payload.
  FIM1:  0x00 "FIM1"  0x04 u32 version (0x00MMmmpp)  0x08 u32 group id
         0x0c u32 load address  0x10 u32 payload length  0x14 u32 type
         0x18 char name[64]  0x58 char version[16]  0x68 md5[16]; zero to 0x200.
There is no checksum over the package.
"""
import hashlib, pathlib, struct, sys

HDR = 0x200


def entries(d):
    assert d[:4] == b"FPK1"
    n = struct.unpack_from("<I", d, 8)[0]
    offs = struct.unpack_from("<%dI" % n, d, 0x0C)
    sizes = struct.unpack_from("<%dI" % n, d, 0x4C)
    for o, s in zip(offs, sizes):
        h = d[o:o + HDR]
        assert h[:4] == b"FIM1"
        ln = struct.unpack_from("<I", h, 0x10)[0]
        name = h[0x18:0x58].split(b"\0")[0].decode().lstrip("/")
        yield (name if "." in name else name + ".bin"), h, d[o + HDR:o + HDR + ln]


def unpack(pkg, out):
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    for fn, h, p in entries(pathlib.Path(pkg).read_bytes()):
        _, ver, group, load, ln, typ = struct.unpack_from("<6I", h, 0)
        sver = h[0x58:0x68].split(b"\0")[0].decode()
        ok = hashlib.md5(p).digest() == h[0x68:0x78]
        (out / fn).write_bytes(p)
        print(f"{fn:28} {sver:6} type=0x{typ:03x} load=0x{load:08x} len=0x{ln:06x} md5={'ok' if ok else 'BAD'}")


def pack(pkg, out, files):
    d = pathlib.Path(pkg).read_bytes()
    repl = {pathlib.Path(f).name: pathlib.Path(f).read_bytes() for f in files}
    head = bytearray(d[:HDR])
    body = bytearray()
    ents = list(entries(d))
    unknown = set(repl) - {fn for fn, _, _ in ents}
    if unknown:
        sys.exit(f"no entry named {', '.join(sorted(unknown))}")
    for k, (fn, h, p) in enumerate(ents):
        h = bytearray(h)
        if fn in repl:
            p = repl[fn]
            struct.pack_into("<I", h, 0x10, len(p))
            h[0x68:0x78] = hashlib.md5(p).digest()
        slot = bytes(h) + p + bytes(-len(p) % HDR)
        struct.pack_into("<I", head, 0x0C + 4 * k, HDR + len(body))
        struct.pack_into("<I", head, 0x4C + 4 * k, len(slot))
        body += slot
        print(f"{fn:28} {'replaced' if fn in repl else 'original'} len=0x{len(p):06x}")
    new = bytes(head) + bytes(body)
    pathlib.Path(out).write_bytes(new)
    print(f"{out}: 0x{len(new):x} bytes, {'identical to' if new == d else 'differs from'} {pkg}")


if __name__ == "__main__":
    if sys.argv[1] == "--pack":
        pack(sys.argv[2], sys.argv[3], sys.argv[4:])
    else:
        unpack(sys.argv[1], sys.argv[2])
