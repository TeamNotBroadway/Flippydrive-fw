import json, re
BS = chr(92)
import sys
# Usage, from a CMake build's parent dir: gen_wsl_compile.py WSL_PATH_OF_THAT_DIR
# Replays build/compile_commands.json with the Arch gcc into build_arch/ (copy build/generated
# there first, with C:/ rewritten to /mnt/c/, and point -I at it).
ROOT = sys.argv[1]
c = json.load(open('build/compile_commands.json'))

def wsl(p):
    p = p.replace(BS, '/')
    return re.sub(r'([A-Za-z]):/', lambda m: '/mnt/' + m.group(1).lower() + '/', p)

out = ['set -e']
for e in c:
    exe, rest = e['command'].split(' ', 1)
    tool = 'arm-none-eabi-g++' if 'g++' in exe else 'arm-none-eabi-gcc'
    # keep escaped quotes in -D values: they arrive as \"pico\"; protect them before slash conversion
    rest = rest.replace(BS + '"', '\x00')
    rest = wsl(rest).replace('\x00', BS + '"')
    rest = re.sub(r'-MD -MT \S+ -MF \S+', '', rest)
    rest = re.sub(r'-o (\S+)', lambda m: '-o ' + ROOT + '/build_arch/' + m.group(1), rest)
    o = re.search(r'-o (\S+)', rest).group(1)
    out.append(f'mkdir -p "$(dirname {o})"; cd {wsl(e["directory"])}; /mnt/c/Tools/arch-arm-none-eabi-14.2.0/bin/{tool} {rest}')
out.append(f'echo done $(find {ROOT}/build_arch -name "*.obj" | wc -l)')
open('compile_arch.sh', 'w', newline='\n').write('\n'.join(out) + '\n')
print(out[1][:200])
