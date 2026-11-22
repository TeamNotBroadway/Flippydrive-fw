"""Find the configuration a library was built with, by greedy search.

Compiles the given C sources with a config header whose #define values are varied one at a
time, and keeps a change when more of the resulting function bytes are found verbatim in
the image (relocations masked; see symmatch.py). Run inside WSL.

Usage: confsweep.py SPEC.json IMAGE.bin BASE [--fuzzy]
--fuzzy scores by matching 8-byte windows instead of whole functions (see score_fuzzy).
SPEC: {"src_dir": ..., "sources": [...], "config": "ffconf.h",
       "fixed": {"NAME": "value"}, "options": {"NAME": ["v1", "v2", ...]},
       "cflags": [...], "base_flags": [...] (replaces BASE_FLAGS)}
Prints the best configuration and its score; writes BEST.o next to the spec.
"""
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from symmatch import elf_functions, segments, MIN_FIXED  # noqa: E402

GCC = '/mnt/c/Tools/arch-arm-none-eabi-14.2.0/bin/arm-none-eabi-gcc'
BASE_FLAGS = ['-mcpu=cortex-m0plus', '-mthumb', '-O3', '-DNDEBUG',
              '-ffunction-sections', '-fdata-sections', '-c']


def score(objs, segs):
    total, names = 0, []
    for obj in objs:
        for name, code, mask in elf_functions(obj.read_bytes()):
            if sum(mask) < MIN_FIXED:
                continue
            pat = re.compile(b''.join(re.escape(bytes([c])) if m else b'.'
                                      for c, m in zip(code, mask)), re.S)
            if any(pat.search(data) for _, data in segs):
                total += len(code)
                names.append(name)
    return total, names


WINDOW = 8


def window_set(segs):
    return {data[i:i + WINDOW] for _, data in segs for i in range(0, len(data) - WINDOW + 1, 2)}


def score_fuzzy(objs, windows):
    """Count fully non-relocated 8-byte windows of the compiled code found in the image:
    rises gradually as a configuration gets closer, unlike whole-function matches."""
    total = 0
    for obj in objs:
        for _, code, mask in elf_functions(obj.read_bytes()):
            for i in range(0, len(code) - WINDOW + 1, 2):
                if all(mask[i:i + WINDOW]) and code[i:i + WINDOW] in windows:
                    total += 1
    return total


def build(spec, conf, work):
    hdr = (work / spec['config']).read_text()
    for k, v in conf.items():
        hdr, n = re.subn(rf'(#define\s+{k}\s+)\S+', rf'\g<1>{v}', hdr)
        if n == 0:
            hdr += f'\n#define {k} {v}\n'
    (work / spec['config']).write_text(hdr)
    objs = []
    for s in spec['sources']:
        o = work / (pathlib.Path(s).stem + '.o')
        r = subprocess.run([GCC] + spec.get('base_flags', BASE_FLAGS) + spec.get('cflags', []) + ['-I', str(work),
                            str(work / s), '-o', str(o)], capture_output=True, text=True)
        if r.returncode:
            return None
        objs.append(o)
    return objs


def main():
    spec_path = pathlib.Path(sys.argv[1])
    spec = json.loads(spec_path.read_text())
    segs = segments(pathlib.Path(sys.argv[2]).read_bytes(), int(sys.argv[3], 0))
    fuzzy = '--fuzzy' in sys.argv
    windows = window_set(segs) if fuzzy else None
    tmp = pathlib.Path(tempfile.mkdtemp())
    pristine = tmp / 'pristine'
    shutil.copytree(spec['src_dir'], pristine)

    def evaluate(conf):
        work = tmp / 'w'
        shutil.rmtree(work, ignore_errors=True)
        shutil.copytree(pristine, work)
        objs = build(spec, conf, work)
        if objs is None:
            return -1, [], None
        s, names = score(objs, segs)
        if fuzzy:
            s = score_fuzzy(objs, windows)
        return s, names, objs

    conf = dict(spec.get('fixed', {}))
    for k, vals in spec['options'].items():
        conf.setdefault(k, vals[0])
    best, names, _ = evaluate(conf)
    print('start', best)
    improved = True
    while improved:
        improved = False
        for k, vals in spec['options'].items():
            for v in vals:
                if v == conf[k]:
                    continue
                trial = dict(conf, **{k: v})
                s, n, _ = evaluate(trial)
                if s > best:
                    best, names, conf = s, n, trial
                    improved = True
                    print(f'  {k}={v} -> {best}', flush=True)
    s, names, objs = evaluate(conf)
    for o in objs:
        shutil.copy(o, spec_path.with_name(spec_path.stem + '.' + o.name))
    print(json.dumps({'score': best, 'exact_functions': sorted(names), 'config': conf}))
    shutil.rmtree(tmp)


if __name__ == '__main__':
    main()
