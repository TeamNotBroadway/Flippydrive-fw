#!/usr/bin/env python3
"""Per-component byte table of the RP2040 exact builds, as Markdown (run under WSL after
tools/check_all.sh).

    python3 tools/exact_summary.py

Reads ~/fd-build/exact-IMAGE/{sections,status,gaps}.tsv. Placed sections are bytes compiled
from this project's sources; gaps.tsv bytes are copied from the original image.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = {"bootloader": 0x40600, "main-ode": 0x1b800}
TINYUSB = {"usbd", "usbd_control", "dcd_rp2040", "rp2040_usb", "cdc_device", "vendor_device",
           "tusb_fifo", "tusb"}
ORDER = ["Application (decompiled)", "FatFs R0.15 (vendor-edited)", "esp-serial-flasher (fork-edited)",
         "pico-sdk 2.1.1", "TinyUSB (SDK 2.1.1)", "newlib / libgcc", "Embedded GameCube disc and data"]


def component(obj, app):
    o = obj.split("(")[-1].rstrip(")")
    base = re.sub(r"\.(c|S)\.o$", "", o)
    if "fatfs" in obj:
        return ORDER[1]
    if "esp_serial_flasher" in obj:
        return ORDER[2]
    if obj.startswith(("libg.a", "libc", "libm", "libgcc", "libnosys")) or "libc_a-" in obj:
        return ORDER[5]
    if base == "flash_blob":
        return ORDER[6]
    if base in TINYUSB:
        return ORDER[3 + 1]
    if base in app:
        return ORDER[0]
    return ORDER[3]


def main():
    home = os.path.expanduser("~/fd-build")
    out = []
    for img, size in IMAGES.items():
        d = f"{home}/exact-{img}"
        app = {os.path.splitext(f)[0] for f in os.listdir(os.path.join(ROOT, img)) if f.endswith(".c")}
        tot = {}
        for l in open(f"{d}/sections.tsv"):
            c = l.rstrip("\n").split("\t")
            if len(c) >= 3 and c[1].isdigit():
                k = component(c[2], app)
                tot[k] = tot.get(k, 0) + int(c[1])
        orig = sum(int(l.split("\t")[1]) for l in open(f"{d}/gaps.tsv") if l.split("\t")[1].isdigit())
        st = [l.rstrip("\n").split("\t") for l in open(f"{d}/status.tsv")]
        n_c = sum(1 for r in st if r[2] == "C")
        placed = sum(tot.values())
        out.append(f"\n**{img}** ({size:#x} bytes; {n_c} of {len(st)} application functions from C)\n")
        out.append("| Component | Bytes from C | Share of image |")
        out.append("|---|---:|---:|")
        for k in ORDER:
            if k in tot:
                out.append(f"| {k} | {tot[k]} | {100 * tot[k] / size:.1f}% |")
        rest = size - placed - orig
        out.append(f"| Strings, padding and fill (generated) | {rest} | {100 * rest / size:.1f}% |")
        out.append(f"| **Copied from the original** | **{orig}** | **{100 * orig / size:.1f}%** |")
        names = [r[1] for r in st if r[2] != "C"]
        out.append(f"\nApplication functions still copied from the original: {', '.join(f'`{n}`' for n in names)}.")
    print("\n".join(out))


if __name__ == "__main__":
    main()
