// Applies a Screamer symbol map (reference/symbols/*.sym) to the current program.
//
// Line format (whitespace separated, '#' starts a comment line):
//   <hexaddr> f <name> [cc=<convention>] [sig=<C prototype>] [; plate comment]
//   <hexaddr> d <name> [type=<datatype>] [; comment]
//   <hexaddr> l <name> [; comment]
// f = function (created if missing), d = global data label, l = code label.
// cc defaults to __watcall. sig, when given, is a full C prototype applied with the
// Ghidra C parser, e.g.  sig=int fopen_(char *name, char *mode)
// type is any Ghidra data type name such as int, char[64], dword, pointer.
// A name ending in '?' is a guess: the '?' is dropped and the comment says "(unconfirmed)".
//
// Args: [types.h ...] <symbol file> [more symbol files...]
// .h arguments are parsed with Ghidra's C parser first; their structs, typedefs and enums can then
// be used in type= and sig=.
//@category Screamer

import java.io.File;
import java.nio.file.Files;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.util.data.DataTypeParser;
import ghidra.util.data.DataTypeParser.AllowedDataTypes;

public class ApplySymbols extends GhidraScript {

    int funcs, datas, labels, errors;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            printerr("usage: ApplySymbols.java <file.sym> ...");
            return;
        }
        // C headers first, so symbols and signatures can use their types.
        for (String a : args) {
            if (a.endsWith(".h")) applyHeader(new File(a));
        }
        for (String a : args) {
            if (a.endsWith(".h")) continue;
            File f = new File(a);
            List<String> lines = Files.readAllLines(f.toPath());
            int n = 0;
            for (String raw : lines) {
                n++;
                String line = raw.strip();
                if (line.isEmpty() || line.startsWith("#")) continue;
                try {
                    apply(line);
                } catch (Exception e) {
                    errors++;
                    printerr(f.getName() + ":" + n + ": " + e.getMessage() + "  [" + line + "]");
                }
            }
        }
        ScreamerInit.setWatcallEverywhere(currentProgram, monitor);
        println(String.format("ApplySymbols: %d functions, %d data, %d labels, %d errors",
                funcs, datas, labels, errors));
    }

    void apply(String line) throws Exception {
        String comment = null;
        int semi = line.indexOf(';');
        if (semi >= 0) {
            comment = line.substring(semi + 1).strip();
            line = line.substring(0, semi).strip();
        }
        String[] head = line.split("\\s+", 4);
        if (head.length < 3) throw new IllegalArgumentException("need <addr> <kind> <name>");
        Address addr = toAddr(Long.parseLong(head[0].replaceFirst("^0x", ""), 16));
        String kind = head[1], name = head[2];
        if (name.endsWith("?")) { // unconfirmed name: keep it, flag it in the comment
            name = name.substring(0, name.length() - 1);
            comment = "(unconfirmed) " + (comment == null ? "" : comment);
        }
        String rest = head.length > 3 ? head[3] : "";
        String cc = option(rest, "cc"), sig = option(rest, "sig"), type = option(rest, "type");

        switch (kind) {
            case "f" -> {
                Function fn = getFunctionAt(addr);
                if (fn == null) {
                    disassemble(addr);
                    fn = createFunction(addr, name);
                    if (fn == null) throw new IllegalStateException("cannot create function");
                }
                fn.setName(name, SourceType.USER_DEFINED);
                fn.setCallingConvention(cc != null ? cc : "__watcall");
                if (sig != null) applySignature(fn, sig, cc != null ? cc : "__watcall");
                if (comment != null) fn.setComment(comment);
                funcs++;
            }
            case "d", "l" -> {
                Symbol s = getSymbolAt(addr);
                if (s != null && s.getSource() != SourceType.DEFAULT && !s.getName().equals(name)) {
                    s.setName(name, SourceType.USER_DEFINED);
                } else if (s == null || !s.getName().equals(name)) {
                    createLabel(addr, name, true, SourceType.USER_DEFINED);
                }
                if (type != null) {
                    DataTypeParser p = new DataTypeParser(currentProgram.getDataTypeManager(), null, null,
                            AllowedDataTypes.ALL);
                    DataType dt = p.parse(type);
                    clearListing(addr, addr.add(Math.max(dt.getLength(), 1) - 1));
                    createData(addr, dt);
                }
                if (comment != null) setEOLComment(addr, comment);
                if (kind.equals("d")) datas++; else labels++;
            }
            default -> throw new IllegalArgumentException("unknown kind " + kind);
        }
    }

    /** Parses a C header into the program's data type manager (types replace older versions). */
    void applyHeader(File h) throws Exception {
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        ghidra.app.util.cparser.C.CParser p = new ghidra.app.util.cparser.C.CParser(dtm, true, null);
        try (java.io.InputStream in = new java.io.FileInputStream(h)) {
            p.parse(in);
        }
        int n = p.getComposites().size() + p.getTypes().size() + p.getEnums().size();
        println("ApplySymbols: parsed " + h.getName() + " (" + n + " types)");
    }

    void applySignature(Function fn, String sig, String cc) throws Exception {
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        FunctionDefinitionDataType def = CParserUtils.parseSignature((ghidra.app.services.DataTypeManagerService) null,
                currentProgram, sig, true);
        if (def == null) throw new IllegalArgumentException("cannot parse signature");
        def.setCallingConvention(cc);
        ApplyFunctionSignatureCmd cmd = new ApplyFunctionSignatureCmd(fn.getEntryPoint(), def, SourceType.USER_DEFINED);
        if (!cmd.applyTo(currentProgram, monitor)) throw new IllegalStateException(cmd.getStatusMsg());
        fn.setName(fn.getName(), SourceType.USER_DEFINED);
    }

    /** Value of key=... in rest; sig= runs to the end of the line. */
    static String option(String rest, String key) {
        int i = (" " + rest).indexOf(" " + key + "=");
        if (i < 0) return null;
        String v = rest.substring(i + key.length() + 1);
        if (key.equals("sig")) return v.strip();
        int sp = v.indexOf(' ');
        return sp < 0 ? v : v.substring(0, sp);
    }
}
