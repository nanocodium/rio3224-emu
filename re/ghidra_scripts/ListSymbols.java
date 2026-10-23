// Print address, size and name of every symbol whose name contains one of
// the substrings given as script arguments (case sensitive).
//   analyzeHeadless <proj> <name> -process TLDStageBox -noanalysis
//       -scriptPath <dir> -postScript ListSymbols.java FDSP_CODE SEQUENCE
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class ListSymbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext()) {
            Symbol s = it.next();
            String n = s.getName(true);
            boolean hit = args.length == 0;
            for (String a : args) if (n.contains(a)) hit = true;
            if (!hit) continue;
            Data d = getDataAt(s.getAddress());
            long size = d != null ? d.getLength() : 0;
            println(String.format("SYM %s size 0x%x %s", s.getAddress(), size, n));
        }
    }
}
