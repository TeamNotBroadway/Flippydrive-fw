# Exact build

Two builds of each RP2040 image come from the same objects:

- **C build** (`tools/wsl_build.sh`): every function from C. Runs, but is not byte-identical.
- **Exact build** (`tools/rp_exact.py`): byte-identical to the original. Every section of the C
  build's objects and archive members that matches the original is linked at its original
  address; the rest is taken from the original image.

```
wsl.exe -d Ubuntu --cd F:lippydrive-fw -- bash tools/wsl_build.sh . fw
wsl.exe -d Ubuntu --cd F:lippydrive-fw -- python3 tools/rp_exact.py bootloader
wsl.exe -d Ubuntu --cd F:lippydrive-fw -- python3 tools/rp_exact.py main-ode
```

Output in `~/fd-build/exact-<image>/`:

| File | Contents |
|---|---|
| `flippydrive-<image>.bin` | the image; the run ends `IDENTICAL` or exits non-zero with the first differing offset |
| `status.tsv` | each `syms/<image>.src.tsv` function: `C` and its object, or `original` and why |
| `sections.tsv` | every placed section: address, size, object, section, what fixed its address |
| `gaps.tsv` | every range still taken from the original: address, length, owner where known |
| `exact.ld`, `gaps.s`, `syms.s`, `exact.map` | generated linker script, gap contents, absolute symbols, map |

The run prints the image's bytes by source: `C` (placed sections), zero bytes, strings found in
our objects' merged string sections, long-branch veneers to known functions, and `original`.
Only `original` is `.incbin`'d. `gaps.tsv` owners come from `syms/<image>.lib.tsv`, from
unplaced sections whose address a placed one implies (with our size), and from `src.tsv`
functions up to the next known start, so an owner's byte count is approximate.

In `status.tsv`, a reason of `differs at +N` or `no BL at +N` is a code mismatch.
`X would be at A, not B (from F)` means this function and F disagree on where X is: one of them
calls or loads the wrong thing even though both score exact.

## How it works

1. Inputs: every object and archive member in the C build's linker map
   (`~/fd-build/fw/flippydrive-<image>.elf.map`, from `pico_add_map_output`).
2. Addresses: `syms/<image>.src.tsv` functions are fixed up front. A section whose address is
   known is compared with the original, relocations masked; each relocation then says what its
   symbol must resolve to: the original's word minus the addend (ABS32, TARGET1, REL32), or the
   BL target (THM_CALL). A BL to an ld long-branch veneer resolves to the veneer's target.
   Global symbols resolve through the strong definition, as the linker would.
3. A section is placed only if every value it implies agrees with what is already known and it
   does not overlap a placed section; otherwise it stays in the original's bytes. Placed
   sections imply more addresses; when none are pending, sections with at least 12 unmasked
   bytes that occur exactly once in the image (at an aligned address) are tried there.
4. The linker script places used sections at their original addresses (with ASSERTs) inside
   output sections that follow the original's flash and RAM-copy layout (from crt0's copy
   table). Gaps are zero fill, verified strings and veneers, or `.incbin` of the original.
   Sections referred to but not placed get their derived address as NOLOAD sections; symbols
   outside our objects are defined absolute in `syms.s`.
5. The linked objects are copies: unused sections lose their relocations and alignment (they
   only lend an address), and two kinds of word take the original's value with the relocation
   removed, because the linker made them rather than the C:
   - pointers into merged string sections, after checking the string at that address is ours;
   - BLs through a long-branch veneer, after checking the veneer's target.

The C build is unaffected. Nothing is recompiled; run `wsl_build.sh` first.
