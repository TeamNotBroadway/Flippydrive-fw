// Decompile the function at each address given as a script argument, printing C to the log.
// Force-creates a clean function at the exact address (removing any overlapping one), so a
// wrong library-symbol boundary does not corrupt the result.
// @category FlippyDrive

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class FdDecompAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String s : getScriptArgs()) {
            Address a = toAddr(Long.parseLong(s.replace("0x", ""), 16));
            Function f = getFunctionAt(a);
            if (f == null) {
                Function c = getFunctionContaining(a);
                if (c != null && !c.getEntryPoint().equals(a)) {
                    removeFunction(c);
                }
                f = createFunction(a, null);
            }
            println("==== " + s + "  " + (f != null ? f.getName() : "(none)") + " ====");
            if (f == null) { println("could not create a function at " + s); continue; }
            DecompileResults r = di.decompileFunction(f, 180, monitor);
            println(r.decompileCompleted() ? r.getDecompiledFunction().getC()
                                           : "decompile failed: " + r.getErrorMessage());
            println("==== end " + s + " ====");
        }
        di.dispose();
    }
}
