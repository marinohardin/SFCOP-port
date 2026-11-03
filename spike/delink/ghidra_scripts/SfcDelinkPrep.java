// Prepares the copied program for delinking with ghidra-delinker-extension. Every change is
// logged to <outdir>/prep_*.tsv so the run can be audited and repeated.
//
//  0. Bytes in .text that analysis left undefined but that are not padding: code after calls to
//     functions wrongly marked no-return, code hidden behind instructions decoded from the
//     middle of real ones, and jump tables after a RET. The synthesizer never sees undefined
//     bytes, so an absolute address in them would keep pointing into the old image.
//  1. References from immediate operands: drop the ones that are constants (bit masks, round
//     numbers, far jumps into the middle of code) and add the ones the analysis missed (code
//     addresses loaded into a register or pushed, such as catch-block continuations).
//  2. Overrides from overrides.tsv (address, operand, keep|drop) decide the cases the rules
//     cannot.
//  3. Instructions outside every function join the function that flows into them, or become
//     functions of their own: the synthesizer only looks at instructions inside functions.
//  4. Aligned dwords in undefined data whose value is the start of something in the image
//     become pointers: the synthesizer emits data relocations only for pointer-typed data.
//     Pointers the analysis made out of short strings are turned back into bytes first.
//  5. Import address table slots are named after the import library symbols (imports.tsv).
//  6. The TLS directory is typed with pointer fields and named __tls_used; the entry point is
//     named _WinMainCRTStartup.
//  7. Every relocation target has a symbol, and exported names are unique.
//
// Args: <imports.tsv> <overrides.tsv> <outdir> <symbols.tsv>
//@category SFCOP

import java.io.*;
import java.nio.file.*;
import java.util.*;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.util.PseudoDisassembler;
import ghidra.app.util.PseudoInstruction;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class SfcDelinkPrep extends GhidraScript {
	static final Set<String> LOGIC = Set.of("AND", "OR", "XOR", "TEST", "NOT", "SHL", "SHR", "SAR",
		"ROL", "ROR", "IMUL", "MUL", "DIV", "IDIV");
	static final long NEAR_CODE = 0x1000;

	Listing listing;
	FunctionManager fm;
	SymbolTable st;
	ReferenceManager rm;
	Address imageLo, imageHi;
	AddressSetView text;
	Map<String, String> overrides = new HashMap<>();
	PrintWriter refLog;
	Set<Address> pendingFunctions = new TreeSet<>();
	Set<Address> pendingRedisassembly = new TreeSet<>();

	boolean isEhStub(Address a) throws Exception {
		byte[] b = getBytes(a, 10);
		return (b[0] & 0xff) == 0xb8 && (b[5] & 0xff) == 0xe9;
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		Path out = Paths.get(args[2]);
		Files.createDirectories(out);
		listing = currentProgram.getListing();
		fm = currentProgram.getFunctionManager();
		st = currentProgram.getSymbolTable();
		rm = currentProgram.getReferenceManager();
		imageLo = currentProgram.getImageBase();
		imageHi = currentProgram.getMemory().getBlock(".rsrc").getEnd();
		MemoryBlock t = currentProgram.getMemory().getBlock(".text");
		text = new AddressSet(t.getStart(), t.getEnd());

		for (String line : Files.readAllLines(Paths.get(args[1]))) {
			if (line.isBlank() || line.startsWith("#") || line.startsWith("address")) continue;
			String[] f = line.split("\t");
			overrides.put(f[0].toLowerCase() + ":" + f[1], f[2]);
		}

		try (PrintWriter log = new PrintWriter(out.resolve("prep_refs.tsv").toFile())) {
			refLog = log;
			log.println("from\top\tvalue\taction\treason\tinsn");
			try (PrintWriter glog = new PrintWriter(out.resolve("prep_gaps.tsv").toFile())) {
				glog.println("pass\tstart\tlength\taction\tdetail");
				repairCodeGaps(glog);
			}
			fixScalarReferences();
			try (PrintWriter olog = new PrintWriter(out.resolve("prep_orphans.tsv").toFile())) {
				olog.println("pass\tstart\tend\tbytes\taction\tfunction");
				adoptOrphanInstructions(olog);
			}
			// Again, for the code the steps above disassembled; a second pass changes nothing
			// it already decided.
			pendingFunctions.clear();
			pendingRedisassembly.clear();
			fixScalarReferences();
			fixDisplacementReferences();
			retypeDataInText();
			untypeStringPointers();
			typeDataPointers();
			dropReferencesOutsideImage();
		}
		try (PrintWriter log = new PrintWriter(out.resolve("prep_names.tsv").toFile())) {
			log.println("address\told\tnew\treason");
			unthunk(log);
			nameImports(Paths.get(args[0]), log);
			markTlsAndEntry(log);
			nameFromTable(Paths.get(args[3]), log);
			labelRelocationTargets(log);
			dedupeNames(log);
		}
		try (PrintWriter log = new PrintWriter(out.resolve("prep_bad_instructions.tsv").toFile())) {
			log.println("address\tfunction\tfunction_size\tnote");
			listBadInstructions(log);
		}
	}

	boolean inImage(long v) {
		return v >= imageLo.getOffset() && v <= imageHi.getOffset();
	}

	void logRef(Instruction ins, int op, long v, String action, String reason) {
		refLog.printf("%s\t%d\t%08x\t%s\t%s\t%s%n", ins.getAddress(), op, v, action, reason, ins);
	}

	void fixScalarReferences() throws Exception {
		int dropped = 0, added = 0, kept = 0;
		for (Instruction ins : listing.getInstructions(true)) {
			monitor.checkCancelled();
			for (int op = 0; op < ins.getNumOperands(); op++) {
				int type = ins.getOperandType(op);
				if (!OperandType.isScalar(type) || OperandType.isDynamic(type)) continue;
				Scalar s = ins.getScalar(op);
				if (s == null || !inImage(s.getUnsignedValue())) continue;
				long v = s.getUnsignedValue();
				Address target = toAddr(v);
				Reference ref = null;
				for (Reference r : ins.getOperandReferences(op)) {
					if (r.getToAddress().equals(target)) ref = r;
				}
				String override = overrides.get(ins.getAddress().toString() + ":" + op);
				String[] verdict = override != null ? new String[] { override, "override" }
						: judge(ins, target, v, ref != null);
				boolean keep = verdict[0].equals("keep");
				if (keep && ref != null && !ref.isPrimary()) {
					rm.setPrimary(ref, true);
					logRef(ins, op, v, "primary", verdict[1]);
				}
				if (keep && ref == null) {
					Reference added0 = rm.addMemoryReference(ins.getAddress(), target, RefType.DATA,
						SourceType.USER_DEFINED, op);
					rm.setPrimary(added0, true);
					logRef(ins, op, v, "add", verdict[1]);
					added++;
				} else if (!keep && ref != null) {
					rm.delete(ref);
					logRef(ins, op, v, "drop", verdict[1]);
					dropped++;
				} else if (keep) {
					kept++;
				}
			}
		}
		for (Address a : pendingRedisassembly) {
			CodeUnit cu = listing.getCodeUnitContaining(a);
			Address from = cu == null ? a : cu.getMinAddress();
			Function owner = fm.getFunctionContaining(from);
			if (owner != null && owner.getBody().getNumAddresses() <= 16) removeFunction(owner);
			listing.clearCodeUnits(from, a.add(9), false);
			pendingFunctions.add(a);
		}
		int made = 0;
		for (Address a : pendingFunctions) {
			disassemble(a);
			Function f = createFunction(a, null);
			refLog.printf("%s\t-\t%08x\t%s\tfunction at an undisassembled code address\t-%n", a, a.getOffset(),
				f != null ? "function-created" : "function-failed");
			if (f != null) made++;
		}
		println(String.format("scalar references: %d kept, %d added, %d dropped; %d of %d functions created " +
			"at undisassembled code addresses", kept, added, dropped, made, pendingFunctions.size()));
	}

	// A memory operand whose displacement is an address in the image (an array or table base)
	// needs a reference to exactly that address: analysis sometimes points the reference at the
	// element it computed (base + index * scale), and the synthesizer only emits a relocation
	// when the reference matches the operand bytes.
	void fixDisplacementReferences() throws Exception {
		int added = 0, promoted = 0;
		for (Instruction ins : listing.getInstructions(true)) {
			monitor.checkCancelled();
			for (int op = 0; op < ins.getNumOperands(); op++) {
				if (!OperandType.isDynamic(ins.getOperandType(op))) continue;
				for (Object o : ins.getOpObjects(op)) {
					if (!(o instanceof Scalar s) || !inImage(s.getUnsignedValue())) continue;
					Address target = toAddr(s.getUnsignedValue());
					Reference exact = null;
					for (Reference r : ins.getOperandReferences(op)) {
						if (r.getToAddress().equals(target)) exact = r;
					}
					if (exact != null && exact.isPrimary()) continue;
					if (exact == null) {
						exact = rm.addMemoryReference(ins.getAddress(), target, RefType.DATA,
							SourceType.USER_DEFINED, op);
						logRef(ins, op, s.getUnsignedValue(), "add", "displacement without an exact reference");
						added++;
					} else {
						logRef(ins, op, s.getUnsignedValue(), "primary", "displacement reference made primary");
						promoted++;
					}
					// The synthesizer reads only the primary reference of an operand.
					rm.setPrimary(exact, true);
				}
			}
		}
		println("displacement references: " + added + " added, " + promoted + " made primary");
	}

	// Data that lives in .text and was disassembled as code. The only case found is the
	// DirectInput keyboard data format (c_dfDIKeyboard, static data from dinput.lib): 256
	// DIOBJECTDATAFORMAT entries whose first field points at GUID_Key in .rdata. The
	// DIDATAFORMAT at 0x7bb1e8 points at it.
	void retypeDataInText() throws Exception {
		Address start = toAddr(0x6f2860);
		int count = 256;
		Address end = start.add(16L * count - 1);
		List<Function> overlapping = new ArrayList<>();
		fm.getFunctionsOverlapping(new AddressSet(start, end)).forEachRemaining(overlapping::add);
		for (Function f : overlapping) {
			refLog.printf("%s\t-\t-\tremove-function\tdata disassembled as code (%s)\t-%n", f.getEntryPoint(), f.getName());
			removeFunction(f);
		}
		listing.clearCodeUnits(start, end, false);
		StructureDataType odf = new StructureDataType("SFC_DIOBJECTDATAFORMAT", 0);
		odf.add(new PointerDataType(DataType.DEFAULT, 4), "pguid", null);
		odf.add(DWordDataType.dataType, "dwOfs", null);
		odf.add(DWordDataType.dataType, "dwType", null);
		odf.add(DWordDataType.dataType, "dwFlags", null);
		Data arr = listing.createData(start, new ArrayDataType(odf, count, 16));
		for (int i = 0; i < count; i++) {
			Data ptr = arr.getComponent(i).getComponent(0);
			rm.addMemoryReference(ptr.getAddress(), (Address) ptr.getValue(), RefType.DATA, SourceType.USER_DEFINED, 0);
		}
		st.createLabel(start, "c_dfDIKeyboard_objects", SourceType.USER_DEFINED);
		refLog.printf("%s\t-\t-\tretype\tDIOBJECTDATAFORMAT[%d] in .text\t-%n", start, count);
		println("data in .text retyped: c_dfDIKeyboard objects at " + start);
	}

	static boolean isUndefined(Data d) {
		return d == null || !d.isDefined() || d.getDataType() instanceof Undefined;
	}

	// Pointer tables the analysis never typed: string tables, COM interface maps, structures
	// that point at static objects. A dword is typed as a pointer when it sits in undefined
	// bytes on a 4-byte boundary and its value is the start of a function, an instruction, a
	// defined data item, or undefined data (often .bss). Values that land inside an
	// instruction or inside defined data are left alone (except inside a string, when a
	// neighbouring dword is a pointer: two strings merged by analysis), and so are values that land on an
	// instruction but whose three low bytes are printable (short strings such as "SFC\0"),
	// unless the dwords on both sides point into .text (the static initializer table).
	// The import address table and the import directory are rebuilt by the linker and skipped.
	boolean besidePointer(Address a) {
		for (int d : new int[] { -8, -4, 4, 8 }) {
			Data n = listing.getDataAt(a.add(d));
			if (n != null && n.isPointer()) return true;
		}
		return false;
	}

	boolean insideCodePointerTable(Address a) {
		for (int d : new int[] { -4, 4 }) {
			Data n = listing.getDataAt(a.add(d));
			if (n == null || !n.isPointer() || !(n.getValue() instanceof Address t) || !text.contains(t)) return false;
		}
		return true;
	}

	static boolean fourcc(long v) {
		for (int k = 0; k < 3; k++) {
			long c = (v >> (8 * k)) & 0xff;
			if (c < 0x20 || c >= 0x7f) return false;
		}
		return true;
	}

	// The analysis typed some short strings in data as pointers into .text: tag tables of
	// three-letter names ("SHT\0", "BOX\0") whose values happen to be .text addresses. A real
	// code pointer lands on an instruction start; these land inside instructions, or on a
	// label that is no function, with a printable value, outside a run of code pointers.
	// Relocating them would rewrite the strings. They are cleared here, in two passes (the
	// second pass sees the neighbours the first one cleared), and typeDataPointers then
	// judges them like any other undefined dword.
	void untypeStringPointers() throws Exception {
		int cleared = 0;
		for (int pass = 0; pass < 2; pass++) {
			List<Data> drop = new ArrayList<>();
			for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
				if (!b.isInitialized() || b.isExecute() || b.getName().equals("Headers")) continue;
				for (Data d : listing.getDefinedData(new AddressSet(b.getStart(), b.getEnd()).subtract(importData()), true)) {
					if (!d.isPointer() || d.getLength() != 4 || !(d.getValue() instanceof Address t) || !text.contains(t)) continue;
					CodeUnit tcu = listing.getCodeUnitContaining(t);
					if (!(tcu instanceof Instruction)) continue;
					boolean offcut = !tcu.getMinAddress().equals(t);
					boolean tag = fourcc(t.getOffset()) && fm.getFunctionAt(t) == null && !insideCodePointerTable(d.getAddress());
					if (pass == 0 ? offcut : tag) drop.add(d);
				}
			}
			for (Data d : drop) {
				Address a = d.getAddress();
				long v = ((Address) d.getValue()).getOffset();
				for (Reference r : rm.getReferencesFrom(a)) rm.delete(r);
				listing.clearCodeUnits(a, a.add(3), false);
				refLog.printf("%s\t-\t%08x\tuntype\tpointer to %s, a short string\t-%n", a, v,
					pass == 0 ? "inside an instruction" : "a non-function label with a printable value");
				cleared++;
			}
		}
		println("string pointers untyped: " + cleared);
	}

	// The import address table and the import directory, rebuilt by the linker.
	AddressSet importData() {
		AddressSet skip = new AddressSet(toAddr(0x795000), toAddr(0x7955b7));
		skip.add(toAddr(0x835218), toAddr(0x837fff));
		return skip;
	}

	void typeDataPointers() throws Exception {
		AddressSet skip = importData();
		int typed = 0, rejected = 0;
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			if (!b.isInitialized() || b.isExecute() || b.getName().equals("Headers") || b.getName().equals(".rsrc")) continue;
			for (Address a = b.getStart(); a.add(3).compareTo(b.getEnd()) <= 0; a = a.add(4)) {
				if (skip.contains(a)) continue;
				boolean free = true;
				for (int k = 0; k < 4 && free; k++) {
					CodeUnit cu = listing.getCodeUnitContaining(a.add(k));
					free = !(cu instanceof Instruction) && isUndefined((Data) cu);
				}
				if (!free) continue;
				long v = currentProgram.getMemory().getInt(a) & 0xffffffffL;
				if (!inImage(v) || v < imageLo.getOffset() + 0x1000) continue;
				Address t = toAddr(v);
				MemoryBlock tb = currentProgram.getMemory().getBlock(t);
				if (tb == null || tb.getName().equals("Headers")) continue;
				boolean fourcc = fourcc(v);
				String kind;
				boolean ok;
				CodeUnit tcu = listing.getCodeUnitContaining(t);
				if (fm.getFunctionAt(t) != null) { kind = "function"; ok = true; }
				else if (tcu instanceof Instruction) {
					ok = tcu.getMinAddress().equals(t);
					kind = ok ? "instruction" : "inside an instruction";
				}
				else if (isUndefined((Data) tcu)) { kind = "undefined data"; ok = true; }
				else {
					Data td = (Data) tcu;
					ok = tcu.getMinAddress().equals(t);
					kind = (ok ? "data " : "inside data ") + td.getDataType().getName();
					if (!ok && td.hasStringValue() && besidePointer(a)) {
						// Analysis merged two strings; the table points at the second one.
						ok = true;
						kind = "inside a string, in a pointer table";
					}
				}
				// Short strings ("SFC\0") read as addresses of instructions: a .text address has
				// 0x40-0x79 in its third byte, printable. Data addresses are not filtered.
				if (ok && fourcc && tcu instanceof Instruction && fm.getFunctionAt(t) == null) {
					if (insideCodePointerTable(a)) kind = "FourCC-like value in a code pointer table, target " + kind;
					else {
						ok = false;
						kind = "FourCC-like value, target " + kind;
					}
				}
				if (!ok) {
					refLog.printf("%s	-	%08x	leave	data dword, %s	-%n", a, v, kind);
					rejected++;
					continue;
				}
				Data d = DataUtilities.createData(currentProgram, a, new PointerDataType(DataType.DEFAULT, 4), 4,
					DataUtilities.ClearDataMode.CLEAR_ALL_UNDEFINED_CONFLICT_DATA);
				Reference r = rm.addMemoryReference(a, t, RefType.DATA, SourceType.USER_DEFINED, 0);
				rm.setPrimary(r, true);
				refLog.printf("%s	-	%08x	type-pointer	data dword, %s	%s%n", a, v, kind, d);
				typed++;
			}
		}
		println("data pointers typed: " + typed + ", left as values: " + rejected);
	}

	// References to the PE header or to addresses outside every block (0xffffffff sentinels,
	// absolute low addresses, junk decoded from tables) cannot be relocations: the header is
	// rebuilt by link.exe and nothing else exists at those addresses.
	void dropReferencesOutsideImage() throws Exception {
		MemoryBlock headers = currentProgram.getMemory().getBlock("Headers");
		List<Reference> doomed = new ArrayList<>();
		for (Address from : rm.getReferenceSourceIterator(currentProgram.getMemory(), true)) {
			for (Reference r : rm.getReferencesFrom(from)) {
				Address to = r.getToAddress();
				if (!to.isMemoryAddress() || r.isStackReference() || r.isRegisterReference()) continue;
				MemoryBlock b = currentProgram.getMemory().getBlock(to);
				if (b == null || b == headers) doomed.add(r);
			}
		}
		for (Reference r : doomed) {
			CodeUnit cu = listing.getCodeUnitAt(r.getFromAddress());
			refLog.printf("%s\t%d\t%08x\tdrop\treference outside the image or into the PE header\t%s%n",
				r.getFromAddress(), r.getOperandIndex(), r.getToAddress().getOffset(), cu);
			rm.delete(r);
		}
		println("references outside the image dropped: " + doomed.size());
	}

	boolean isPadding(byte[] b) {
		for (byte x : b) if ((x & 0xff) != 0x90 && (x & 0xff) != 0xcc) return false;
		return true;
	}

	boolean hasReturn(Function f) {
		for (Instruction i : listing.getInstructions(f.getBody(), true)) {
			if (i.getMnemonicString().startsWith("RET")) return true;
		}
		return false;
	}

	boolean codeStart(long v) {
		Address a = toAddr(v);
		return text.contains(a) && (fm.getFunctionAt(a) != null || listing.getInstructionAt(a) != null);
	}

	// n bytes at a, all 4-byte entries pointing into .text within 64 KB: typed as pointers, and
	// any entry whose target was never disassembled is disassembled.
	boolean tableShape(Address a, int n) throws Exception {
		if (n < 4 || n % 4 != 0) return false;
		for (int i = 0; i < n; i += 4) {
			long v = getInt(a.add(i)) & 0xffffffffL;
			if (!text.contains(toAddr(v)) || Math.abs(v - a.getOffset()) >= 0x10000) return false;
		}
		return true;
	}

	boolean typeJumpTable(Address a, int n) throws Exception {
		if (!tableShape(a, n)) return false;
		for (int i = 0; i < n; i += 4) {
			Address e = a.add(i);
			Address t = toAddr(getInt(e) & 0xffffffffL);
			createData(e, new PointerDataType(DataType.DEFAULT, 4));
			Reference ref = rm.addMemoryReference(e, t, RefType.DATA, SourceType.USER_DEFINED, 0);
			rm.setPrimary(ref, true);
			if (listing.getCodeUnitContaining(t) instanceof Data d && !d.isDefined()) disassemble(t);
		}
		return true;
	}

	// Real code starts at start, but analysis decoded instructions from inside it (some of them
	// even became functions or parts of functions) or typed data over it. Decode the real
	// instructions from start until one ends where an existing instruction begins, clear what
	// overlaps that span, then disassemble from start. A jump table entry, or a function that
	// something calls or jumps to, inside the span stops it.
	String disassembleOverJunk(Address start, int len, String what) throws Exception {
		PseudoDisassembler pd = new PseudoDisassembler(currentProgram);
		Address gapEnd = start.add(len);
		Address a = start;
		for (int n = 0; n < 32; n++) {
			if (a.compareTo(gapEnd) >= 0 && listing.getInstructionAt(a) != null) break;
			PseudoInstruction pi = null;
			try { pi = pd.disassemble(a); } catch (Exception e) { }
			if (pi == null) return "disassemble-" + what + "-failed (no instruction at " + a + ")";
			a = a.add(pi.getLength());
			FlowType ft = pi.getFlowType();
			if (a.compareTo(gapEnd) >= 0 && (ft.isTerminal() || (ft.isJump() && !ft.isConditional()))) break;
		}
		List<CodeUnit> junk = new ArrayList<>();
		List<Function> bogus = new ArrayList<>();
		for (CodeUnit cu : listing.getCodeUnits(new AddressSet(start, a.subtract(1)), true)) {
			if (cu instanceof Data d) {
				if (!d.isDefined()) continue;
				if (d.isPointer() && d.getValue() instanceof Address t && text.contains(t)) {
					return "disassemble-" + what + "-failed (jump table at " + cu.getMinAddress() + ")";
				}
			}
			Function f = fm.getFunctionAt(cu.getMinAddress());
			if (f != null) {
				if (hasFlowReference(f.getEntryPoint())) {
					return "disassemble-" + what + "-failed (called function at " + cu.getMinAddress() + ")";
				}
				bogus.add(f);
			}
			junk.add(cu);
		}
		for (Function f : bogus) {
			refLog.printf("%s\t-\t-\tremove-function\tbegins inside code at %s, nothing calls it (%s)\t-%n",
				f.getEntryPoint(), start, f.getName());
			removeFunction(f);
		}
		for (CodeUnit cu : junk) {
			if (cu instanceof Data) {
				refLog.printf("%s\t-\t-\tclear-data\t%s inside code at %s\t-%n", cu.getMinAddress(), cu, start);
			}
			listing.clearCodeUnits(cu.getMinAddress(), cu.getMaxAddress(), false);
		}
		disassemble(start);
		boolean ok = listing.getInstructionAt(start) != null;
		return "disassemble-" + what + (ok ? "" : "-failed") + " (cleared " + junk.size() + ")";
	}

	// VC6 function entries seen in the gaps: MOV EAX, handler / CALL __EH_prolog or JMP
	// __CxxFrameHandler (EH stub), PUSH EBP / MOV EBP, ESP, PUSH -1 / PUSH handler (SEH frame),
	// JMP rel32, and MOV ECX, object / CALL (a static object's initializer).
	boolean hasFlowReference(Address a) {
		for (Reference r : rm.getReferencesTo(a)) {
			if (r.getReferenceType().isFlow()) return true;
		}
		return false;
	}

	boolean looksLikeFunctionStart(Address a) throws Exception {
		byte[] b = getBytes(a, 6);
		int b0 = b[0] & 0xff, b1 = b[1] & 0xff, b2 = b[2] & 0xff, b5 = b[5] & 0xff;
		return ((b0 == 0xb8 || b0 == 0xb9) && (b5 == 0xe8 || b5 == 0xe9))
			|| (b0 == 0x55 && b1 == 0x8b && b2 == 0xec)
			|| (b0 == 0x6a && b1 == 0xff && b2 == 0x68)
			|| b0 == 0xe9;
	}

	void repairCodeGaps(PrintWriter log) throws Exception {
		Map<String, Integer> counts = new TreeMap<>();
		for (int pass = 0; pass < 4; pass++) {
			int changed = 0;
			List<AddressRange> gaps = new ArrayList<>();
			listing.getUndefinedRanges(text, false, monitor).forEach(gaps::add);
			for (AddressRange r : gaps) {
				Address start = r.getMinAddress();
				if (listing.getCodeUnitAt(start) instanceof Instruction) continue;  // filled this pass
				int len = (int) r.getLength();
				byte[] b = getBytes(start, len);
				if (isPadding(b)) continue;
				Instruction prev = listing.getInstructionBefore(start);
				String action;
				String detail = prev == null ? "-" : prev.getAddress() + " " + prev;
				CodeUnit before = listing.getCodeUnitBefore(start);
				if (before instanceof Data d && d.isDefined() && d.getMaxAddress().next().equals(start)
						&& (prev == null || !prev.getMaxAddress().next().equals(start) || !prev.getFlowType().hasFallthrough())) {
					// The rest of a table whose first entries are typed, or a jump table after
					// the byte index table of another switch.
					// An entry can be cut short by a stray byte item, or an instruction no
					// function owns, decoded over its tail.
					int n = (len + 3) / 4 * 4;
					boolean stray = n != len && tableShape(start, n);
					List<Function> bogus = new ArrayList<>();
					for (CodeUnit cu : listing.getCodeUnits(new AddressSet(start, start.add(n - 1)), true)) {
						if (cu instanceof Data dd) { stray &= !dd.isPointer(); continue; }
						Function f = fm.getFunctionContaining(cu.getMinAddress());
						if (f == null) continue;
						if (f.getEntryPoint().equals(cu.getMinAddress()) && !hasFlowReference(f.getEntryPoint())) bogus.add(f);
						else stray = false;
					}
					if (stray) {
						for (Function f : bogus) {
							refLog.printf("%s\t-\t-\tremove-function\tdecoded from a jump table entry (%s)\t-%n", f.getEntryPoint(), f.getName());
							removeFunction(f);
						}
						listing.clearCodeUnits(start, start.add(n - 1), false);
					}
					action = typeJumpTable(start, stray ? n : len) ? "jump-table-entries-continued" : "leave-after-pointer";
				}
				else if (prev == null || !prev.getMaxAddress().next().equals(start)) {
					action = "leave-not-after-instruction";
				}
				else if (prev.getFlowType().isCall()) {
					Address[] flows = prev.getFlows();
					Function callee = flows.length > 0 ? fm.getFunctionAt(flows[0]) : null;
					if (callee != null && callee.hasNoReturn()) {
						if (!hasReturn(callee)) {
							log.printf("%d\t%s\t%d\tleave-callee-has-no-ret\t%s%n", pass, start, len, detail);
							counts.merge("leave-callee-has-no-ret", 1, Integer::sum);
							continue;
						}
						callee.setNoReturn(false);
						log.printf("%d\t%s\t-\tclear-noreturn\t%s has a RET%n", pass, callee.getEntryPoint(), callee.getName());
					}
					if (prev.getFlowOverride() != FlowOverride.NONE) prev.setFlowOverride(FlowOverride.NONE);
					prev.clearFallThroughOverride();
					action = disassembleOverJunk(start, len, "after-call");
				}
				else if (prev.getFlowType().isTerminal() || (prev.getFlowType().isJump() && !prev.getFlowType().isConditional())) {
					// A jump table (VC6 puts switch tables right after the function's last
					// instruction, not always 4-byte aligned), possibly after padding, or the
					// start of a function nothing references directly.
					int skip = 0;
					while (skip < len && ((b[skip] & 0xff) == 0x90 || (b[skip] & 0xff) == 0xcc)) skip++;
					Address s0 = start.add(skip);
					int n = len - skip;
					// VC6 pads to the table with NOPs, INT3 or LEA ECX,[ECX+0] (8d 49 00).
					int k = 0;
					while (k < 4 && k < len && !(((len - k) % 4 == 0) && tableShape(start.add(k), len - k))) k++;
					if (k < 4 && k < len && typeJumpTable(start.add(k), len - k)) {
						action = k == 0 ? "jump-table-entries" : "jump-table-entries-after-padding";
					}
					else if (looksLikeFunctionStart(s0)) {
						action = disassembleOverJunk(s0, n, "function-start");
						if (listing.getInstructionAt(s0) != null && fm.getFunctionContaining(s0) == null) createFunction(s0, null);
					}
					else action = "leave-after-terminal";
				}
				else {
					// The previous instruction falls through into the gap: real code, hidden
					// behind instructions decoded from inside it that no function owns.
					action = disassembleOverJunk(start, len, "fallthrough");
				}
				if (!action.startsWith("leave")) changed++;
				counts.merge(action.replaceAll(" \\(.*", ""), 1, Integer::sum);
				log.printf("%d\t%s\t%d\t%s\t%s%n", pass, start, len, action, detail);
			}
			if (changed == 0) break;
		}
		println("code gaps: " + counts);
	}

	boolean afterSbbMask(Instruction ins) {
		Instruction p = ins.getPrevious();
		for (int i = 0; i < 2 && p != null; i++, p = p.getPrevious()) {
			String m = p.getMnemonicString();
			if (m.equals("SBB")) {
				Object[] a = p.getOpObjects(0), b = p.getOpObjects(1);
				return a.length == 1 && b.length == 1 && a[0].equals(b[0]);
			}
			if (!m.equals("NOT") && !m.equals("DEC") && !m.equals("NEG")) return false;
		}
		return false;
	}

	// Returns {keep|drop, reason} for an immediate operand whose value lies in the image.
	// "keep" adds a reference when there is none, so the rules for code targets only add one
	// for MOV/PUSH, the two ways VC6 materializes a code address.
	String[] judge(Instruction ins, Address target, long v, boolean hasRef) throws Exception {
		String m = ins.getMnemonicString();
		boolean movPush = m.equals("MOV") || m.equals("PUSH");
		// Except SBB r,r / NOT r / AND r, offset X: MSVC's branch-free "flag ? &X : NULL".
		if (LOGIC.contains(m) && !(m.equals("AND") && afterSbbMask(ins))) {
			return new String[] { "drop", "bit-logic/arithmetic operand" };
		}
		if (!currentProgram.getMemory().contains(target)) return new String[] { "drop", "not in memory" };
		Function f = fm.getFunctionAt(target);
		CodeUnit cu = listing.getCodeUnitContaining(target);
		if (text.contains(target)) {
			// No round-number rule here: VC6 loads EH handler addresses with
			// MOV EAX, offset handler / CALL __EH_prolog, and handlers in .text$x often sit on
			// round addresses (0x75c000, 0x780000, ...).
			if (f != null) return new String[] { hasRef || movPush ? "keep" : "drop", "function entry" };
			if (cu instanceof Instruction && cu.getMinAddress().equals(target)) {
				if (hasRef || movPush) return new String[] { "keep", "code address" };
				return new String[] { "drop", "code address used by " + m };
			}
			if (cu instanceof Data d && !d.isDefined() && movPush) {
				// e.g. PUSH offset of an empty static destructor (a lone RET) handed to atexit;
				// disassembled after the instruction walk
				pendingFunctions.add(target);
				return new String[] { "keep", "undisassembled code, function to create" };
			}
			if (movPush && isEhStub(target)) {
				// MOV EAX, offset FuncInfo / JMP __CxxFrameHandler that analysis decoded from
				// the wrong byte
				pendingRedisassembly.add(target);
				return new String[] { "keep", "misaligned EH handler stub, re-disassembled" };
			}
			return new String[] { "drop", "inside an instruction or data in .text" };
		}
		if (v % 1000000 == 0) return new String[] { "drop", "multiple of 1,000,000" };
		if (cu instanceof Data d && d.isDefined()) {
			return new String[] { "keep", "defined data " + d.getDataType().getName() };
		}
		return new String[] { "keep", "undefined data" };
	}

	// Maximal runs of instructions in .text that no function contains.
	List<AddressSet> orphanRuns() {
		List<AddressSet> runs = new ArrayList<>();
		AddressSet cur = null;
		Address prevEnd = null;
		for (Instruction ins : listing.getInstructions(text, true)) {
			if (fm.getFunctionContaining(ins.getAddress()) != null) {
				cur = null;
				continue;
			}
			if (cur == null || prevEnd == null || !prevEnd.next().equals(ins.getAddress())) {
				cur = new AddressSet();
				runs.add(cur);
			}
			cur.add(ins.getMinAddress(), ins.getMaxAddress());
			prevEnd = ins.getMaxAddress();
		}
		return runs;
	}

	// Most runs are whole functions reached only through data (dynamic initializers listed in
	// the .CRT$XCU table, vtable slots, pushed callbacks), so the run start becomes a function
	// whose body Ghidra computes; runs that continue a function by fall-through or a jump join
	// it. Repeated because a computed body can leave the tail of a run behind; whatever is left
	// after that becomes a function with exactly the run as its body.
	void adoptOrphanInstructions(PrintWriter log) throws Exception {
		int joined = 0, created = 0, forced = 0, failed = 0, total = 0;
		for (int pass = 0; pass < 6; pass++) {
			List<AddressSet> runs = orphanRuns();
			if (pass == 0) total = runs.size();
			if (runs.isEmpty()) break;
			boolean last = pass == 5;
			for (AddressSet run : runs) {
				monitor.checkCancelled();
				Address start = run.getMinAddress();
				if (fm.getFunctionContaining(start) != null) continue;   // covered earlier this pass
				Function owner = null;
				String action = null;
				Instruction before = listing.getInstructionBefore(start);
				if (before != null && before.getMaxAddress().next().equals(start) &&
					before.getFlowType().hasFallthrough()) {
					owner = fm.getFunctionContaining(before.getAddress());
				}
				if (owner == null) {
					for (Reference r : rm.getReferencesTo(start)) {
						RefType t = r.getReferenceType();
						if (t.isFlow() && !t.isCall()) {
							owner = fm.getFunctionContaining(r.getFromAddress());
							if (owner != null) break;
						}
					}
				}
				if (owner != null) {
					try {
						owner.setBody(owner.getBody().union(run));
						action = "joined";
						joined++;
					} catch (Exception e) {
						owner = null;
					}
				}
				if (owner == null && !last) {
					owner = createFunction(start, null);
					if (owner != null) {
						action = "created";
						created++;
					}
				}
				if (owner == null) {
					CreateFunctionCmd cmd = new CreateFunctionCmd(null, start, run, SourceType.ANALYSIS);
					if (cmd.applyTo(currentProgram, monitor)) {
						owner = fm.getFunctionAt(start);
						action = "created-with-run-body";
						forced++;
					} else {
						action = "failed:" + cmd.getStatusMsg();
						failed++;
					}
				}
				log.printf("%d\t%s\t%s\t%d\t%s\t%s%n", pass, start, run.getMaxAddress(), run.getNumAddresses(),
					action, owner == null ? "-" : owner.getName(true));
			}
		}
		int left = orphanRuns().size();
		println(String.format("orphan runs: %d at start; joined %d, created %d, created with run body %d, " +
			"failed %d; %d runs left", total, joined, created, forced, failed, left));
	}

	// A thunk shows its target's name, so a thunk and its target would export the same symbol
	// (and renaming one renames both). Turn every thunk into an ordinary function with a name
	// of its own; the JMP inside it is relocated like any other.
	void unthunk(PrintWriter log) throws Exception {
		int n = 0;
		List<Function> thunks = new ArrayList<>();
		for (Function f : fm.getFunctions(true)) if (f.isThunk()) thunks.add(f);
		for (Function f : thunks) {
			Function target = f.getThunkedFunction(false);
			String old = f.getName(true);
			f.setThunkedFunction(null);
			String name = String.format("thunk_%s_%s", target == null ? "unknown" : target.getName(), f.getEntryPoint());
			f.setName(name.replaceAll("[^A-Za-z0-9_@?$]", "_"), SourceType.USER_DEFINED);
			f.setParentNamespace(currentProgram.getGlobalNamespace());
			log.printf("%s\t%s\t%s\tthunk made an ordinary function%n", f.getEntryPoint(), old, f.getName());
			n++;
		}
		println("thunks made ordinary functions: " + n);
	}

	void setPrimaryGlobal(Address a, String name, String reason, PrintWriter log) throws Exception {
		Symbol old = st.getPrimarySymbol(a);
		Symbol s = st.getSymbol(name, a, currentProgram.getGlobalNamespace());
		if (s == null) s = st.createLabel(a, name, currentProgram.getGlobalNamespace(), SourceType.USER_DEFINED);
		s.setPrimary();
		log.printf("%s\t%s\t%s\t%s%n", a, old == null ? "-" : old.getName(true), name, reason);
	}

	// symbols.tsv: names that compiled code linked against the object needs (address, name,
	// evidence).
	void nameFromTable(Path tsv, PrintWriter log) throws Exception {
		int n = 0;
		for (String line : Files.readAllLines(tsv)) {
			if (line.isBlank() || line.startsWith("#")) continue;
			String[] f = line.split("\t");
			setPrimaryGlobal(toAddr(Long.parseLong(f[0], 16)), f[1], "symbols.tsv", log);
			n++;
		}
		println("names from symbols.tsv: " + n);
	}

	void nameImports(Path tsv, PrintWriter log) throws Exception {
		int n = 0;
		for (String line : Files.readAllLines(tsv)) {
			if (line.startsWith("iat_va")) continue;
			String[] f = line.split("\t");
			Address a = toAddr(Long.parseLong(f[0], 16));
			// Make the slot a plain pointer so the symbol is a data symbol on a 4-byte unit.
			setPrimaryGlobal(a, f[3], "import " + f[1] + "!" + f[2], log);
			n++;
		}
		println("import slots named: " + n);
	}

	void markTlsAndEntry(PrintWriter log) throws Exception {
		long tlsVa = currentProgram.getImageBase().getOffset() + 0x3bbd70;
		Address tls = toAddr(tlsVa);
		StructureDataType dir = new StructureDataType("SFC_IMAGE_TLS_DIRECTORY32", 0);
		PointerDataType p = new PointerDataType(DataType.DEFAULT, 4);
		dir.add(p, "StartAddressOfRawData", null);
		dir.add(p, "EndAddressOfRawData", null);
		dir.add(p, "AddressOfIndex", null);
		dir.add(p, "AddressOfCallBacks", null);
		dir.add(DWordDataType.dataType, "SizeOfZeroFill", null);
		dir.add(DWordDataType.dataType, "Characteristics", null);
		listing.clearCodeUnits(tls, tls.add(dir.getLength() - 1), false);
		Data d = listing.createData(tls, dir);
		for (int i = 0; i < 4; i++) {
			Data c = d.getComponent(i);
			Address to = (Address) c.getValue();
			if (to != null && to.getOffset() != 0) {
				rm.addMemoryReference(c.getAddress(), to, RefType.DATA, SourceType.USER_DEFINED, 0);
			}
		}
		setPrimaryGlobal(tls, "__tls_used", "TLS directory (link.exe looks it up by name)", log);

		Address entry = toAddr(currentProgram.getImageBase().getOffset() + 0x33a2a0);
		Function f = fm.getFunctionAt(entry);
		String old = f.getName(true);
		f.setName("_WinMainCRTStartup", SourceType.USER_DEFINED);
		f.setParentNamespace(currentProgram.getGlobalNamespace());
		log.printf("%s\t%s\t%s\t%s%n", entry, old, "_WinMainCRTStartup", "PE entry point");
	}

	// The synthesizer emits a relocation only when the code unit containing the target has a
	// primary symbol; give every such unit a label.
	void labelRelocationTargets(PrintWriter log) throws Exception {
		int n = 0;
		AddressSet seen = new AddressSet();
		for (Address from : rm.getReferenceSourceIterator(currentProgram.getMemory(), true)) {
			for (Reference r : rm.getReferencesFrom(from)) {
				Address to = r.getToAddress();
				if (!to.isMemoryAddress() || !currentProgram.getMemory().contains(to)) continue;
				CodeUnit cu = listing.getCodeUnitContaining(to);
				Address base = cu == null ? to : cu.getMinAddress();
				if (seen.contains(base)) continue;
				seen.add(base);
				if (st.getPrimarySymbol(base) == null) {
					st.createLabel(base, String.format("D_%08x", base.getOffset()), SourceType.ANALYSIS);
					n++;
				}
			}
		}
		log.printf("-\t-\t-\t%d labels added at relocation targets without a symbol%n", n);
		println("labels added at targets: " + n);
	}

	// The name the COFF exporter will choose (its MSVC preference): any ?-mangled name, else
	// the primary symbol's full name.
	static String exportName(Symbol[] syms) {
		for (Symbol s : syms) if (s.getName(true).startsWith("?")) return s.getName(true);
		for (Symbol s : syms) if (s.isPrimary()) return s.getName(true);
		return null;
	}

	void dedupeNames(PrintWriter log) throws Exception {
		int renamed = 0;
		for (int pass = 0; pass < 4; pass++) {
			Map<String, List<Address>> byName = new HashMap<>();
			for (Symbol s : st.getAllSymbols(false)) {
				if (s.isExternal() || !s.isPrimary() || s.isDynamic()) continue;
				Address a = s.getAddress();
				if (!a.isMemoryAddress()) continue;
				Function f = fm.getFunctionContaining(a);
				if (f != null && !f.getEntryPoint().equals(a)) continue;   // exported static
				String name = exportName(st.getSymbols(a));
				if (name != null) byName.computeIfAbsent(name, k -> new ArrayList<>()).add(a);
			}
			int before = renamed;
			for (var e : byName.entrySet()) {
				if (e.getValue().size() < 2) continue;
				for (Address a : e.getValue()) {
					String fresh = e.getKey() + "_" + a.toString();
					for (Symbol s : st.getSymbols(a)) {
						if (s.getName(true).equals(e.getKey())) {
							s.setNameAndNamespace(fresh, currentProgram.getGlobalNamespace(),
								SourceType.USER_DEFINED);
							break;
						}
					}
					log.printf("%s\t%s\t%s\tduplicate export name (pass %d)%n", a, e.getKey(), fresh, pass);
					renamed++;
				}
			}
			if (renamed == before) break;
		}
		println("duplicate names renamed: " + renamed);
	}

	// Functions that contain undecodable bytes may be data decoded as code; list the
	// in-image references their instructions make, for review.
	void listBadInstructions(PrintWriter log) {
		var it = currentProgram.getBookmarkManager().getBookmarksIterator("Error");
		Set<Function> seen = new HashSet<>();
		while (it.hasNext()) {
			var b = it.next();
			Function f = fm.getFunctionContaining(b.getAddress());
			log.printf("%s\t%s\t%s\t-%n", b.getAddress(), f == null ? "-" : f.getName(true),
				f == null ? "-" : Long.toString(f.getBody().getNumAddresses()));
			if (f == null || !seen.add(f)) continue;
			for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
				for (Reference r : ins.getReferencesFrom()) {
					if (r.getToAddress().isMemoryAddress() && !r.getReferenceType().isFlow()) {
						log.printf("%s\t%s\t-\treference to %s: %s%n", ins.getAddress(), f.getName(true),
							r.getToAddress(), ins);
					}
				}
			}
		}
	}
}
