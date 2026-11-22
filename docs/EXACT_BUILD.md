# Exact build

Two builds of each RP2040 image come from the same objects:

- **C build** (`tools/wsl_build.sh`): every function from C. Runs, but is not byte-identical.
- **Exact build** (`tools/rp_exact.py`): byte-identical to the original. Functions whose C matches
  are linked from the C build's objects at their original addresses; everything else is
  `.incbin`'d from the original image.

```
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- bash tools/wsl_build.sh . fw
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- python3 tools/rp_exact.py bootloader
wsl.exe -d Ubuntu --cd F:\flippydrive-fw -- python3 tools/rp_exact.py main-ode
```

Output in `~/fd-build/exact-<image>/`:

| File | Contents |
|---|---|
| `flippydrive-<image>.bin` | the image; the run ends `IDENTICAL` or exits non-zero with the first differing offset |
| `status.tsv` | address, function, `C` or `original`, and the object or the reason it was not used |
| `exact.ld`, `gaps.s`, `syms.s` | generated linker script, original-byte gaps, absolute symbols |
| `exact.map` | linker map |

`status.tsv` is the list of functions still taken from the original. A reason of `differs at +N`
or `no BL at +N` is a code mismatch. `X would be at A, not B (from F)` means this function and F
disagree on where X is: one of them calls or loads the wrong thing even though both score exact.

## How it works

1. Candidates: every function in `syms/<image>.src.tsv` that is defined in one of the image's
   objects with a section of its own (`-ffunction-sections`).
2. Each candidate's bytes are compared with the original, relocations masked. Each relocation
   then says what its symbol must resolve to: the original's word minus the addend (ABS32), or
   the BL target (THM_CALL). A BL to an ld long-branch veneer resolves to the veneer's target.
3. All `src.tsv` addresses are fixed up front. A candidate is used only if every value it
   implies agrees with what is already known; otherwise it stays in the original's bytes.
4. The linker script places used functions at their original addresses (with ASSERTs) inside
   output sections that follow the original's flash and RAM-copy layout (from crt0's copy
   table). Gaps are `.incbin` of the original. Sections the used functions refer to but which
   are not used themselves get their derived address as NOLOAD sections; symbols outside our
   objects are defined absolute in `syms.s`.
5. The linked objects are copies: unused sections lose their relocations and alignment (they
   only lend an address), and two kinds of word take the original's value with the relocation
   removed, because the linker made them rather than the C:
   - pointers into merged string sections, after checking the string at that address is ours;
   - BLs through a long-branch veneer, after checking the veneer's target.

The C build is unaffected. Nothing is recompiled; run `wsl_build.sh` first.
