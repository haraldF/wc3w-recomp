// Statically recompiles Wing Commander III's Windows version (WC3W.EXE and XANLIB.DLL, linked into one
// flat 32-bit image by cli.ts) to C++: every instruction's p-code becomes C++ operating on an
// emulated x86 state (runtime/wc_rt.h): R[] holds Ghidra's register space, M[] the memory. The image
// is imported at its address, so code addresses are the program's own.
//  - Each Ghidra function becomes "void F_<address>(uint32_t at)", entered at any of its
//    instructions via a switch; branches inside it are gotos, anything else goes through run().
//  - Calls become C calls (the p-code still pushes the return address, and RET pops it). Calls of
//    Windows functions go through the import slots (CALL [slot], JMP [slot]) to run(), which
//    recognises their thunk addresses.
//  - INT n becomes cpu_int(n); IN/OUT become port_in/port_out (neither should happen in Windows).
//  - Ghidra ignores segments in 32-bit code: memory accesses add the base of their segment's
//    selector (wc_sb[]: all 0 here but FS, the thread information block), segment register loads go
//    through seg_load().
//  - x87 values are carried as doubles (x87_* in wc_rt.h).
// Started as wc3-recomp's generator (the DOS version), without its drivers loaded at run time.
// Writes <out>/seg_*.cpp (16 KB of code each), <out>/wc_dispatch.cpp, <out>/wc_gen.h, <out>/image.bin and
// <out>/summary.json (the counts, and the hooks that aren't at an instruction: cli.ts checks them).
// Code reached only through pointers in data has no Ghidra function; list such entry points (hex,
// one per line) in the entries file and they get recompiled too, along with any functions they
// call. The runtime reports such addresses ("npm run discover").
// Code pointers (hex, one per line: the targets of the image's base relocations that are in code)
// outside every Ghidra function are decoded the same way.
// Hooks (hex, one per line, "#" comments): at these instructions the code first asks the runtime
// (wc_hook) whether it handles them itself (e.g. a function it replaces), and returns if so. The
// hooked instructions are listed for the runtime (wc_hook_addr), which checks its handlers against them.
// Args: <output dir> <entries file> [<code pointers file> [<hooks file>]]
//@category WC

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.PseudoDisassembler;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.Reference;

public class WcRecomp extends GhidraScript {

	private final Map<Long, String> entries = new HashMap<>(); // entry address -> C function name
	private int farJumpFixes, iretFixes, lslFixes, unhandled;
	/** unique offset -> address varnode it was loaded from, within the current instruction */
	private final Map<Long, Varnode> loadAddrOf = new HashMap<>();

	/** Per instruction: its segment override prefix (index into wc_sb[]: ES 0, CS 1, SS 2, DS 3, FS 4,
	 * GS 5) or -1, whether it is a string instruction, and the uniques holding EDI */
	private int override = -1;
	/** whether the instruction's memory operand is based on EBP or ESP, so in SS by default */
	private boolean stackBased;
	/** Per instruction: LDS/LES/LFS/LGS/LSS, whose far pointer Ghidra loads like a stack access. */
	private boolean farLoad;
	private boolean stringOp;
	private final Set<Long> ediUniques = new HashSet<>();

	/** Addresses of the code's own bytes that it writes with stores to fixed addresses (self-modifying
	 * code: in the DOS version, the texture mapper's row table address patched into its
	 * displacements); the instruction being translated */
	private final Set<Long> patched = new HashSet<>();
	private Instruction current;
	private final Set<Long> hooks = new TreeSet<>();
	/** the hooks whose instruction was emitted (each must be: the summary lists the others) */
	private final Set<Long> hooked = new TreeSet<>();

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		Path out = Path.of(args[0]);
		Path entriesFile = Path.of(args[1]);
		Files.createDirectories(out);
		List<Function> functions = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			functions.add(f);
			entries.put(f.getEntryPoint().getOffset(), fname(f.getEntryPoint().getOffset()));
		}
		List<Long> seeds = new ArrayList<>();
		if (Files.exists(entriesFile)) {
			for (String line : Files.readAllLines(entriesFile)) {
				line = line.replaceAll("#.*", "").trim();
				if (!line.isEmpty() && currentProgram.getMemory().contains(toAddr(Long.parseLong(line, 16)))) {
					seeds.add(Long.parseLong(line, 16));
				}
			}
		}
		// Jumps, calls and fall-throughs that leave every function: decode those too.
		int gaps = 0;
		for (Function f : functions) {
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				List<Address> targets = new ArrayList<>(List.of(ins.getFlows()));
				if (ins.getFallThrough() != null) {
					targets.add(ins.getFallThrough());
				}
				for (Address t : targets) {
					if (!recompiled(t) && currentProgram.getMemory().contains(t)) {
						seeds.add(t.getOffset());
						gaps++;
					}
				}
			}
		}
		println("WcRecomp: " + gaps + " targets of functions outside every function");
		int pointers = 0;
		if (args.length > 2 && Files.exists(Path.of(args[2]))) {
			for (String line : Files.readAllLines(Path.of(args[2]))) {
				line = line.trim();
				if (line.isEmpty()) {
					continue;
				}
				Address a = toAddr(Long.parseLong(line, 16));
				if (currentProgram.getMemory().contains(a) && getFunctionContaining(a) == null) {
					seeds.add(a.getOffset());
					pointers++;
				}
			}
		}
		println("WcRecomp: " + pointers + " code pointers outside every function");
		if (args.length > 3 && Files.exists(Path.of(args[3]))) {
			for (String line : Files.readAllLines(Path.of(args[3]))) {
				line = line.replaceAll("#.*", "").trim();
				if (!line.isEmpty()) {
					hooks.add(Long.parseLong(line, 16));
				}
			}
		}
		List<Fragment> fragments = fragments(seeds);
		for (Fragment fr : fragments) {
			entries.put(fr.entry, fname(fr.entry));
		}
		findPatches(functions, fragments);

		Map<String, StringBuilder> files = new TreeMap<>();
		// address of every emitted instruction -> C function, for run()
		TreeMap<Long, String> owner = new TreeMap<>();
		monitor.initialize(functions.size());
		for (Function f : functions) {
			monitor.checkCancelled();
			monitor.incrementProgress(1);
			List<Instruction> body = new ArrayList<>();
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				body.add(ins);
			}
			long entry = f.getEntryPoint().getOffset();
			emitFunction(fname(entry), f.getName() + " @ " + f.getEntryPoint(), body, fileFor(entry, files), owner);
		}
		for (Fragment fr : fragments) {
			emitFunction(fname(fr.entry), "discovered entry point @ " + fr.body.get(0).getAddress(), fr.body,
				fileFor(fr.entry, files), owner);
		}
		for (var e : files.entrySet()) {
			writeIfChanged(out.resolve(e.getKey()), withPrototypes(e.getValue().toString()));
		}
		// The build compiles every file here: remove those that no longer have code.
		try (var old = Files.newDirectoryStream(out, "seg_*.cpp*")) {
			for (Path p : old) {
				if (!files.containsKey(p.getFileName().toString())) {
					Files.delete(p);
				}
			}
		}
		// the image as loaded, from its first byte, and the dispatch table
		MemoryBlock block = currentProgram.getMemory().getBlocks()[0];
		byte[] image = new byte[(int) block.getSize()];
		block.getBytes(block.getStart(), image);
		Files.write(out.resolve("image.bin"), image);
		writeHeaderAndDispatch(out, owner, block.getStart().getOffset(), block.getEnd().getOffset() + 1);
		// What cli.ts checks afterwards (a script's failure doesn't fail analyzeHeadless): written last.
		Set<Long> missing = new TreeSet<>(hooks);
		missing.removeAll(hooked);
		Files.writeString(out.resolve("summary.json"), String.format(
			"{ \"functions\": %d, \"fragments\": %d, \"instructions\": %d, \"files\": %d, \"unhandled\": %d, " +
			"\"hooks\": %d, \"missingHooks\": [%s] }\n", functions.size(), fragments.size(), owner.size(), files.size(),
			unhandled, hooked.size(),
			String.join(", ", missing.stream().map(a -> "\"" + Long.toHexString(a) + "\"").toList())));
		println(String.format("WcRecomp: %d functions + %d discovered fragments, %d instructions in %d files; " +
			"%d far jump fixes, %d IRET fixes, %d LSL fixes, %d unhandled p-code ops", functions.size(), fragments.size(),
			owner.size(), files.size(), farJumpFixes, iretFixes, lslFixes, unhandled));
	}

	/**
	 * The code's bytes that its own stores to fixed addresses write. Only stores to a constant address
	 * count, the way programs patch their code.
	 */
	private void findPatches(List<Function> functions, List<Fragment> fragments) {
		String what = "program";
		List<Instruction> all = new ArrayList<>();
		for (Function f : functions) {
			currentProgram.getListing().getInstructions(f.getBody(), true).forEach(all::add);
		}
		fragments.forEach(fr -> all.addAll(fr.body()));
		for (Instruction ins : all) {
			Map<Long, Long> constUniques = new HashMap<>();
			for (PcodeOp op : ins.getPcode(false)) {
				Varnode o = op.getOutput();
				if (op.getOpcode() == PcodeOp.COPY && o != null && o.isUnique() && op.getInput(0).isConstant()) {
					constUniques.put(o.getOffset(), op.getInput(0).getOffset());
				}
				// FS:[c] and GS:[c]: Ghidra adds its FS_OFFSET/GS_OFFSET register (the base is in wc_sb[])
				if (op.getOpcode() == PcodeOp.INT_ADD && o != null && o.isUnique() && op.getInput(0).isRegister() &&
						(op.getInput(0).getOffset() == 0x110 || op.getInput(0).getOffset() == 0x114) && op.getInput(1).isConstant()) {
					constUniques.put(o.getOffset(), op.getInput(1).getOffset());
				}
				// ES:[c] (and the other segments' overrides): 0 + c
				if (op.getOpcode() == PcodeOp.INT_ADD && o != null && o.isUnique() && op.getInput(0).isConstant() &&
						op.getInput(1).isConstant()) {
					constUniques.put(o.getOffset(), (op.getInput(0).getOffset() + op.getInput(1).getOffset()) & 0xffffffffL);
				}
				if (op.getOpcode() != PcodeOp.STORE) {
					continue;
				}
				Varnode a = op.getInput(1);
				Long off = a.isConstant() ? Long.valueOf(a.getOffset()) : a.isUnique() ? constUniques.get(a.getOffset()) : null;
				if (off != null && currentProgram.getMemory().contains(toAddr(off))) {
					for (int k = 0; k < op.getInput(2).getSize(); k++) {
						patched.add(off + k);
					}
				}
			}
		}
		int n = 0;
		for (Instruction ins : all) {
			for (int k = 0; k < ins.getLength(); k++) {
				if (patched.contains(ins.getAddress().getOffset() + k)) {
					println("WcRecomp: " + what + ": " + ins.getAddress() + " " + ins + " is patched at runtime");
					n++;
					break;
				}
			}
		}
		println("WcRecomp: " + what + ": " + n + " instructions patched at runtime");
	}

	/** A constant of the instruction being translated that is in bytes the program patches: read them
	 * at runtime, else null. */
	private String patchedConstant(Varnode v) {
		if (current == null || v.getSize() > 4 || patched.isEmpty()) {
			return null;
		}
		long at = current.getAddress().getOffset();
		try {
			byte[] b = current.getBytes();
			for (int p = 0; p + v.getSize() <= b.length; p++) {
				if (!patched.contains(at + p)) {
					continue;
				}
				long value = 0;
				for (int k = v.getSize() - 1; k >= 0; k--) {
					value = value << 8 | (b[p + k] & 0xff);
				}
				if (value == (v.getOffset() & mask(v.getSize()))) {
					return "mrd32(0x" + Long.toHexString(at + p) + ", " + v.getSize() + ")";
				}
			}
		}
		catch (Exception e) {
			// no bytes
		}
		return null;
	}

	/** 16 KB of code per file, so the build can work in parallel */
	private StringBuilder fileFor(long entry, Map<String, StringBuilder> files) {
		String file = String.format("seg_%06x.cpp", entry & ~0x3fffL);
		return files.computeIfAbsent(file, k -> new StringBuilder());
	}

	/** Unchanged files keep their timestamps, so the C build only recompiles what changed. */
	private static void writeIfChanged(Path file, String content) throws Exception {
		if (!Files.exists(file) || !Files.readString(file).equals(content)) {
			Files.writeString(file, content);
		}
	}

	private record Fragment(long entry, List<Instruction> body) {}

	/**
	 * Code reached only through pointers in data (the entries file) is decoded with the
	 * pseudo-disassembler, which leaves Ghidra's listing alone. Each entry point becomes a fragment
	 * holding the instructions its control flow reaches, up to code that is already recompiled; the
	 * functions it calls become fragments too.
	 */
	private List<Fragment> fragments(List<Long> seeds) throws Exception {
		List<Address> pending = new ArrayList<>();
		for (long seed : seeds) {
			pending.add(toAddr(seed));
		}
		PseudoDisassembler pdis = new PseudoDisassembler(currentProgram);
		Set<Long> done = new HashSet<>();
		List<Fragment> result = new ArrayList<>();
		while (!pending.isEmpty()) {
			Address a = pending.remove(pending.size() - 1);
			long entry = a.getOffset();
			if (entries.containsKey(entry) || !done.add(entry) || recompiled(a)) {
				continue;
			}
			TreeMap<Long, Instruction> body = new TreeMap<>();
			List<Address> work = new ArrayList<>(List.of(a));
			while (!work.isEmpty() && body.size() < 20000) {
				Address x = work.remove(work.size() - 1);
				if (body.containsKey(x.getOffset()) || x.getOffset() != entry && recompiled(x)) {
					continue;
				}
				Instruction pi;
				try {
					pi = pdis.disassemble(x);
				}
				catch (Exception e) {
					pi = null;
				}
				if (pi == null) {
					continue;
				}
				body.put(x.getOffset(), pi);
				for (Address t : pi.getFlows()) {
					// data decoded as code "jumps" anywhere: only what is in the image can be code
					if (currentProgram.getMemory().contains(t)) {
						(pi.getFlowType().isCall() ? pending : work).add(t);
					}
				}
				if (pi.getFallThrough() != null) {
					work.add(pi.getFallThrough());
				}
			}
			if (body.isEmpty()) {
				println("WcRecomp: cannot decode an instruction at " + a);
				continue;
			}
			result.add(new Fragment(entry, new ArrayList<>(body.values())));
		}
		println("WcRecomp: decoded " + result.size() + " fragments from " + seeds.size() + " entry points");
		return result;
	}

	/** An instruction start inside a Ghidra function, i.e. code that is emitted anyway. */
	private boolean recompiled(Address a) {
		return getInstructionAt(a) != null && getFunctionContaining(a) != null;
	}

	// ---- per function ----

	private void emitFunction(String fn, String title, List<Instruction> body, StringBuilder sb,
			TreeMap<Long, String> owner) {
		Set<Long> labels = new HashSet<>();
		for (Instruction ins : body) {
			labels.add(ins.getAddress().getOffset());
			owner.putIfAbsent(ins.getAddress().getOffset(), fn);
		}
		Set<String> uniques = new TreeSet<>();
		StringBuilder code = new StringBuilder();
		for (int i = 0; i < body.size(); i++) {
			Instruction ins = body.get(i);
			Instruction next = i + 1 < body.size() ? body.get(i + 1) : null;
			emitInstruction(ins, next, labels, uniques, code);
		}

		sb.append("/* ").append(title).append(" */\n");
		sb.append("void ").append(fn).append("(uint32_t at)\n{\n");
		if (!uniques.isEmpty()) {
			sb.append("\tuint64_t ").append(String.join(" = 0, ", uniques)).append(" = 0;\n");
		}
		sb.append("\tswitch (at) {\n");
		for (Instruction ins : body) {
			long a = ins.getAddress().getOffset();
			sb.append("\tcase 0x").append(Long.toHexString(a)).append(": goto L_").append(Long.toHexString(a)).append(";\n");
		}
		sb.append("\tdefault: cpu_fail(\"no instruction in ").append(fn).append(" at\", at);\n\t}\n");
		sb.append(code);
		sb.append("}\n\n");
	}

	private void emitInstruction(Instruction ins, Instruction next, Set<Long> labels, Set<String> uniques,
			StringBuilder c) {
		long at = ins.getAddress().getOffset();
		current = ins;
		c.append("L_").append(Long.toHexString(at)).append(": TRACE_INS(0x").append(Long.toHexString(at))
				.append("); /* ").append(ins.getAddress()).append(" ").append(ins.toString().replace("*/", "* /"))
				.append(" */\n");
		if (hooks.contains(at)) {
			hooked.add(at);
			c.append("\tif (wc_hook(0x").append(Long.toHexString(at)).append(")) return;\n");
		}

		// Ghidra's LSL keeps only the limit's low word, even into a 32-bit register (the C library's
		// heap takes its size from DS's limit): the whole limit, and ZF for a valid selector.
		if (ins.getMnemonicString().equals("LSL") && ins.getRegister(0) != null && ins.getRegister(1) != null) {
			var dst = ins.getRegister(0);
			var src = ins.getRegister(1);
			lslFixes++;
			c.append("\t{ uint64_t l = seg_limit32(rd(0x").append(Long.toHexString(src.getOffset())).append(", 2)); ")
					.append("if (l >> 32) wr(0x").append(Long.toHexString(dst.getOffset())).append(", ")
					.append(dst.getMinimumByteSize()).append(", l); wr(0x206, 1, l >> 32); }\n");
			if (next == null || next.getAddress().getOffset() != ins.getFallThrough().getOffset()) {
				c.append('\t').append(jump(ins.getFallThrough().getOffset(), labels)).append('\n');
			}
			return;
		}

		PcodeOp[] ops = ins.getPcode(false);
		boolean relative = false;
		for (PcodeOp op : ops) {
			if ((op.getOpcode() == PcodeOp.BRANCH || op.getOpcode() == PcodeOp.CBRANCH) && op.getInput(0).isConstant()) {
				relative = true;
			}
		}
		override = segmentOverride(ins);
		stackBased = STACK_BASED.matcher(ins.toString()).find();
		farLoad = FAR_LOADS.contains(ins.getMnemonicString());
		stringOp = STRING_OPS.contains(ins.getMnemonicString().replaceAll("\\..*", ""));
		ediUniques.clear();
		loadAddrOf.clear();
		Set<Long> swiOutputs = new HashSet<>();
		boolean ends = false;
		for (int i = 0; i < ops.length; i++) {
			PcodeOp op = ops[i];
			if (relative) {
				c.append("P_").append(Long.toHexString(at)).append("_").append(i).append(":;\n");
			}
			String line = translate(op, i, ins, labels, uniques, swiOutputs);
			c.append('\t').append(line).append('\n');
			int opc = op.getOpcode();
			ends = opc == PcodeOp.BRANCH || opc == PcodeOp.BRANCHIND || opc == PcodeOp.RETURN;
		}
		if (relative) {
			c.append("P_").append(Long.toHexString(at)).append("_").append(ops.length).append(":;\n");
		}

		// Continue with the next instruction unless the p-code already left.
		if (!ends) {
			Address ft = ins.getFallThrough();
			if (ft == null) {
				// e.g. a call to a function that never returns
				c.append("\tcpu_fail(\"no fall-through after\", 0x").append(Long.toHexString(at)).append(");\n");
			}
			else if (next == null || next.getAddress().getOffset() != ft.getOffset()) {
				c.append('\t').append(jump(ft.getOffset(), labels)).append('\n');
			}
		}
	}

	private String translate(PcodeOp op, int index, Instruction ins, Set<Long> labels, Set<String> uniques,
			Set<Long> swiOutputs) {
		long at = ins.getAddress().getOffset();
		Varnode out = op.getOutput();
		Varnode[] in = op.getInputs();
		int os = out != null ? out.getSize() : 0;
		String a = in.length > 0 ? val(in[0], uniques) : null;
		String b = in.length > 1 ? val(in[1], uniques) : null;
		int as = in.length > 0 ? in[0].getSize() : 0;
		switch (op.getOpcode()) {
			case PcodeOp.COPY:
				trackLoad(out, in[0]);
				if (out.isUnique() && in[0].isRegister() && in[0].getOffset() == 0x1c && in[0].getSize() == 4) {
					ediUniques.add(out.getOffset());
				}
				return set(out, a, uniques);
			case PcodeOp.LOAD:
				if (out.isUnique()) {
					loadAddrOf.put(out.getOffset(), in[1]);
				}
				if (os == 10) { // an x87 extended value (FLD m80): carried as a double
					return set(out, "x87_ld80(" + base(in[0], in[1]) + " + " + val(in[1], uniques) + ")", uniques);
				}
				return set(out, "mrd32(" + base(in[0], in[1]) + " + " + val(in[1], uniques) + ", " + Math.min(os, 8) + ")", uniques);
			case PcodeOp.STORE:
				if (in[2].getSize() == 10) { // FSTP m80
					return "x87_st80(" + base(in[0], in[1]) + " + " + b + ", " + val(in[2], uniques) + ");";
				}
				return "mwr32(" + base(in[0], in[1]) + " + " + b + ", " + Math.min(in[2].getSize(), 8) + ", " +
					val(in[2], uniques) + ");";
			case PcodeOp.INT_ADD: return set(out, a + " + " + b, uniques);
			case PcodeOp.INT_SUB: return set(out, a + " - " + b, uniques);
			case PcodeOp.INT_MULT: return set(out, a + " * " + b, uniques);
			case PcodeOp.INT_AND: return set(out, a + " & " + b, uniques);
			case PcodeOp.INT_OR: return set(out, a + " | " + b, uniques);
			case PcodeOp.INT_XOR: return set(out, a + " ^ " + b, uniques);
			// DIV and IDIV: the size is the dividend's, whose quotient must fit in half of it (wc_rt.h)
			case PcodeOp.INT_DIV:
				return set(out, "udiv(" + a + ", " + b + ", " + as + ", 0x" + Long.toHexString(at) + ")", uniques);
			case PcodeOp.INT_REM: return set(out, "urem(" + a + ", " + b + ", 0x" + Long.toHexString(at) + ")", uniques);
			case PcodeOp.INT_SDIV:
				return set(out, "sdiv(" + a + ", " + b + ", " + as + ", 0x" + Long.toHexString(at) + ")", uniques);
			case PcodeOp.INT_SREM:
				return set(out, "srem(" + a + ", " + b + ", " + as + ", 0x" + Long.toHexString(at) + ")", uniques);
			case PcodeOp.INT_LEFT: return set(out, "shl(" + a + ", " + b + ")", uniques);
			case PcodeOp.INT_RIGHT: return set(out, "shr(" + a + ", " + b + ")", uniques);
			case PcodeOp.INT_SRIGHT: return set(out, "sar(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.INT_2COMP: return set(out, "0 - " + a, uniques);
			case PcodeOp.INT_NEGATE: return set(out, "~" + a, uniques);
			case PcodeOp.INT_EQUAL: return set(out, "(" + a + " == " + b + ")", uniques);
			case PcodeOp.INT_NOTEQUAL: return set(out, "(" + a + " != " + b + ")", uniques);
			case PcodeOp.INT_LESS: return set(out, "(" + a + " < " + b + ")", uniques);
			case PcodeOp.INT_LESSEQUAL: return set(out, "(" + a + " <= " + b + ")", uniques);
			case PcodeOp.INT_SLESS: return set(out, "(sx(" + a + ", " + as + ") < sx(" + b + ", " + as + "))", uniques);
			case PcodeOp.INT_SLESSEQUAL:
				return set(out, "(sx(" + a + ", " + as + ") <= sx(" + b + ", " + as + "))", uniques);
			case PcodeOp.INT_CARRY: return set(out, "carry(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.INT_SCARRY: return set(out, "scarry(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.INT_SBORROW: return set(out, "sborrow(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.INT_ZEXT:
				trackLoad(out, in[0]);
				return set(out, a, uniques);
			case PcodeOp.INT_SEXT: return set(out, "(uint64_t)sx(" + a + ", " + as + ")", uniques);
			case PcodeOp.BOOL_NEGATE: return set(out, "!" + a, uniques);
			case PcodeOp.BOOL_AND: return set(out, "(" + a + " && " + b + ")", uniques);
			case PcodeOp.BOOL_OR: return set(out, "(" + a + " || " + b + ")", uniques);
			case PcodeOp.BOOL_XOR: return set(out, "((" + a + " != 0) != (" + b + " != 0))", uniques);
			case PcodeOp.POPCOUNT: return set(out, "(uint64_t)__builtin_popcountll(" + a + ")", uniques);
			case PcodeOp.SUBPIECE: return set(out, "shr(" + a + ", " + (in[1].getOffset() * 8) + ")", uniques);
			case PcodeOp.PIECE:
				return set(out, "shl(" + a + ", " + (in[1].getSize() * 8) + ") | " + b, uniques);

			// x87: ST registers (10 bytes) carry doubles; 4- and 8-byte values are floats and doubles
			case PcodeOp.FLOAT_FLOAT2FLOAT: return set(out, "x87_fcvt(" + a + ", " + as + ", " + os + ")", uniques);
			case PcodeOp.FLOAT_INT2FLOAT: return set(out, "x87_fint(" + a + ", " + as + ", " + os + ")", uniques);
			case PcodeOp.FLOAT_TRUNC: return set(out, "x87_ftrunc(" + a + ", " + as + ", " + os + ")", uniques);
			case PcodeOp.FLOAT_ADD: return set(out, "x87_fadd(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_SUB: return set(out, "x87_fsub(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_MULT: return set(out, "x87_fmul(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_DIV: return set(out, "x87_fdiv(" + a + ", " + b + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_NEG: return set(out, "x87_fneg(" + a + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_ABS: return set(out, "x87_fabs(" + a + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_SQRT: return set(out, "x87_fsqrt(" + a + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_CEIL: return set(out, "x87_fceil(" + a + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_FLOOR: return set(out, "x87_ffloor(" + a + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_ROUND: return set(out, "x87_fround(" + a + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_NAN: return set(out, "x87_fnan(" + a + ", " + as + ")", uniques);
			case PcodeOp.FLOAT_EQUAL: return set(out, "(x87_fval(" + a + ", " + as + ") == x87_fval(" + b + ", " + as + "))", uniques);
			case PcodeOp.FLOAT_NOTEQUAL: return set(out, "(x87_fval(" + a + ", " + as + ") != x87_fval(" + b + ", " + as + "))", uniques);
			case PcodeOp.FLOAT_LESS: return set(out, "(x87_fval(" + a + ", " + as + ") < x87_fval(" + b + ", " + as + "))", uniques);
			case PcodeOp.FLOAT_LESSEQUAL:
				return set(out, "(x87_fval(" + a + ", " + as + ") <= x87_fval(" + b + ", " + as + "))", uniques);

			case PcodeOp.CALLOTHER: {
				String name = currentProgram.getLanguage().getUserDefinedOpName((int) in[0].getOffset());
				switch (name) {
					case "SegmentLimit": // LSL: the limit's low word, then the valid flag
						return set(out, "seg_limit(" + val(in[1], uniques) + ")", uniques);
					case "swi":
						if (out != null && out.isUnique()) {
							swiOutputs.add(out.getOffset());
						}
						return "cpu_int(" + val(in[1], uniques) + ", 0x" + Long.toHexString(at) + ", 0x" +
							Long.toHexString(at + ins.getLength()) + ");";
					case "in":
						return set(out, "port_in(" + val(in[1], uniques) + ", " + os + ")", uniques);
					case "out":
						return "port_out(" + val(in[1], uniques) + ", " + val(in[2], uniques) + ", " + in[2].getSize() + ");";
					case "LOCK":
					case "UNLOCK":
						return "/* " + name + " */";
					// x87 transcendentals (the C library's pow, exp, log): F2XM1, FSCALE, FYL2X
					case "f2xm1":
						return set(out, "x87_f2xm1(" + val(in[1], uniques) + ", " + in[1].getSize() + ")", uniques);
					case "fscale":
						return set(out, "x87_fscale(" + val(in[1], uniques) + ", " + val(in[2], uniques) + ", " +
							in[1].getSize() + ")", uniques);
					case "log2":
						return set(out, "x87_log2(" + val(in[1], uniques) + ", " + in[1].getSize() + ")", uniques);
					default:
						unhandled++;
						return "cpu_fail(\"unhandled CALLOTHER " + name + " at\", 0x" + Long.toHexString(at) + ");";
				}
			}

			case PcodeOp.BRANCH:
				if (in[0].isConstant()) {
					return "goto P_" + Long.toHexString(at) + "_" + (index + (int) in[0].getOffset()) + ";";
				}
				return jump(in[0].getAddress().getOffset(), labels);
			case PcodeOp.CBRANCH: {
				String target = in[0].isConstant()
						? "goto P_" + Long.toHexString(at) + "_" + (index + (int) in[0].getOffset()) + ";"
						: jump(in[0].getAddress().getOffset(), labels);
				return "if (" + b + ") " + target;
			}
			case PcodeOp.BRANCHIND:
				if (ins.getMnemonicString().equals("JMPF") && !loadAddrOf.containsKey(in[0].getOffset())) {
					// "JMP FAR m16:32" jumps to the operand's address: load the far pointer from it
					farJumpFixes++;
					return "{ uint64_t fa = " + base(null, in[0]) + " + " + a + "; seg_load(1, mrd32(fa + 4, 2)); " +
						switchJump(ins, "mrd32(fa, 4)", labels) + " }";
				}
				return switchJump(ins, a, labels);
			case PcodeOp.CALL: {
				long t = in[0].getAddress().getOffset();
				String call = entries.containsKey(t) ? entries.get(t) + "(0x" + Long.toHexString(t) + ");"
						: "run_code(0x" + Long.toHexString(t) + ");";
				return "push_ret(0x" + Long.toHexString(at + ins.getLength()) + "); " + call + " if (pop_ret()) return;";
			}
			case PcodeOp.CALLIND:
				if (in[0].isUnique() && swiOutputs.contains(in[0].getOffset())) {
					return "/* interrupt handled by cpu_int */";
				}
				if (ins.getMnemonicString().equals("CALLF") && in[0].isUnique() && !loadAddrOf.containsKey(in[0].getOffset())) {
					// "CALL FAR m16:32" calls the operand's address: load the far pointer from it
					farJumpFixes++;
					return "{ uint64_t fa = " + base(null, in[0]) + " + " + a + "; seg_load(1, mrd32(fa + 4, 2)); push_ret(0x" +
						Long.toHexString(at + ins.getLength()) + "); run((uint32_t)mrd32(fa, 4)); if (pop_ret()) return; }";
				}
				return "push_ret(0x" + Long.toHexString(at + ins.getLength()) + "); run((uint32_t)" + a + "); if (pop_ret()) return;";
			case PcodeOp.RETURN:
				// A return to somewhere other than after the C caller's call (an IRETD from a routine the
				// interrupt handler called, a RET past a frame) unwinds to the frame that expects it.
				if (ins.getMnemonicString().equals("IRET") || ins.getMnemonicString().equals("IRETD")) {
					// Ghidra's IRET pops the flags into the flags word only; unpack them into CF, IF, ...
					iretFixes++;
					return "set_flags_word(rd(0x280, 2)); ret_to(code_addr(" + a + ")); return;";
				}
				return "ret_to(code_addr(" + a + ")); return;";
			default:
				unhandled++;
				return "cpu_fail(\"unhandled p-code " + op.getMnemonic() + " at\", 0x" + Long.toHexString(at) + ");";
		}
	}

	private static final Set<String> STRING_OPS = Set.of("STOSB", "STOSW", "STOSD", "MOVSB", "MOVSW", "MOVSD",
		"CMPSB", "CMPSW", "CMPSD", "SCASB", "SCASW", "SCASD", "INSB", "INSW", "INSD");

	/** The segment override prefix of an instruction (wc_sb[] index), or -1. */
	private static int segmentOverride(Instruction ins) {
		try {
			for (byte b : ins.getBytes()) {
				switch (b & 0xff) {
					case 0x26: return 0;
					case 0x2e: return 1;
					case 0x36: return 2;
					case 0x3e: return 3;
					case 0x64: return 4;
					case 0x65: return 5;
					case 0x66: case 0x67: case 0xf0: case 0xf2: case 0xf3: continue; // size, LOCK, REP
					default: return -1;
				}
			}
		}
		catch (Exception e) {
			// no bytes
		}
		return -1;
	}

	/**
	 * The base of the segment a memory access goes through. Ghidra ignores segments in 32-bit code:
	 * stack accesses (PUSH, POP, CALL, RET: an 8-byte space constant) use SS, string instructions
	 * write (and SCAS/CMPS read) ES:EDI, otherwise the prefix's segment, or SS for an operand based on
	 * EBP or ESP (the C library's far copy loads DS with LDS, then reads its arguments), or DS.
	 * LDS and its kin load through an 8-byte space constant too, but their operand is an ordinary one:
	 * read through SS, the sound mixer's source pointer came from its caller's stack segment.
	 */
	private String base(Varnode space, Varnode addr) {
		if (space != null && space.isConstant() && space.getSize() == 8 && !farLoad) {
			return "wc_sb[2]";
		}
		if (stringOp && addr != null && (addr.isUnique() && ediUniques.contains(addr.getOffset()) ||
				addr.isRegister() && addr.getOffset() == 0x1c)) {
			return "wc_sb[0]";
		}
		return "wc_sb[" + (override >= 0 ? override : stackBased ? 2 : 3) + "]";
	}

	private void trackLoad(Varnode out, Varnode in) {
		if (out != null && out.isUnique() && in.isUnique() && loadAddrOf.containsKey(in.getOffset())) {
			loadAddrOf.put(out.getOffset(), loadAddrOf.get(in.getOffset()));
		}
	}

	/** Indirect jump: known targets in this function become gotos, anything else goes through run(). */
	private String switchJump(Instruction ins, String target, Set<Long> labels) {
		StringBuilder sb = new StringBuilder("switch (code_addr(" + target + ")) {");
		Set<Long> seen = new HashSet<>();
		for (Reference r : getReferencesFrom(ins.getAddress())) {
			long t = r.getToAddress().getOffset();
			if (r.getReferenceType().isJump() && labels.contains(t) && seen.add(t)) {
				sb.append(" case 0x").append(Long.toHexString(t)).append(": goto L_").append(Long.toHexString(t)).append(";");
			}
		}
		// A jump to the address the current call is expected to return to is a return (a hand-made one).
		return sb.append(" default: if (is_return(code_addr(").append(target).append("))) return; run((uint32_t)")
				.append(target).append("); return; }").toString();
	}

	private String jump(long target, Set<Long> labels) {
		return labels.contains(target) ? "goto L_" + Long.toHexString(target) + ";"
				: "{ run_code(0x" + Long.toHexString(target) + "); return; }";
	}

	// ---- varnodes ----

	private String val(Varnode v, Set<String> uniques) {
		int s = v.getSize();
		if (v.isConstant() && patchedConstant(v) != null) {
			return patchedConstant(v);
		}
		if (v.isConstant()) {
			return "0x" + Long.toHexString(v.getOffset() & mask(s)) + "ULL";
		}
		if (v.isRegister()) {
			return "rd(0x" + Long.toHexString(v.getOffset()) + ", " + Math.min(s, 8) + ")";
		}
		if (v.isUnique()) {
			String u = "u" + Long.toHexString(v.getOffset());
			uniques.add(u);
			return s >= 8 ? u : "(" + u + " & 0x" + Long.toHexString(mask(s)) + "ULL)";
		}
		if (v.isAddress() && s == 10) { // an x87 extended value in memory
			return "x87_ld80(" + base(null, null) + " + 0x" + Long.toHexString(v.getOffset()) + ")";
		}
		if (v.isAddress()) {
			return "mrd32(" + base(null, null) + " + 0x" + Long.toHexString(v.getOffset()) + ", " + Math.min(s, 8) + ")";
		}
		unhandled++;
		return "0 /* unhandled varnode " + v + " */";
	}

	private String set(Varnode out, String expr, Set<String> uniques) {
		int s = out.getSize();
		if (out.isRegister() && out.getOffset() >= 0x100 && out.getOffset() <= 0x10a && s == 2) {
			return "seg_load(" + (out.getOffset() - 0x100) / 2 + ", " + expr + ");";
		}
		if (out.isRegister()) {
			return "wr(0x" + Long.toHexString(out.getOffset()) + ", " + Math.min(s, 8) + ", " + expr + ");";
		}
		if (out.isUnique()) {
			String u = "u" + Long.toHexString(out.getOffset());
			uniques.add(u);
			return u + " = (uint64_t)(" + expr + ")" + (s >= 8 ? "" : " & 0x" + Long.toHexString(mask(s)) + "ULL") + ";";
		}
		if (out.isAddress() && s == 10) {
			return "x87_st80(" + base(null, null) + " + 0x" + Long.toHexString(out.getOffset()) + ", " + expr + ");";
		}
		if (out.isAddress()) {
			return "mwr32(" + base(null, null) + " + 0x" + Long.toHexString(out.getOffset()) + ", " + Math.min(s, 8) + ", " +
				expr + ");";
		}
		unhandled++;
		return "/* unhandled output " + out + " */";
	}

	private static long mask(int size) {
		return size >= 8 ? -1L : (1L << (size * 8)) - 1;
	}

	private static String fname(long address) {
		return "F_" + Long.toHexString(address);
	}

	// ---- whole program ----

	/** A generated file with the prototypes of the functions it uses, not all of wc_gen.h: that gains
	 * entries in every discover round, and a file then only changes when its own code does. */
	private static String withPrototypes(String body) {
		TreeSet<String> used = new TreeSet<>();
		Matcher m = FUNCTION_NAME.matcher(body);
		while (m.find()) {
			used.add(m.group());
		}
		StringBuilder sb = new StringBuilder("#include \"wc_rt.h\"\n\n");
		for (String fn : used) {
			sb.append("void ").append(fn).append("(uint32_t at);\n");
		}
		return sb.append("\n").append(body).toString();
	}

	private static final Set<String> FAR_LOADS = Set.of("LDS", "LES", "LFS", "LGS", "LSS");
	private static final Pattern STACK_BASED = Pattern.compile("\\[E[BS]P[\\] ]");
	private static final Pattern FUNCTION_NAME = Pattern.compile("\\bF_[0-9a-f]+\\b");

	private void writeHeaderAndDispatch(Path out, TreeMap<Long, String> owner, long imageLo, long imageHi) throws Exception {
		StringBuilder h = new StringBuilder("/* Generated by WcRecomp.java. */\n#pragma once\n#include <stdint.h>\n\n");
		for (String fn : new TreeSet<>(entries.values())) {
			h.append("void ").append(fn).append("(uint32_t at);\n");
		}
		writeIfChanged(out.resolve("wc_gen.h"), h.toString());

		StringBuilder d = new StringBuilder("/* Generated by WcRecomp.java: instruction address -> function. */\n");
		d.append("#include \"wc_rt.h\"\n#include \"wc_gen.h\"\n\n");
		d.append("const uint32_t wc_code_count = ").append(owner.size()).append(";\n");
		d.append("const uint32_t wc_code_addr[] = {\n");
		for (long a : owner.keySet()) {
			d.append("0x").append(Long.toHexString(a)).append(",\n");
		}
		d.append("};\nvoid (*const wc_code_func[])(uint32_t) = {\n");
		for (String fn : owner.values()) {
			d.append(fn).append(",\n");
		}
		d.append("};\nconst uint32_t wc_hook_addr[] = {");
		for (long a : hooked) {
			d.append(" 0x").append(Long.toHexString(a)).append(",");
		}
		d.append(" 0 };\nconst uint32_t wc_image_lo = 0x").append(Long.toHexString(imageLo)).append(", wc_image_hi = 0x")
			.append(Long.toHexString(imageHi)).append(";\n");
		writeIfChanged(out.resolve("wc_dispatch.cpp"), d.toString());
	}
}
