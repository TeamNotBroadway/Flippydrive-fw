# Function matching: agent brief

Instructions for one agent matching one function against the original image: a vendored
library function or an application function. The task prompt names the function, its address,
the image and the workspace.

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

- Application functions: `python3 tools/ghidra_fn.py IMAGE ADDR` (under WSL, ADDR in hex without
  0x) prints Ghidra's decompile with calls and literal-pool globals renamed to our names. The
  workspace holds the image's whole source directory; our current C is the function of the same
  name there.
- Library functions: Ghidra's decompile is `decomp/<image>/<lib>/0x<addr>.c` where one exists.
- Ghidra's names are stale guesses; FUN_xxx are unnamed. `syms/<image>.src.tsv` and
  `syms/<image>.lib.tsv` map addresses to our names.

## Commands (PowerShell; never put a `$` in the command line)

```
wsl.exe -d Ubuntu -- env IMG=IMAGE bash /mnt/f/flippydrive-fw/tools/ws.sh score FUNC 0xADDR --show
wsl.exe -d Ubuntu -- env IMG=IMAGE bash /mnt/f/flippydrive-fw/tools/ws.sh permute FUNC 0xADDR 300
```

IMAGE is `main-ode` or `bootloader`. For a long function, `--show` output is long: pipe it through
`grep -n '^\*'` first and read only the regions that differ.

`score` prints `hits/total size addr first-diff name calls distance N`, then ours beside the
original ('*' marks differing lines). Goal: EXACT and calls distance 0. `permute` writes its
best variant to `permute_out/FUNC.best.c` and does not touch the workspace: copy its function
body into the workspace when it scores higher. Byte match comes first; readability (names,
comments, removing odd temporaries, `__builtin_expect`, `!(!(x))`) is restored after the image
matches, so do not reject a variant for looking unnatural.

If the function is not emitted on its own (inlined into its callers in our build but not in the
original), `score` says so: add `__attribute__((noinline))` only if the original calls it.
For a compiler clone in the original (fewer parameters), score our clone name, e.g.
`put_fat.part.0`, not the source name.

## Method

1. Read the original's code and the ids notes; apply the logic edits. Score. If reshaping our C
   stalls (block order or call order differs), rewrite the function from `ghidra_fn.py`'s output
   instead: keep its statement order, branch structure and conditions, and write it as readable C
   with our types, struct fields and macros. Keep the rewrite only if it scores higher.
2. Read the `--show` diff and reshape: statement order, temporaries, types, condition form,
   early returns. Keep a change only if hits rise.
3. Permuter when stuck (up to three runs, continuing from the best so far). Stop at EXACT, after about 30 score runs, or after 10 runs without
   progress.

## Report (short, plain text)

- Function, address, baseline score, final score (hits/total, EXACT or first diff, calls
  distance).
- Each logic edit made, one line each, with its evidence (address or Ghidra line).
- Any change needed outside the function body.
- What the remaining difference is, in one line.
Do not paste the function body: it is in the workspace.
