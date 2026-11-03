// Diagnostic: for each candidate missed data pointer from tools/validate.py, what Ghidra has at
// the dword and at the value it holds.
// Args: <stale_candidates.tsv> <out.tsv>
//@category SFCOP

import java.io.*;
import java.nio.file.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Symbol;

public class SfcStaleProbe extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		Listing l = currentProgram.getListing();
		try (PrintWriter w = new PrintWriter(new File(args[1]))) {
			w.println("address\tvalue\tunit\tunit_start\tunit_symbol\ttarget_kind\ttarget_symbol\trefs_to_target");
			for (String line : Files.readAllLines(Path.of(args[0]))) {
				String[] f = line.split("\t");
				if (f[0].equals("address")) continue;
				Address a = toAddr(Long.parseLong(f[0], 16));
				Address t = toAddr(Long.parseLong(f[1], 16));
				CodeUnit cu = l.getCodeUnitContaining(a);
				String unit = cu == null ? "-" : cu instanceof Instruction ? "insn" : ((Data) cu).getDataType().getName();
				Symbol us = cu == null ? null : getSymbolAt(cu.getMinAddress());
				String kind;
				if (getFunctionAt(t) != null) kind = "function";
				else if (l.getInstructionAt(t) != null) kind = "instruction";
				else if (l.getInstructionContaining(t) != null) kind = "mid-instruction";
				else {
					Data d = l.getDataContaining(t);
					kind = d == null ? "nothing" : (d.getMinAddress().equals(t) ? "data:" : "mid-data:") + d.getDataType().getName();
				}
				Symbol ts = getSymbolAt(t);
				w.printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d%n", f[0], f[1], unit, cu == null ? "-" : cu.getMinAddress(),
					us == null ? "-" : us.getName(true), kind, ts == null ? "-" : ts.getName(true),
					currentProgram.getReferenceManager().getReferenceCountTo(t));
			}
		}
	}
}
