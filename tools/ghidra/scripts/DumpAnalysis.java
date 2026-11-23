// Writes analysis dumps of the current program for offline reading / grepping.
// Output stays under artifacts/ (it is derived from the original executable).
//
// Args: <outdir> [decompile=all|none|named|<hexaddr,...>]
//   <outdir>/functions.tsv : addr, name, size, callers, callees, ints, ports, strings
//   <outdir>/decomp.c      : decompiled C for the selected functions
//@category Screamer

import java.io.File;
import java.io.PrintWriter;
import java.util.*;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class DumpAnalysis extends GhidraScript {

    /** A printable NUL-terminated string of 3+ chars at a, or null. */
    String cString(Address a) {
        try {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 200; i++) {
                int c = currentProgram.getMemory().getByte(a.add(i)) & 0xff;
                if (c == 0) return sb.length() >= 3 ? sb.toString() : null;
                if (c == 9 || c == 10 || c == 13) { sb.append(' '); continue; }
                if (c < 0x20 || c > 0x7e) return null;
                sb.append((char) c);
            }
        } catch (Exception e) { }
        return null;
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : "artifacts/ghidra/dump");
        out.mkdirs();
        String mode = args.length > 1 ? args[1] : "all";

        Listing listing = currentProgram.getListing();
        ReferenceManager refs = currentProgram.getReferenceManager();
        try (PrintWriter w = new PrintWriter(new File(out, "functions.tsv"), "UTF-8")) {
            w.println("addr\tname\tsize\tcallers\tcallees\tints\tports\tstrings");
            for (Function f : listing.getFunctions(true)) {
                Set<Function> callers = f.getCallingFunctions(monitor);
                Set<Function> callees = f.getCalledFunctions(monitor);
                TreeSet<String> ints = new TreeSet<>(), ports = new TreeSet<>(), strs = new TreeSet<>();
                for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                    String mn = ins.getMnemonicString();
                    if (mn.equals("INT")) {
                        Scalar s = ins.getScalar(0);
                        ints.add(s == null ? "?" : String.format("%02x", s.getUnsignedValue()));
                    } else if (mn.equals("IN") || mn.equals("OUT")) {
                        for (int i = 0; i < ins.getNumOperands(); i++) {
                            Scalar s = ins.getScalar(i);
                            if (s != null) ports.add(String.format("%x", s.getUnsignedValue()));
                            else if (ins.getDefaultOperandRepresentation(i).equals("DX")) ports.add("dx");
                        }
                    }
                    for (Reference r : ins.getReferencesFrom()) {
                        String v = cString(r.getToAddress());
                        if (v != null) strs.add(v.length() > 40 ? v.substring(0, 40) : v);
                    }
                }
                w.printf("%s\t%s\t%d\t%d\t%d\t%s\t%s\t%s%n", f.getEntryPoint(), f.getName(),
                        f.getBody().getNumAddresses(), callers.size(), callees.size(),
                        String.join(",", ints), String.join(",", ports),
                        String.join(" | ", strs).replace('\t', ' ').replace('\n', ' ').replace('\r', ' '));
            }
        }
        try (PrintWriter w = new PrintWriter(new File(out, "coverage.txt"), "UTF-8")) {
            for (ghidra.program.model.mem.MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                long ins = 0, dat = 0, und = 0;
                Address runStart = null;
                List<String> gaps = new ArrayList<>();
                Address a = b.getStart();
                while (a != null && a.compareTo(b.getEnd()) <= 0) {
                    CodeUnit cu = listing.getCodeUnitAt(a);
                    int len = cu == null ? 1 : cu.getLength();
                    boolean undef = cu == null || (cu instanceof Data d && !d.isDefined());
                    if (cu instanceof Instruction) ins += len; else if (undef) und += len; else dat += len;
                    if (undef && runStart == null) runStart = a;
                    if (!undef && runStart != null) {
                        long n = a.subtract(runStart);
                        if (n >= 64) gaps.add(runStart + "	" + n);
                        runStart = null;
                    }
                    a = a.add(len);
                    if (a.compareTo(b.getEnd()) > 0 || a.getOffset() == 0) break;
                }
                w.printf("%s %s: instr %d data %d undefined %d%n", b.getName(), b.getStart(), ins, dat, und);
                for (String g : gaps) w.println("  gap " + g);
            }
        }
        try (PrintWriter w = new PrintWriter(new File(out, "calls.tsv"), "UTF-8")) {
            for (Function f : listing.getFunctions(true)) {
                for (Function c : f.getCalledFunctions(monitor)) {
                    w.println(f.getEntryPoint() + "	" + c.getEntryPoint());
                }
            }
        }
        try (PrintWriter w = new PrintWriter(new File(out, "disasm.txt"), "UTF-8")) {
            for (Function f : listing.getFunctions(true)) {
                w.println("; ===== " + f.getEntryPoint() + " " + f.getName() + " " + f.getCallingConventionName());
                for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                    StringBuilder sb = new StringBuilder();
                    sb.append(ins.getAddress()).append("  ").append(ins.toString());
                    for (Reference r : ins.getReferencesFrom()) {
                        if (!r.getReferenceType().isFlow() || r.getReferenceType().isCall()) {
                            Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(r.getToAddress());
                            String v = cString(r.getToAddress());
                            sb.append("  ; ").append(s != null ? s.getName() : r.getToAddress().toString());
                            if (v != null) sb.append(" \"").append(v.length() > 60 ? v.substring(0, 60) : v).append('"');
                        }
                    }
                    w.println(sb);
                }
            }
        }
        if (mode.equals("none")) return;

        DecompInterface di = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        di.setOptions(opts);
        di.openProgram(currentProgram);
        Set<Address> only = null;
        if (!mode.equals("all") && !mode.equals("named")) {
            only = new HashSet<>();
            for (String a : mode.split(",")) only.add(toAddr(a));
        }
        try (PrintWriter w = new PrintWriter(new File(out, "decomp.c"), "UTF-8")) {
            for (Function f : listing.getFunctions(true)) {
                if (monitor.isCancelled()) break;
                if (only != null && !only.contains(f.getEntryPoint())) continue;
                if (mode.equals("named") && f.getName().startsWith("FUN_")) continue;
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                w.println("// ===== " + f.getEntryPoint() + " " + f.getName());
                if (r != null && r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
                else w.println("// decompile failed: " + (r == null ? "" : r.getErrorMessage()));
            }
        }
        di.dispose();
    }
}
