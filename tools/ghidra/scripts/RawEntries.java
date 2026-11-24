// Disassembles a raw binary from the given entry points and makes functions there.
// Used for blobs without a container format (e.g. an HMI sound driver cut out of HMIMDRV.386).
// Args: <hexaddr> ...
//@category Screamer

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class RawEntries extends GhidraScript {
    @Override
    protected void run() throws Exception {
        for (String a : getScriptArgs()) {
            Address at = toAddr(Long.parseLong(a.replace("0x", ""), 16));
            new DisassembleCommand(at, null, true).applyTo(currentProgram, monitor);
            if (getFunctionAt(at) == null) createFunction(at, null);
        }
        analyzeAll(currentProgram);
    }
}
