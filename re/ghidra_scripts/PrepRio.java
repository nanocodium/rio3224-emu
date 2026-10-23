// Pre-analysis setup for RIO2_PROG.BIN (SH-2A, loaded at 0x0C0F0FF0 so file offset 0x10 == 0x0C0F1000)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;

public class PrepRio extends GhidraScript {
    @Override
    public void run() throws Exception {
        setAnalysisOption(currentProgram, "Aggressive Instruction Finder", "true");
        setAnalysisOption(currentProgram, "Decompiler Switch Analysis", "true");
        Address entry = toAddr(0x0C0F1000L);
        new DisassembleCommand(entry, null, true).applyTo(currentProgram, monitor);
        createFunction(entry, "fw_entry");
        addEntryPoint(entry);
        println("PrepRio done");
    }
}
