// P1-7: dump the DirectPlay wrapper region for the inventory in analysis/netcode/directplay.md.
// Read-only: run with -readOnly so the project is never saved.
//   arg 0  output directory
//   arg 1+ address ranges as start-end (hex, end exclusive); default 0x0060F000-0x0061B800
// Writes:
//   functions.tsv  every function in the ranges: size, name, callers (in/out of range), callees
//   vcalls.tsv     every indirect call through a register plus displacement (COM / C++ vtable slot)
//   xrefs.tsv      references into the region from outside it (the call graph's upward edge)
//   refchain.tsv   references to the DirectPlay GUIDs and the tDirectPlayError type descriptor,
//                  followed up to three hops (CatchableType -> CatchableTypeArray -> ThrowInfo -> code)
//   vtables.tsv    the wrapper's vtable (0x007B64A8) and its abstract base's (0x007B654C), 41 slots
//                  each; slot targets the shared program never made functions are created in memory
//                  first (the run is read-only, so nothing is saved)
//   eh.tsv         throw sites (push of a ThrowInfo whose first CatchableType is the seed type) and
//                  catch handlers (HandlerType naming it) for direct_play::tDirectPlayError
//   decomp/<addr>.c decompiled pseudocode (also for the outside callers listed in xrefs.tsv)
//@category SFCOP
import java.io.*;
import java.util.*;

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class DpDump extends GhidraScript {
	// Retail 2.5.5.2 addresses (the Phase 1 lifter spike's GUID table, since removed; the decompilation's
	// ghidra/exports/classes.csv).
	static final long[][] SEEDS = {
		{0x007BB118L}, {0x007BB128L}, {0x007BB0B8L}, {0x007BB0C8L}, {0x0087C590L}};
	static final String[] SEED_NAMES = {
		"CLSID_DirectPlay", "IID_IDirectPlay4A", "CLSID_DirectPlayLobby", "IID_IDirectPlayLobby3A",
		"direct_play::tDirectPlayError type descriptor"};

	// Wrapper vtable and the abstract interface vtable its constructor (FUN_0060f950) installs first.
	static final long[][] VTABLES = {{0x007B64A8L, 41}, {0x007B654CL, 41}};

	AddressSet region = new AddressSet();
	FunctionManager fm;
	Listing listing;
	ReferenceManager rm;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args[0]);
		File dec = new File(out, "decomp");
		dec.mkdirs();
		fm = currentProgram.getFunctionManager();
		listing = currentProgram.getListing();
		rm = currentProgram.getReferenceManager();
		if (args.length < 2) args = new String[] {args[0], "0060F000-0061B800"};
		for (int i = 1; i < args.length; i++) {
			String[] p = args[i].split("-");
			region.add(toAddr(Long.parseLong(p[0], 16)), toAddr(Long.parseLong(p[1], 16) - 1));
		}

		File vt = new File(out, "vtables.tsv");
		try (PrintWriter w = new PrintWriter(new FileWriter(vt))) {
			w.println("vtable\tslot\ttarget\tcreated");
			for (long[] t : VTABLES) {
				for (int i = 0; i < t[1]; i++) {
					Address e = toAddr(currentProgram.getMemory().getInt(toAddr(t[0] + 4L * i)) & 0xffffffffL);
					boolean created = false;
					if (fm.getFunctionAt(e) == null) {
						disassemble(e);
						created = createFunction(e, null) != null;
					}
					w.println(toAddr(t[0]) + "\t" + i + "\t" + e + "\t" + (created ? "yes" : "no"));
				}
			}
		}
		writeEh(new File(out, "eh.tsv"), toAddr(SEEDS[4][0]));

		List<Function> funcs = new ArrayList<>();
		for (Function f : fm.getFunctions(region, true)) funcs.add(f);
		Set<Function> outsideCallers = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));

		try (PrintWriter fw = new PrintWriter(new FileWriter(new File(out, "functions.tsv")));
				PrintWriter vw = new PrintWriter(new FileWriter(new File(out, "vcalls.tsv")));
				PrintWriter xw = new PrintWriter(new FileWriter(new File(out, "xrefs.tsv")))) {
			fw.println("address\tsize\tname\tcallers_in\tcallers_out\tcallees");
			vw.println("function\tsite\tinstruction\tbase\tdisplacement\tslot");
			xw.println("target\ttarget_name\tfrom\tfrom_function\tref_type");
			for (Function f : funcs) {
				List<String> in = new ArrayList<>(), outc = new ArrayList<>(), callees = new ArrayList<>();
				for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
					Function c = fm.getFunctionContaining(r.getFromAddress());
					String who = (c == null ? "?" : c.getEntryPoint().toString()) + "@" + r.getFromAddress();
					boolean inside = region.contains(r.getFromAddress());
					(inside ? in : outc).add(who);
					if (!inside) {
						xw.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + r.getFromAddress() + "\t" +
							(c == null ? "?" : c.getEntryPoint() + " " + c.getName()) + "\t" + r.getReferenceType());
						if (c != null) outsideCallers.add(c);
					}
				}
				for (Function c : f.getCalledFunctions(monitor)) callees.add(c.getEntryPoint() + " " + c.getName());
				fw.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName() + "\t" +
					String.join(",", in) + "\t" + String.join(",", outc) + "\t" + String.join(";", callees));
				for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
					if (!ins.getFlowType().isCall() || ins.getFlowType().isUnConditional() && ins.getFlows().length > 0)
						continue;
					Object[] ops = ins.getOpObjects(0);
					Register base = null;
					long disp = 0;
					for (Object o : ops) {
						if (o instanceof Register) base = (Register) o;
						else if (o instanceof Scalar) disp = ((Scalar) o).getSignedValue();
					}
					String b = base == null ? "-" : base.getName();
					vw.println(f.getEntryPoint() + "\t" + ins.getAddress() + "\t" + ins + "\t" + b + "\t0x" +
						Long.toHexString(disp) + "\t" + (disp % 4 == 0 ? Long.toString(disp / 4) : "?"));
				}
			}
		}

		try (PrintWriter cw = new PrintWriter(new FileWriter(new File(out, "refchain.tsv")))) {
			cw.println("seed\thop\tto\tfrom\tfrom_function\tref_type");
			for (int i = 0; i < SEEDS.length; i++) chain(cw, SEED_NAMES[i], toAddr(SEEDS[i][0]), 0, new HashSet<>());
		}

		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		Set<Function> all = new LinkedHashSet<>(funcs);
		all.addAll(outsideCallers);
		for (Function f : all) {
			if (monitor.isCancelled()) break;
			DecompileResults r = di.decompileFunction(f, 60, monitor);
			String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage();
			try (PrintWriter w = new PrintWriter(new FileWriter(new File(dec, f.getEntryPoint() + ".c")))) {
				w.println("// " + f.getName() + (region.contains(f.getEntryPoint()) ? "" : "  (outside caller)"));
				w.print(c);
			}
		}
		di.dispose();
		println("DpDump: " + funcs.size() + " functions in region, " + outsideCallers.size() + " outside callers");
	}

	// References to `to`, and from each referencing data location the references to that, three hops deep.
	private void chain(PrintWriter cw, String seed, Address to, int hop, Set<Address> seen) {
		if (hop > 3 || !seen.add(to)) return;
		for (Reference r : rm.getReferencesTo(to)) {
			Address from = r.getFromAddress();
			Function f = fm.getFunctionContaining(from);
			cw.println(seed + "\t" + hop + "\t" + to + "\t" + from + "\t" +
				(f == null ? "-" : f.getEntryPoint() + " " + f.getName()) + "\t" + r.getReferenceType());
			if (f == null) {
				Data d = listing.getDataContaining(from);
				chain(cw, seed, d == null ? from : d.getMinAddress(), hop + 1, seen);
			}
		}
	}

	private List<Address> findPointer(long value) {
		List<Address> hits = new ArrayList<>();
		byte[] pat = {(byte) value, (byte) (value >> 8), (byte) (value >> 16), (byte) (value >> 24)};
		ghidra.program.model.mem.Memory mem = currentProgram.getMemory();
		for (ghidra.program.model.mem.MemoryBlock b : mem.getBlocks()) {
			if (!b.isInitialized()) continue;
			Address a = b.getStart();
			while (a != null && a.compareTo(b.getEnd()) <= 0) {
				a = mem.findBytes(a, b.getEnd(), pat, null, true, monitor);
				if (a == null) break;
				hits.add(a);
				a = a.next();
			}
		}
		return hits;
	}

	private boolean inText(long v) {
		ghidra.program.model.mem.MemoryBlock b = currentProgram.getMemory().getBlock(toAddr(v));
		return b != null && b.isExecute();
	}

	// MSVC 6 EH records: HandlerType {adjectives, pType, dispCatchObj, addressOfHandler};
	// CatchableType {properties, pType, PMD(12), sizeOrOffset, copyFunction}, listed in a
	// CatchableTypeArray {n, types[]} named by ThrowInfo {attributes, pmfnUnwind, pForward, pCTA}.
	// A catch funclet is emitted inside its parent's code, so the parent is the nearest
	// preceding function that is not itself a funclet.
	private void writeEh(File f, Address td) throws Exception {
		ghidra.program.model.mem.Memory mem = currentProgram.getMemory();
		try (PrintWriter w = new PrintWriter(new FileWriter(f))) {
			w.println("kind\tsite\tfunction\trecord");
			for (Address hit : findPointer(td.getOffset())) {
				Address rec = hit.subtract(4);
				long fourth = mem.getInt(rec.add(12)) & 0xffffffffL;
				if (inText(fourth)) {
					Address h = toAddr(fourth);
					Function p = getFunctionBefore(h);
					while (p != null && p.getName().startsWith("Catch@")) p = getFunctionBefore(p.getEntryPoint());
					w.println("catch\t" + h + "\t" + (p == null ? "-" : p.getEntryPoint()) + "\tHandlerType@" + rec);
					continue;
				}
				for (Address ctaSlot : findPointer(rec.getOffset())) {
					for (Address cta = ctaSlot.subtract(4); cta.compareTo(ctaSlot.subtract(4 * 8)) >= 0; cta = cta.subtract(4)) {
						int n = mem.getInt(cta);
						if (n <= 0 || n > 16 || cta.add(4L * n).compareTo(ctaSlot) < 0) continue;
						if (!cta.add(4).equals(ctaSlot)) continue; // only throws whose most-derived type is the seed
						for (Address tiSlot : findPointer(cta.getOffset())) {
							Address ti = tiSlot.subtract(12);
							for (Address code : findPointer(ti.getOffset())) {
								if (!inText(code.getOffset())) continue;
								Function p = fm.getFunctionContaining(code);
								w.println("throw\t" + code.subtract(1) + "\t" + (p == null ? "-" : p.getEntryPoint()) +
									"\tThrowInfo@" + ti);
							}
						}
					}
				}
			}
		}
	}
}
