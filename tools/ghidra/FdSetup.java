// Pre-analysis setup for a FlippyDrive RP2040 image imported as raw binary at its load address.
// Adds the RAM regions crt0 copies from flash, the rest of SRAM, the RP2040 peripheral
// regions, sets Thumb mode everywhere, names the vector table entries and applies library
// symbols from a symmatch.py TSV (functions found there are tagged LIB).
// Script argument: path of the TSV.
// @category FlippyDrive

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.FileReader;
import java.math.BigInteger;
import java.util.ArrayList;
import java.util.List;

public class FdSetup extends GhidraScript {
    private AddressSpace space;

    private long u32(byte[] b, int o) {
        return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16
            | (b[o + 3] & 0xffL) << 24;
    }

    private Address addr(long a) {
        return space.getAddress(a);
    }

    private void uninit(String name, long start, long size, boolean volatil) throws Exception {
        MemoryBlock b = currentProgram.getMemory().createUninitializedBlock(name, addr(start), size, false);
        b.setPermissions(true, true, false);
        b.setVolatile(volatil);
    }

    private static final String[] VECTORS = {"__StackTop", "_reset_handler", "isr_nmi",
        "isr_hardfault", null, null, null, null, null, null, null, "isr_svcall", null, null,
        "isr_pendsv", "isr_systick"};

    @Override
    public void run() throws Exception {
        space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Memory mem = currentProgram.getMemory();
        MemoryBlock flash = mem.getBlocks()[0];
        flash.setName("FLASH");
        flash.setPermissions(true, false, true);
        long base = flash.getStart().getOffset();
        byte[] img = new byte[(int) flash.getSize()];
        flash.getBytes(flash.getStart(), img);

        // binary_info header: marker, start, end, data_cpy_table
        int bi = -1;
        for (int i = 0; i + 4 <= img.length; i += 4) {
            if (u32(img, i) == 0x7188ebf2L) { bi = i; break; }
        }
        long table = u32(img, bi + 12) - base;
        long tableAddr = base + table;
        List<long[]> ram = new ArrayList<>();
        int n = 0;
        for (;; table += 12) {
            long src = u32(img, (int) table), dst = u32(img, (int) table + 4), end = u32(img, (int) table + 8);
            if (src == 0) break;
            if (end <= dst) continue;
            byte[] data = new byte[(int) (end - dst)];
            System.arraycopy(img, (int) (src - base), data, 0, data.length);
            MemoryBlock b = mem.createInitializedBlock("RAM_COPY" + n++, addr(dst),
                new ByteArrayInputStream(data), data.length, monitor, false);
            b.setPermissions(true, true, true);
            b.setComment(String.format("copied by crt0 from flash 0x%08x", src));
            ram.add(new long[] {dst, end});
        }
        // the rest of SRAM (0x20000000-0x20042000), uninitialised
        ram.sort((x, y) -> Long.compare(x[0], y[0]));
        long pos = 0x20000000L;
        int g = 0;
        for (long[] r : ram) {
            if (r[0] > pos) uninit("RAM" + g++, pos, r[0] - pos, false);
            pos = Math.max(pos, r[1]);
        }
        if (pos < 0x20042000L) uninit("RAM" + g, pos, 0x20042000L - pos, false);

        uninit("BOOTROM", 0x00000000L, 0x4000, false);
        uninit("XIP_SSI", 0x18000000L, 0x100, true);
        uninit("APB", 0x40000000L, 0x70000, true);
        uninit("AHB", 0x50000000L, 0x400000, true);
        uninit("SIO", 0xd0000000L, 0x200, true);
        uninit("PPB", 0xe0000000L, 0x10000, true);

        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        for (MemoryBlock b : mem.getBlocks()) {
            if (b.isExecute() || b.getName().startsWith("RAM")) {
                currentProgram.getProgramContext().setValue(tmode, b.getStart(), b.getEnd(), BigInteger.ONE);
            }
        }

        // vector table: first word 0x20042000 followed by a Thumb pointer into this image
        int vt = -1;
        for (int i = 0; i + 8 <= 0x200 && i + 8 <= img.length; i += 4) {
            if (u32(img, i) == 0x20042000L && (u32(img, i + 4) & 1) == 1) { vt = i; break; }
        }
        createLabel(addr(base + vt), "__vectors", true, SourceType.USER_DEFINED);
        for (int v = 1; v < 48; v++) {
            long target = u32(img, vt + v * 4) & ~1L;
            String name = v < 16 ? VECTORS[v] : "isr_irq" + (v - 16);
            if (name == null || target == 0) continue;
            Function f = getFunctionAt(addr(target));
            if (f == null) {
                disassemble(addr(target));
                f = createFunction(addr(target), name);
            }
            // handlers inside crt0 (before data_cpy_table) are the SDK's bkpt stubs
            if (f != null && target < tableAddr) f.addTag("LIB");
        }

        // crt0 literal pool after the copy table's terminator: VTOR, SIO, bss start, bss end,
        // runtime_init, main, exit
        int lit = (int) table + 4;
        while (u32(img, lit) != 0xe000ed08L) lit += 4;
        String[] entries = {"runtime_init", "main", "exit"};
        for (int i = 0; i < 3; i++) {
            Address a = addr(u32(img, lit + 16 + 4 * i) & ~1L);
            disassemble(a);
            Function f = getFunctionAt(a);
            if (f == null) f = createFunction(a, entries[i]);
            else f.setName(entries[i], SourceType.USER_DEFINED);
            if (f != null && i != 1) f.addTag("LIB");
        }

        // library symbols
        int applied = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] p = line.split("\t");
                Address a = addr(Long.decode(p[0]));
                String[] names = p[1].split("\\|");
                disassemble(a);
                Function f = getFunctionAt(a);
                if (f == null) f = createFunction(a, names[0]);
                if (f == null) continue;
                f.setName(names[0], SourceType.IMPORTED);
                for (int i = 1; i < names.length; i++) createLabel(a, names[i], false, SourceType.IMPORTED);
                f.addTag("LIB");
                if (!p[3].equals("1")) f.setComment("identical code also matched at other addresses: " + p[1]);
                if (p[4].startsWith("near:")) f.setComment("near match (" + p[4] + " windows), not byte-identical to the vendored library");
                applied++;
            }
        }
        println("FdSetup: RAM copies " + ram.size() + ", vector table at 0x"
            + Long.toHexString(base + vt) + ", library functions " + applied);
    }
}
