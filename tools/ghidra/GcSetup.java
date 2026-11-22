// Pre-analysis setup for the GameCube images (ELF wrappers from tools/dol2elf.py): applies
// library symbols from a symmatch.py-format TSV if it exists (functions found there are
// tagged LIB, so FdExport skips them).
// Script argument: path of the TSV (may be missing).
// @category FlippyDrive

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

public class GcSetup extends GhidraScript {
    @Override
    public void run() throws Exception {
        File tsv = new File(getScriptArgs()[0]);
        int applied = 0;
        if (tsv.exists()) {
            try (BufferedReader r = new BufferedReader(new FileReader(tsv))) {
                String line;
                while ((line = r.readLine()) != null) {
                    String[] p = line.split("\t");
                    Address a = toAddr(Long.decode(p[0]));
                    String[] names = p[1].split("\\|");
                    disassemble(a);
                    Function f = getFunctionAt(a);
                    if (f == null) f = createFunction(a, names[0]);
                    if (f == null) continue;
                    f.setName(names[0], SourceType.IMPORTED);
                    for (int i = 1; i < names.length; i++) createLabel(a, names[i], false, SourceType.IMPORTED);
                    f.addTag("LIB");
                    if (!p[3].equals("1")) f.setComment("identical code also matched at other addresses: " + p[1]);
                    if (p.length > 4 && p[4].startsWith("near:")) f.setComment("near match (" + p[4] + " windows)");
                    applied++;
                }
            }
        }
        println("GcSetup: library functions " + applied);
    }
}
