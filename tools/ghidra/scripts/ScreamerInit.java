// One-time setup for a Screamer executable imported from tools/le/le2elf.py output.
//  * installs the Watcom register calling convention (__watcall) as a compiler-spec extension
//  * marks every 32-bit LE fixup that lies outside an instruction as a pointer, and
//    creates functions at fixup targets that are referenced from code-pointer tables
//
// Args: path to the <name>.fixups.json written by le2elf.py (default: next to the imported file).
//@category Screamer

import java.io.File;
import java.nio.file.Files;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.database.SpecExtension;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

public class ScreamerInit extends GhidraScript {

    // Watcom C/C++ 10.x default 32-bit register convention: args in EAX, EDX, EBX, ECX,
    // then on the stack (callee pops). Return in EAX (EDX:EAX for 64-bit, ST0 for floats
    // with -fpi87). The callee preserves every register except the return registers.
    static final String WATCALL = """
        <prototype name="__watcall" extrapop="unknown" stackshift="4">
          <input>
            <pentry minsize="1" maxsize="4"><register name="EAX"/></pentry>
            <pentry minsize="1" maxsize="4"><register name="EDX"/></pentry>
            <pentry minsize="1" maxsize="4"><register name="EBX"/></pentry>
            <pentry minsize="1" maxsize="4"><register name="ECX"/></pentry>
            <pentry minsize="1" maxsize="500" align="4"><addr offset="4" space="stack"/></pentry>
          </input>
          <output>
            <pentry minsize="4" maxsize="10" metatype="float"><register name="ST0"/></pentry>
            <pentry minsize="1" maxsize="4"><register name="EAX"/></pentry>
            <pentry minsize="5" maxsize="8"><addr space="join" piece1="EDX" piece2="EAX"/></pentry>
          </output>
          <unaffected>
            <register name="EBX"/><register name="ECX"/><register name="EDX"/>
            <register name="ESI"/><register name="EDI"/><register name="EBP"/>
            <register name="ESP"/><register name="DF"/>
          </unaffected>
        </prototype>
        """;

    @Override
    protected void run() throws Exception {
        installWatcall();
        String path = getScriptArgs().length > 0 ? getScriptArgs()[0]
                : currentProgram.getExecutablePath().replaceAll("\\.elf$", ".fixups.json");
        if (path.startsWith("/") && path.matches("/[A-Za-z]:.*")) path = path.substring(1);
        File f = new File(path);
        if (!f.exists()) {
            printerr("fixups file not found: " + path);
            return;
        }
        applyFixups(Files.readString(f.toPath()));
        createOrphanFunctions();
        setWatcallEverywhere(currentProgram, monitor);
    }

    /** Watcom compiles nearly everything with the register convention. */
    static int setWatcallEverywhere(ghidra.program.model.listing.Program p, ghidra.util.task.TaskMonitor mon)
            throws Exception {
        int n = 0;
        for (ghidra.program.model.listing.Function fn : p.getFunctionManager().getFunctions(true)) {
            if (mon.isCancelled()) break;
            String cc = fn.getCallingConventionName();
            if (fn.hasVarArgs() || "__cdecl".equals(cc) && fn.getSignatureSource() == ghidra.program.model.symbol.SourceType.USER_DEFINED) continue;
            if (!"__watcall".equals(cc)) { fn.setCallingConvention("__watcall"); n++; }
        }
        return n;
    }

    /**
     * The renderer's hand-written assembly is mostly reached through jump/call tables and
     * patched code, so auto-analysis disassembles it but creates no functions. Start a
     * function at every instruction that is outside any function and does not follow on
     * from a previous orphan instruction (i.e. after a RET/JMP, data or a gap).
     */
    void createOrphanFunctions() throws Exception {
        ghidra.program.model.listing.Listing listing = currentProgram.getListing();
        int created = 0;
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (!b.isExecute()) continue;
            Instruction prev = null;
            for (Instruction ins : listing.getInstructions(b.getStart(), true)) {
                if (monitor.isCancelled() || !b.contains(ins.getAddress())) break;
                boolean orphan = getFunctionContaining(ins.getAddress()) == null;
                boolean fallsIn = prev != null && prev.getFallThrough() != null
                        && prev.getFallThrough().equals(ins.getAddress())
                        && getFunctionContaining(prev.getAddress()) == null;
                if (orphan && !fallsIn) {
                    if (createFunction(ins.getAddress(), null) != null) created++;
                }
                prev = ins;
            }
        }
        println("orphan code blocks turned into functions: " + created);
    }

    void installWatcall() throws Exception {
        SpecExtension ext = new SpecExtension(currentProgram);
        ext.addReplaceCompilerSpecExtension(WATCALL, monitor);
        println("installed __watcall calling convention");
    }

    void applyFixups(String json) throws Exception {
        // Only 32-bit offset fixups. In the code object, a lone fixup outside known instructions
        // is usually an operand of code that analysis has not reached yet (the renderer), so
        // marking it as a pointer would block disassembly there. Only runs of adjacent fixups
        // (pointer/jump tables) are marked in executable blocks; everything is marked in data.
        Pattern p = Pattern.compile("\\{\"src\": (\\d+), \"type\": (\\d+), \"target\": (\\d+), \"target_obj\": (\\d+)\\}");
        Matcher m = p.matcher(json);
        java.util.TreeSet<Long> srcs = new java.util.TreeSet<>();
        while (m.find()) {
            if (Integer.parseInt(m.group(2)) == 7) srcs.add(Long.parseLong(m.group(1)));
        }
        int pointers = 0, skipped = 0;
        for (long src : srcs) {
            if (monitor.isCancelled()) break;
            Address a = toAddr(src);
            MemoryBlock blk = currentProgram.getMemory().getBlock(a);
            boolean inTable = srcs.contains(src - 4) || srcs.contains(src + 4);
            if (blk == null || (blk.isExecute() && (!inTable || (src & 3) != 0))) { skipped++; continue; }
            Instruction ins = currentProgram.getListing().getInstructionContaining(a);
            if (ins != null) { skipped++; continue; }
            Data d = currentProgram.getListing().getDefinedDataContaining(a);
            if (d != null && !(d.getAddress().equals(a) && d.getLength() == 4)) {
                if (d.isPointer() || d.hasStringValue() || d.isArray() || d.isStructure()) continue;
            }
            try {
                clearListing(a, a.add(3));
                createData(a, PointerDataType.dataType);
                pointers++;
            } catch (Exception e) {
                skipped++;
            }
        }
        println("fixup pointers created: " + pointers + ", skipped (code operands etc.): " + skipped);
    }

}
