// Diagnostic: undefined byte runs in .text that are not just padding (0x90, 0xCC), with the
// instruction before each run and whether its call target is marked no-return.
// Args: <out.tsv>
//@category SFCOP

import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;

public class SfcGapProbe extends GhidraScript {
	@Override
	public void run() throws Exception {
		MemoryBlock t = currentProgram.getMemory().getBlock(".text");
		Listing l = currentProgram.getListing();
		AddressSet text = new AddressSet(t.getStart(), t.getEnd());
		AddressSetView undef = l.getUndefinedRanges(text, false, monitor);
		int pad = 0, code = 0;
		long codeBytes = 0;
		Map<String, Integer> byPrev = new TreeMap<>();
		try (PrintWriter w = new PrintWriter(new File(getScriptArgs()[0]))) {
			w.println("start\tlength\tprev_insn\tcall_target\tnoreturn\tfirst_bytes");
			for (AddressRange r : undef) {
				byte[] b = getBytes(r.getMinAddress(), (int) r.getLength());
				boolean padding = true;
				for (byte x : b) padding &= (x & 0xff) == 0x90 || (x & 0xff) == 0xcc;
				if (padding) { pad++; continue; }
				code++;
				codeBytes += b.length;
				Instruction prev = l.getInstructionBefore(r.getMinAddress());
				String target = "-", nr = "-";
				if (prev != null && prev.getFlowType().isCall()) {
					Address[] flows = prev.getFlows();
					if (flows.length > 0) {
						target = flows[0].toString();
						Function f = getFunctionAt(flows[0]);
						nr = f == null ? "nofunc" : String.valueOf(f.hasNoReturn());
					}
				}
				String key = (prev == null ? "-" : prev.getMnemonicString()) + " noreturn=" + nr;
				byPrev.merge(key, 1, Integer::sum);
				StringBuilder hex = new StringBuilder();
				for (int i = 0; i < Math.min(8, b.length); i++) hex.append(String.format("%02x", b[i] & 0xff));
				w.printf("%s\t%d\t%s\t%s\t%s\t%s%n", r.getMinAddress(), b.length, prev, target, nr, hex);
			}
		}
		println("padding runs " + pad + ", other runs " + code + " (" + codeBytes + " bytes)");
		for (var e : byPrev.entrySet()) println("  " + e.getValue() + "\t" + e.getKey());
	}
}
