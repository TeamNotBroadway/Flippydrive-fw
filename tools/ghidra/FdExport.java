// Post-analysis export: decompile every function not tagged LIB into OUTDIR/app.c and list
// all functions in OUTDIR/functions.tsv (address, size, name, lib/app).
// Script argument: OUTDIR.
// @category FlippyDrive

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionTag;
import java.io.File;
import java.io.PrintWriter;

public class FdExport extends GhidraScript {
    private boolean isLib(Function f) {
        for (FunctionTag t : f.getTags()) if (t.getName().equals("LIB")) return true;
        return false;
    }

    @Override
    public void run() throws Exception {
        File out = new File(getScriptArgs()[0]);
        out.mkdirs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        int app = 0, lib = 0, failed = 0;
        long appBytes = 0, libBytes = 0;
        try (PrintWriter c = new PrintWriter(new File(out, "app.c"), "UTF-8");
             PrintWriter t = new PrintWriter(new File(out, "functions.tsv"), "UTF-8")) {
            c.println("// Ghidra decompilation of " + currentProgram.getName()
                + " (functions not matched to a library). Starting point only.");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) break;
                long size = f.getBody().getNumAddresses();
                boolean l = isLib(f);
                t.printf("0x%08x\t%d\t%s\t%s%n", f.getEntryPoint().getOffset(), size, f.getName(), l ? "lib" : "app");
                if (l) { lib++; libBytes += size; continue; }
                app++; appBytes += size;
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                c.printf("%n// 0x%08x, %d bytes%n", f.getEntryPoint().getOffset(), size);
                if (r.decompileCompleted()) {
                    c.print(r.getDecompiledFunction().getC());
                } else {
                    failed++;
                    c.println("// decompile failed: " + r.getErrorMessage());
                }
            }
        }
        di.dispose();
        println(String.format("FdExport: app %d functions / %d bytes (%d failed), lib %d / %d bytes",
            app, appBytes, failed, lib, libBytes));
    }
}
