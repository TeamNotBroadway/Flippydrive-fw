# Library function matching: agent brief

Instructions for one agent matching one library function (FatFs so far) against the original
image. The task prompt names the function, its address and the workspace.

## Rules

- Edit only `permute_out/ws/<workspace>/` files. No other file, no commits, no git writes.
- The vendored source is the public release; the vendor edited it. Reproduce the vendor's edits
  (logic the original has and the public source does not, or the reverse), then shape the C until
  it compiles to the original's bytes.
- Every logic change must be visible in the original's code (Ghidra output or disassembly).
  Do not change behaviour to gain windows without that evidence.
- Keep edits inside the function where possible. If a helper, macro or prototype must change,
  report it separately; the merge tool copies function bodies only.

## Inputs

- Ghidra's decompile of the original: `decomp/main-ode/fatfs/0x<addr>.c`. Its names are stale
  guesses; FUN_xxx are unnamed. Known: 0x10080734 `disk_read`, 0x10092c30 `disk_write`.
- Identified differences per address: `decomp/main-ode/fatfs/ids_A.md` and `ids_B.md`.
- Struct layouts equal ours (`ff.h`); main-ode uses `FF_FS_LOCK` 2 (the workspace `ffconf.h`
  may be set to 2).

## Commands (PowerShell; never put a `$` in the command line)

```
wsl.exe -d Ubuntu -- bash /mnt/f/flippydrive-fw/tools/ws.sh score FUNC 0xADDR --show
wsl.exe -d Ubuntu -- bash /mnt/f/flippydrive-fw/tools/ws.sh permute FUNC 0xADDR 300
```

`score` prints `hits/total size addr first-diff name calls distance N`, then ours beside the
original ('*' marks differing lines). Goal: EXACT and calls distance 0. `permute` writes its
best variant to `permute_out/FUNC.best.c` and does not touch the workspace: take ideas from it
by hand, as readable C (no `__builtin_expect`, no `!(!(x))`, no pointless temporaries).

If the function is not emitted on its own (inlined into its callers in our build but not in the
original), `score` says so: add `__attribute__((noinline))` only if the original calls it.
For a compiler clone in the original (fewer parameters), score our clone name, e.g.
`put_fat.part.0`, not the source name.

## Method

1. Read the original's code and the ids notes; apply the logic edits. Score.
2. Read the `--show` diff and reshape: statement order, temporaries, types, condition form,
   early returns. Keep a change only if hits rise.
3. Permuter once when stuck. Stop at EXACT, after about 30 score runs, or after 10 runs without
   progress.

## Report (short, plain text)

- Function, address, baseline score, final score (hits/total, EXACT or first diff, calls
  distance).
- Each logic edit made, one line each, with its evidence (address or Ghidra line).
- Any change needed outside the function body.
- What the remaining difference is, in one line.
Do not paste the function body: it is in the workspace.
