"""Split the GameCube disc image embedded in the bootloader into its parts.

Usage: gcdisc.py BOOTLOADER.bin OUTDIR
Writes disc.iso, boot.bin, bi2.bin, apploader.img (header + code + trailer), fst.bin,
main.dol, every FST file, and payload.dol: main.dol is a stub that gunzips a second DOL
(named updater.dol in the gzip header) and loads it. From payload.dol, font.tpl and logo.tpl,
the two textures gamecube/payload links. Also writes main.elf, payload.elf and
apploader.elf (tools/dol2elf.py) for Ghidra.
"""
import pathlib
import struct
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from dol2elf import dol_sections, write_elf  # noqa: E402

DISC_OFF, DISC_SIZE = 0x2d0, 0x25900   # file offsets in flippydrive-bootloader.bin
# The payload's textures: (name, file offset in payload.dol, size).
TEXTURES = [('font.tpl', 0x27460, 0xf040), ('logo.tpl', 0x364a0, 0x5f40)]


def main():
    img = pathlib.Path(sys.argv[1]).read_bytes()
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    d = img[DISC_OFF:DISC_OFF + DISC_SIZE]
    assert d[0x1c:0x20] == b'\xc2\x33\x9f\x3d', 'no GameCube magic'
    parts = {'disc.iso': d, 'boot.bin': d[:0x440], 'bi2.bin': d[0x440:0x2440]}
    _, entry, size, trailer = struct.unpack_from('>16sIII', d, 0x2440)
    parts['apploader.img'] = d[0x2440:0x2460 + size + trailer]
    dol, fst, fst_size = struct.unpack_from('>III', d, 0x420)
    parts['fst.bin'] = d[fst:fst + fst_size]
    offs = struct.unpack_from('>7I11I', d, dol)
    sizes = struct.unpack_from('>7I11I', d, dol + 0x90)
    text = d[dol:dol + max(o + s for o, s in zip(offs, sizes) if s)]
    parts['main.dol'] = text
    n = struct.unpack_from('>I', d, fst + 8)[0]
    names = fst + n * 12
    for i in range(1, n):
        flags_name, off, ln = struct.unpack_from('>III', d, fst + i * 12)
        if flags_name >> 24 == 0:
            o = names + (flags_name & 0xffffff)
            parts[d[o:d.index(b'\0', o)].decode()] = d[off:off + ln]
    gz = text.index(b'\x1f\x8b\x08', offs[0])
    z = zlib.decompressobj(31)
    parts['payload.dol'] = z.decompress(text[gz:])
    assert z.eof
    for name, off, size in TEXTURES:
        parts[name] = parts['payload.dol'][off:off + size]
        assert parts[name][:4] == bytes.fromhex('0020af30'), f'{name}: no TPL magic'
    for name, data in parts.items():
        (out / name).write_bytes(data)
        print(f'{name:16} {len(data):#8x}')
    for name in ('main', 'payload'):
        write_elf(out / f'{name}.elf', *dol_sections(parts[f'{name}.dol']))
    code = parts['apploader.img'][0x20:0x20 + size]
    write_elf(out / 'apploader.elf', [('.text0', 0x81200000, code, True)], (0x81200000 + size, 0), entry)
    print(f'apploader entry {entry:#x} size {size:#x} trailer {trailer:#x}')
    print(f'payload: gzip at main.dol offset {gz:#x}, {len(text) - len(z.unused_data) - gz:#x} bytes')


if __name__ == '__main__':
    main()
