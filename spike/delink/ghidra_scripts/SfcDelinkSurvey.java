// Read-only survey of the program before delinking: blocks, import slots, code outside
// functions, undefined bytes in .text, references from bit-logic instructions, analyzer history.
// Writes a text report to the path given as the first script argument.
//@category SFCOP

import java.io.PrintWriter;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.framework.options.Options;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class SfcDelinkSurvey extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		try (PrintWriter out = new PrintWriter(args[0])) {
			Listing listing = currentProgram.getListing();
			FunctionManager fm = currentProgram.getFunctionManager();
			SymbolTable st = currentProgram.getSymbolTable();
			ReferenceManager rm = currentProgram.getReferenceManager();

			out.println("== blocks");
			for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
				out.printf("%s %s-%s init=%s r=%s w=%s x=%s%n", b.getName(), b.getStart(), b.getEnd(),
					b.isInitialized(), b.isRead(), b.isWrite(), b.isExecute());
			}

			out.println("== analyzer options (Analyzers.*) that are off");
			Options opts = currentProgram.getOptions(Program.ANALYSIS_PROPERTIES);
			for (String n : opts.getOptionNames()) {
				if (n.contains(".")) continue;
				Object v = opts.getObject(n, null);
				if (v instanceof Boolean && !((Boolean) v)) out.println("off: " + n);
				if (n.toLowerCase().contains("constant")) out.println("opt: " + n + " = " + v);
			}

			out.println("== symbols in IAT 0x795000-0x7955b7 (first 12)");
			Address iatStart = toAddr(0x795000), iatEnd = toAddr(0x7955b7);
			int shown = 0;
			for (Symbol s : st.getSymbols(new AddressSet(iatStart, iatEnd), SymbolType.LABEL, true)) {
				if (shown++ < 12) out.printf("%s %s src=%s primary=%s ns=%s%n", s.getAddress(), s.getName(true),
					s.getSource(), s.isPrimary(), s.getParentNamespace().getName(true));
			}
			Data d0 = listing.getDataAt(iatStart);
			out.println("data at IAT start: " + d0 + " type=" + (d0 == null ? null : d0.getDataType().getName()));

			for (long a : new long[] { 0x73a2a0L, 0x7bbd70L, 0x835218L }) {
				out.printf("== at %x:%n", a);
				for (Symbol s : st.getSymbols(toAddr(a))) out.println("  sym " + s.getName(true) + " src=" + s.getSource());
				CodeUnit cu = listing.getCodeUnitAt(toAddr(a));
				out.println("  cu " + cu);
			}

			MemoryBlock text = currentProgram.getMemory().getBlock(".text");
			AddressSet textSet = new AddressSet(text.getStart(), text.getEnd());

			out.println("== instructions outside functions in .text");
			long outside = 0, outsideBytes = 0;
			List<String> sample = new ArrayList<>();
			Address runStart = null, runEnd = null;
			for (Instruction ins : listing.getInstructions(textSet, true)) {
				if (fm.getFunctionContaining(ins.getAddress()) == null) {
					outside++;
					outsideBytes += ins.getLength();
					if (runStart != null && runEnd.next().equals(ins.getAddress())) {
						runEnd = ins.getMaxAddress();
					} else {
						if (runStart != null && sample.size() < 4000) sample.add(runStart + "-" + runEnd);
						runStart = ins.getAddress();
						runEnd = ins.getMaxAddress();
					}
				}
			}
			if (runStart != null) sample.add(runStart + "-" + runEnd);
			out.printf("instructions=%d bytes=%d runs=%d%n", outside, outsideBytes, sample.size());
			for (String r : sample) out.println("  run " + r);

			out.println("== undefined bytes in .text");
			long undef = 0;
			Map<Long, Integer> hist = new TreeMap<>();
			AddressSetView undefSet = listing.getUndefinedRanges(textSet, false, monitor);
			for (AddressRange r : undefSet) {
				undef += r.getLength();
				long k = Math.min(r.getLength(), 64);
				hist.merge(k, 1, Integer::sum);
			}
			out.println("undefined bytes=" + undef + " ranges=" + undefSet.getNumAddressRanges());
			out.println("length histogram (capped at 64): " + hist);

			out.println("== references from bit-logic instructions to memory");
			Set<String> logic = Set.of("AND", "OR", "XOR", "TEST", "SHL", "SHR", "SAR", "NOT", "IMUL");
			int nlogic = 0;
			for (Instruction ins : listing.getInstructions(true)) {
				String m = ins.getMnemonicString();
				if (!logic.contains(m)) continue;
				for (Reference r : ins.getReferencesFrom()) {
					if (r.getToAddress().isMemoryAddress() && r.getReferenceType().isData()) {
						// a reference is only plausible when the address is a memory operand
						String rep = ins.toString();
						if (nlogic++ < 200) out.println("  " + ins.getAddress() + " " + rep + " -> " + r.getToAddress() + " " + r.getReferenceType() + " src=" + r.getSource());
					}
				}
			}
			out.println("total=" + nlogic);

			out.println("== duplicate primary names (first 50)");
			Map<String, Integer> names = new HashMap<>();
			for (Symbol s : st.getAllSymbols(false)) {
				if (s.isExternal() || !s.isPrimary()) continue;
				names.merge(s.getName(true), 1, Integer::sum);
			}
			int dups = 0;
			for (var e : names.entrySet()) if (e.getValue() > 1) { if (dups++ < 50) out.println("  " + e.getKey() + " x" + e.getValue()); }
			out.println("duplicates=" + dups);

			out.println("== bookmarks by type/category");
			Map<String, Integer> bm = new TreeMap<>();
			var it = currentProgram.getBookmarkManager().getBookmarksIterator();
			while (it.hasNext()) { var b = it.next(); bm.merge(b.getTypeString() + "/" + b.getCategory(), 1, Integer::sum); }
			out.println(bm);
		}
	}
}
