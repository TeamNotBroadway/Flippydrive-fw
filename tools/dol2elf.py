"""Wrap a GameCube DOL (or a raw apploader image) in a minimal ELF so stock tools load it.

Usage: dol2elf.py IN.dol OUT.elf
       dol2elf.py --raw ADDR IN.bin OUT.elf   (one text section at ADDR, entry ADDR)
One PT_LOAD segment and one section per DOL text/data section, plus .bss (NOBITS).
"""
import struct
import sys


def dol_sections(d):
    offs = struct.unpack_from('>18I', d, 0)
    addrs = struct.unpack_from('>18I', d, 0x48)
    sizes = struct.unpack_from('>18I', d, 0x90)
    bss_addr, bss_size, entry = struct.unpack_from('>3I', d, 0xd8)
    secs = [(f'.text{i}' if i < 7 else f'.data{i - 7}', addrs[i], d[offs[i]:offs[i] + sizes[i]], i < 7)
            for i in range(18) if sizes[i]]
    return secs, (bss_addr, bss_size), entry


def write_elf(path, secs, bss, entry):
    names = b'\0' + b''.join(n.encode() + b'\0' for n, *_ in secs) + b'.bss\0.shstrtab\0'
    nph, nsh = len(secs), len(secs) + 3        # null, sections, .bss, .shstrtab
    phoff = 52
    data_off = phoff + 32 * nph
    blob, ph, sh = b'', [], [b'\0' * 40]
    name_off = 1
    for name, addr, body, is_text in secs:
        off = data_off + len(blob)
        blob += body + b'\0' * (-len(body) % 4)
        flags = 5 if is_text else 6               # R+X / R+W
        ph.append(struct.pack('>8I', 1, off, addr, addr, len(body), len(body), flags, 4))
        sh.append(struct.pack('>10I', name_off, 1, 6 if is_text else 3, addr, off, len(body), 0, 0, 4, 0))
        name_off += len(name) + 1
    sh.append(struct.pack('>10I', name_off, 8, 3, bss[0], data_off + len(blob), bss[1], 0, 0, 4, 0))
    name_off += 5
    str_off = data_off + len(blob)
    sh.append(struct.pack('>10I', name_off, 3, 0, 0, str_off, len(names), 0, 0, 1, 0))
    shoff = str_off + len(names) + (-(str_off + len(names)) % 4)
    hdr = b'\x7fELF\x01\x02\x01' + b'\0' * 9 + struct.pack(
        '>HHIIIIIHHHHHH', 2, 20, 1, entry, phoff, shoff, 0, 52, 32, nph, 40, nsh, nsh - 1)
    out = hdr + b''.join(ph) + blob + names
    out += b'\0' * (shoff - len(out)) + b''.join(sh)
    open(path, 'wb').write(out)


def main():
    a = sys.argv[1:]
    if a[0] == '--raw':
        addr = int(a[1], 0)
        body = open(a[2], 'rb').read()
        write_elf(a[3], [('.text0', addr, body, True)], (addr + len(body), 0), addr)
    else:
        secs, bss, entry = dol_sections(open(a[0], 'rb').read())
        write_elf(a[1], secs, bss, entry)


if __name__ == '__main__':
    main()
