"""ar/ELF helpers: yield each executable section of an ARM object with a mask that is 0 over
relocated bytes, so it can be searched for in a linked image."""
import sys, struct, re, pathlib
def ar_members(data):
    o=8; names=b''
    while o<len(data):
        n=data[o:o+16]; size=int(data[o+48:o+58]); body=data[o+60:o+60+size]
        if n.startswith(b'//'): names=body
        elif n.startswith(b'/') and n[1:2].isdigit():
            i=int(n[1:].strip()); yield names[i:names.index(b'/\n',i)].decode(), body
        elif not n.startswith(b'/ '): yield n.split(b'/')[0].decode(), body
        o+=60+size+(size&1)
def sections(elf):
    shoff,=struct.unpack_from('<I',elf,0x20); shentsize,shnum,shstrndx=struct.unpack_from('<HHH',elf,0x2e)
    sh=[struct.unpack_from('<IIIIIIIIII',elf,shoff+i*shentsize) for i in range(shnum)]
    strtab=sh[shstrndx]
    nm=lambda off: elf[strtab[4]+off:elf.index(b'\0',strtab[4]+off)].decode()
    return [(nm(s[0]),)+s for s in sh]
def funcs(elf):
    secs=sections(elf)
    for i,s in enumerate(secs):
        name,_,typ,flags,addr,off,size,link,info,align,entsz=s
        if typ==1 and flags&4 and size>=24:
            code=bytearray(elf[off:off+size]); mask=bytearray(b'\1'*size)
            for r in secs:
                if r[2]==9 and r[8]==i:
                    for k in range(r[6]//8):
                        ro,ri=struct.unpack_from('<II',elf,r[5]+k*8)
                        for b in range(4):
                            if ro+b<size: mask[ro+b]=0
            yield name,bytes(code),bytes(mask)
