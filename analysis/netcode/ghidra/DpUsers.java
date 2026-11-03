// P1-7: who reaches the DirectPlay wrapper. Read-only: run with -readOnly.
//   arg 0  output directory
//   arg 1  global range start-end (hex, end exclusive); default 00C3B478-00C3B62C, the static
//          wrapper object and the word after it
// Writes:
//   users.tsv          every instruction that references the range: function, offset into the object
//   users_vcalls.tsv   per user function, the wrapper vtable slots it calls, read from the decompiled C
//                      as `(**(code **)(DAT_00c3b478 + 0xNN))` (slot = 0xNN / 4)
//                      plus the functions that call each user (one level up the call graph)
//   users/<addr>.c     decompiled pseudocode of each user function
//   arg 2+ (optional) member displacements (hex) of a pointer to the wrapper held by another object
//          (FUN_004315ee stores it at owner+0x8d00); every instruction whose memory operand uses
//          that displacement is a user too, and `(**(code **)(**(int **)(X + 0xDISP) + 0xNN))` gives
//          the slot
//@category SFCOP
import java.io.*;
import java.util.*;
import java.util.regex.*;

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class DpUsers extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args[0]);
		File dec = new File(out, "users");
		dec.mkdirs();
		String range = args.length > 1 ? args[1] : "00C3B478-00C3B62C";
		String[] p = range.split("-");
		Address lo = toAddr(Long.parseLong(p[0], 16));
		Address hi = toAddr(Long.parseLong(p[1], 16) - 1);
		FunctionManager fm = currentProgram.getFunctionManager();
		ReferenceManager rm = currentProgram.getReferenceManager();

		Map<Function, Set<String>> users = new TreeMap<>(Comparator.comparing(Function::getEntryPoint));
		try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, "users.tsv")))) {
			w.println("from\tfunction\tobject_offset\tref_type");
			for (Address a = lo; a.compareTo(hi) <= 0; a = a.next()) {
				for (Reference r : rm.getReferencesTo(a)) {
					Function f = fm.getFunctionContaining(r.getFromAddress());
					w.println(r.getFromAddress() + "\t" + (f == null ? "-" : f.getEntryPoint() + " " + f.getName()) +
						"\t0x" + Long.toHexString(a.subtract(lo)) + "\t" + r.getReferenceType());
					if (f != null) users.computeIfAbsent(f, k -> new TreeSet<>()).add("0x" + Long.toHexString(a.subtract(lo)));
				}
			}
		}

		List<Long> disps = new ArrayList<>();
		for (int i = 2; i < args.length; i++) disps.add(Long.parseLong(args[i], 16));
		if (!disps.isEmpty()) {
			for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
				for (int op = 0; op < ins.getNumOperands(); op++) {
					boolean mem = false;
					long d = -1;
					for (Object o : ins.getOpObjects(op)) {
						if (o instanceof ghidra.program.model.scalar.Scalar) d = ((ghidra.program.model.scalar.Scalar) o).getUnsignedValue();
						if (o instanceof ghidra.program.model.lang.Register) mem = true;
					}
					if (mem && disps.contains(d) && ins.toString().contains("[")) {
						Function f = fm.getFunctionContaining(ins.getAddress());
						if (f != null) users.computeIfAbsent(f, k -> new TreeSet<>()).add("+0x" + Long.toHexString(d) + "@" + ins.getAddress());
					}
				}
			}
		}

		String g = "DAT_" + String.format("%08x", lo.getOffset());
		StringBuilder alt = new StringBuilder("\\(\\*\\*\\(code \\*\\*\\)\\((?:" + g);
		for (long d : disps) alt.append("|\\*\\*?\\(int \\*\\*?\\)\\([a-zA-Z_0-9]+ \\+ 0x").append(Long.toHexString(d)).append("\\)");
		Pattern slot = Pattern.compile(alt + ") \\+ (0x[0-9a-f]+|\\d+)\\)\\)");
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, "users_vcalls.tsv")))) {
			w.println("function\tname\tobject_offsets\tslots\tcallers");
			for (Function f : users.keySet()) {
				if (monitor.isCancelled()) break;
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed";
				try (PrintWriter cw = new PrintWriter(new FileWriter(new File(dec, f.getEntryPoint() + ".c")))) {
					cw.println("// " + f.getName());
					cw.print(c);
				}
				TreeSet<Integer> slots = new TreeSet<>();
				Matcher m = slot.matcher(c);
				while (m.find()) slots.add(Integer.decode(m.group(1)) / 4);
				slots.addAll(slotsFromInstructions(f, disps));
				TreeSet<String> callers = new TreeSet<>();
				for (Reference cr : rm.getReferencesTo(f.getEntryPoint())) {
					Function cf = fm.getFunctionContaining(cr.getFromAddress());
					callers.add(cf == null ? cr.getFromAddress() + "(" + cr.getReferenceType() + ")" : cf.getEntryPoint() + " " + cf.getName());
				}
				w.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + String.join(",", users.get(f)) + "\t" +
					slots.toString().replaceAll("[\\[\\] ]", "") + "\t" + String.join(";", callers));
			}
		}
		di.dispose();
		println("DpUsers: " + users.size() + " functions reference " + range);
	}

	// Register tracking inside one function: r <- [x+DISP] (the wrapper pointer), v <- [r] (its
	// vtable), CALL [v+N] -> slot N/4. A register stops being tracked when anything else writes it.
	// Catches the calls the decompiled-C pattern misses when the pointer sits in a local.
	private Set<Integer> slotsFromInstructions(Function f, List<Long> disps) {
		Set<Integer> slots = new TreeSet<>();
		Set<String> ptr = new HashSet<>(), vt = new HashSet<>();
		for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
			String mn = ins.getMnemonicString();
			Object[] dst = ins.getNumOperands() > 0 ? ins.getOpObjects(0) : new Object[0];
			Object[] src = ins.getNumOperands() > 1 ? ins.getOpObjects(1) : new Object[0];
			String d = dst.length == 1 && dst[0] instanceof ghidra.program.model.lang.Register ? dst[0].toString() : null;
			if (mn.equals("CALL")) {
				String reg = null;
				long n = 0;
				for (Object o : dst) {
					if (o instanceof ghidra.program.model.lang.Register) reg = o.toString();
					if (o instanceof ghidra.program.model.scalar.Scalar) n = ((ghidra.program.model.scalar.Scalar) o).getSignedValue();
				}
				if (reg != null && vt.contains(reg) && ins.toString().contains("[")) slots.add((int) (n / 4));
				ptr.remove("EAX"); vt.remove("EAX"); ptr.remove("ECX"); vt.remove("ECX"); ptr.remove("EDX"); vt.remove("EDX");
				continue;
			}
			if (d == null || mn.equals("TEST") || mn.equals("CMP") || mn.equals("PUSH")) continue;
			String base = null;
			long n = 0;
			for (Object o : src) {
				if (o instanceof ghidra.program.model.lang.Register) base = o.toString();
				if (o instanceof ghidra.program.model.scalar.Scalar) n = ((ghidra.program.model.scalar.Scalar) o).getUnsignedValue();
			}
			boolean load = mn.equals("MOV") && ins.toString().contains("[") && base != null;
			boolean isPtr = load && disps.contains(n);
			boolean isVt = load && n == 0 && ptr.contains(base);
			ptr.remove(d);
			vt.remove(d);
			if (isPtr) ptr.add(d);
			else if (isVt) vt.add(d);
		}
		return slots;
	}
}
