// Decompile every function in the current program and write C to a file.
// Args: <output .c path>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        ifc.setOptions(opts);
        ifc.toggleCCode(true);
        ifc.openProgram(currentProgram);
        try (PrintWriter pw = new PrintWriter(new BufferedWriter(new FileWriter(out)))) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            int n = 0;
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                if (f.isThunk() || f.isExternal()) continue;
                DecompileResults r = ifc.decompileFunction(f, 60, monitor);
                pw.println("// ==== " + f.getEntryPoint() + " " + f.getName(true));
                if (r != null && r.decompileCompleted()) {
                    pw.println(r.getDecompiledFunction().getC());
                } else {
                    pw.println("// decompile failed");
                }
                if (++n % 2000 == 0) println("decompiled " + n);
            }
            println("done: " + n + " functions");
        }
        ifc.dispose();
    }
}
