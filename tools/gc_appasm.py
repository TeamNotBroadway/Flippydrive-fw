"""Write assembly stand-ins for the GameCube payload application's data and for the
functions whose C does not yet build to the original.

The application's functions are the FDEs in payload.dol's .eh_frame that fall in
0x8000533c-0x80009484, all now in C. For each function in ASM_BODIES, NAME.inc holds its
body copied word for word, except that branches out of the function name their target (a C
function, or a library one from syms/gc-payload.lib.tsv) so the link resolves them. The FDE's
call frame instructions are re-emitted as .cfi directives, so the linker builds the same
.eh_frame. main.c includes the file into an asm-only function when NONMATCHING is unset.

The data that is not yet in C is .incbin'd from payload.dol (found on the assembler's -I
path) into app0.s, in the sections a C build gives it, so the linker places and merges it as
in the original.

Temporary: each body and data range is replaced by C as it is decompiled.

Usage: gc_appasm.py payload.dol lib.tsv OUTDIR
"""
import struct
import sys

LO, HI = 0x8000533c, 0x80009484
TEXT, TEXT_OFF = 0x80003100, 0x100
DATA, DATA_OFF = 0x8002a3e0, 0x27400
EH_FRAME = 0x80044580

# Functions in C, by address: the branch targets the bodies name.
C_FUNCS = {
    0x8000533c: 'texture_load',
    0x800053e0: 'draw_texture',
    0x800055fc: 'draw_text',
    0x80005898: 'draw_rect',
    0x800059dc: 'dvd_inquiry',
    0x80005a60: 'dvd_bootloader_status',
    0x80005b04: 'dvd_bootloader_status_ext',
    0x80005b88: 'dvd_bootloader_boot',
    0x80005bf0: 'dvd_bootloader_update',
    0x80005c58: 'dvd_bootloader_noupdate',
    0x80005cc0: 'dvd_custom_close',
    0x80005d20: 'dvd_set_default_fd',
    0x80005d80: 'dvd_read',
    0x80005e18: 'dvd_custom_status',
    0x80005e9c: 'dvd_custom_unlink',
    0x80005f48: 'dvd_custom_unlink_flash',
    0x80005ff4: 'dvd_custom_open',
    0x800060c0: 'dvd_custom_open_flash',
    0x8000618c: 'dvd_custom_bypass',
    0x800061e4: 'dvd_custom_cmd18',
    0x80006240: 'dvd_custom_reload_config',
    0x800062c8: 'dvd_custom_net_status',
    0x80006350: 'dvd_cover_status',
    0x80006364: 'video_init',
    0x800067f4: 'set_clear_color',
    0x8000683c: 'frame_begin',
    0x800068fc: 'frame_end',
    0x800069c4: 'bypass',
    0x800069e8: 'boot_menu',
    0x80006ec8: 'log_printf',
    0x80006f9c: 'draw_progress',
    0x800070e8: 'load_stub',
    0x800071d0: 'chainload',
    0x80007278: 'load_dol',
    0x800073c4: 'boot_main_app',
    0x80007634: 'step_state',
    0x80007bfc: 'gfx_init',
    0x80007cd4: 'status_screen',
    0x800083fc: 'update_firmware',
    0x8000842c: 'start_firmware',
    0x80008484: 'boot_entry',
    0x800084c8: 'exit_menu',
    0x800084dc: 'enhanced_bypass',
    0x8000850c: 'recovery',
    0x80008560: 'main_menu',
    0x8000880c: 'main',
    0x80008990: 'draw_textf',
    0x80008a68: 'ARAMStub',
    0x80008b48: 'ARAMRunStub',
    0x80008c1c: 'DOLMinMax',
    0x80008d94: 'DOLtoARAM',
    0x80009074: '__ARWriteDMA',
    0x80009114: 'ARAMFetch',
    0x80009204: 'ARAMPut',
    0x800093e4: 'ARAMClear',
}

# main.c functions whose C differs from the original (see docs/HACKS.md).
ASM_BODIES = [0x800069e8, 0x800073c4, 0x80008560, 0x8000880c]

# What the bodies address by number (lis/addi) rather than by a branch. main's body, which
# is always linked, gets an R_PPC_NONE reference to each, to keep --gc-sections from
# dropping it. main.c marks them ASM_REF.
ASM_REFS = ['boot_menu', 'boot_main_app', 'boot_entry', 'exit_menu', 'main_items',
            'menu_items', 'boot_entries', 'selected_item', 'dol_hdr', 'dol_fd']

# The application's data still taken from payload.dol: (section, flags, alignment, start,
# end, label). app0.o is the first object with merged sections, so they start with the
# original's.
DATA_SECTIONS = [
    # Merged with the C objects' and libraries' constants and strings: the linker places each
    # merged section at its first input and drops the later duplicates.
    ('.rodata.cst4', '"aMR",@progbits,4', 4, 0x8003f3c0, 0x8003f400, None),
    ('.rodata.str1.4', '"aMSR",@progbits,1', 4, 0x8003f478, 0x8003fd58, None),
]

# Addresses where identical wrappers share one code pattern: the name the caller means,
# chosen by the reentrant function each one calls.
NAMES = {
    0x8000c3ac: 'VIDEO_SetNextFramebuffer',
    0x8001d544: 'vsnprintf',
    0x8001d70c: 'puts',
    0x80021f38: 'write',
    0x8002259c: 'vsprintf',
    0x80025c50: 'free',
    0x80026268: 'snprintf',
    0x80027118: 'memalign',
}


def uleb(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7f) << s
        s += 7
        if c < 0x80:
            return r, i


def sleb(b, i):
    r, j = uleb(b, i)
    bits = 7 * (j - i)
    return (r - (1 << bits) if r >> (bits - 1) & 1 else r), j


def cfi(insns):
    """Decode DWARF call frame instructions (code align 4, data align -4) into
    {code offset: [directive, ...]}."""
    out = {}
    loc = i = 0
    while i < len(insns):
        c = insns[i]
        i += 1
        hi, lo = c >> 6, c & 0x3f
        d = None
        if hi == 1:
            loc += lo * 4
        elif hi == 2:
            n, i = uleb(insns, i)
            d = f'.cfi_offset {lo}, {-4 * n}'
        elif hi == 3:
            d = f'.cfi_restore {lo}'
        elif c == 0x00:
            pass
        elif c == 0x02:
            loc += insns[i] * 4
            i += 1
        elif c == 0x03:
            loc += struct.unpack_from('>H', insns, i)[0] * 4
            i += 2
        elif c == 0x05:
            r, i = uleb(insns, i)
            n, i = uleb(insns, i)
            d = f'.cfi_offset {r}, {-4 * n}'
        elif c == 0x06:
            r, i = uleb(insns, i)
            d = f'.cfi_restore {r}'
        elif c == 0x09:
            r, i = uleb(insns, i)
            r2, i = uleb(insns, i)
            d = f'.cfi_register {r}, {r2}'
        elif c == 0x0a:
            d = '.cfi_remember_state'
        elif c == 0x0b:
            d = '.cfi_restore_state'
        elif c == 0x0e:
            n, i = uleb(insns, i)
            d = f'.cfi_def_cfa_offset {n}'
        elif c == 0x11:
            r, i = uleb(insns, i)
            n, i = sleb(insns, i)
            d = f'.cfi_offset {r}, {-4 * n}'
        else:
            sys.exit(f'unhandled call frame instruction 0x{c:02x}')
        if d:
            out.setdefault(loc, []).append(d)
    return out


def fdes(img):
    """(start, size, {offset: directives}) for each FDE covering the application. Their
    CIE is GCC's default (augmentation zR, pc-relative sdata4, CFA r1+0)."""
    def off(v):
        return v - DATA + DATA_OFF
    v = EH_FRAME
    while True:
        length, cie = struct.unpack_from('>II', img, off(v))
        if length == 0:
            return
        if cie:
            body = img[off(v) + 8:off(v) + 4 + length]
            start = struct.unpack_from('>i', body, 0)[0] + v + 8
            size = struct.unpack_from('>I', body, 4)[0]
            if LO <= start < HI:
                assert body[8] == 0, 'augmentation data'
                yield start, size, cfi(body[9:])
        v += 4 + length


def main():
    img = open(sys.argv[1], 'rb').read()
    lib = {}
    for line in open(sys.argv[2]):
        a, n = line.split('\t')[:2]
        lib[int(a, 16)] = n.split('|')[0]
    lib.update(NAMES)

    def word(va):
        return struct.unpack_from('>I', img, va - TEXT + TEXT_OFF)[0]

    def target(va, w):
        o = w & 0x3fffffc
        return va + (o - 0x4000000 if o & 0x2000000 else o)

    out = ['# Generated by tools/gc_appasm.py from payload.dol: application data.']
    for sec, flags, align, start, end, label in DATA_SECTIONS:
        out += ['', f'	.section {sec},{flags}', f'	.balign {align}']
        if label:
            out += [f'	.globl {label}', f'{label}:']
        out.append(f'	.incbin "payload.dol", 0x{start - DATA + DATA_OFF:x}, 0x{end - start:x}')
    open(f'{sys.argv[3]}/app0.s', 'w', newline='\n').write('\n'.join(out) + '\n')

    for start, size, frame in fdes(img):
        if start not in ASM_BODIES:
            continue
        n = C_FUNCS[start]
        out = [f'# Generated by tools/gc_appasm.py from payload.dol: body of {n}.']
        if n == 'main':
            out += [f'	.reloc ., R_PPC_NONE, {sym}' for sym in ASM_REFS]
        for va in range(start, start + size, 4):
            out += [f'	{d}' for d in frame.get(va - start, [])]
            w = word(va)
            t = target(va, w)
            if w >> 26 == 18 and not w & 2 and not start <= t < start + size:
                name = C_FUNCS.get(t) if LO <= t < HI else lib.get(t)
                if name is None:
                    sys.exit(f'{va:08x}: branch to unnamed {t:08x}')
                out.append(f'	{"bl" if w & 1 else "b"} {name}')
            else:
                out.append(f'	.4byte 0x{w:08x}')
        open(f'{sys.argv[3]}/{n}.inc', 'w', newline='\n').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
