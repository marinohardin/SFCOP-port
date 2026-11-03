// Diagnostic: for the first N runs of instructions outside functions, show what precedes them
// and what references them. Args: <out.txt> <N>
//@category SFCOP
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
public class SfcOrphanProbe extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = getScriptArgs();
		int n = Integer.parseInt(a[1]);
		Listing l = currentProgram.getListing();
		FunctionManager fm = currentProgram.getFunctionManager();
		try (PrintWriter out = new PrintWriter(a[0])) {
			Address last = null;
			for (Instruction ins : l.getInstructions(currentProgram.getMemory().getBlock(".text").getStart(), true)) {
				if (n <= 0) break;
				if (fm.getFunctionContaining(ins.getAddress()) != null) { last = null; continue; }
				if (last != null) continue;
				last = ins.getAddress();
				n--;
				Instruction b = l.getInstructionBefore(ins.getAddress());
				out.printf("RUN %s %s%n  before: %s %s flow=%s fn=%s adj=%s%n", ins.getAddress(), ins,
					b == null ? "-" : b.getAddress(), b, b == null ? "-" : b.getFlowType(),
					b == null ? "-" : fm.getFunctionContaining(b.getAddress()),
					b != null && b.getMaxAddress().next().equals(ins.getAddress()));
				for (Reference r : currentProgram.getReferenceManager().getReferencesTo(ins.getAddress())) {
					out.printf("  ref from %s %s fn=%s%n", r.getFromAddress(), r.getReferenceType(),
						fm.getFunctionContaining(r.getFromAddress()));
				}
				Function fb = getFunctionBefore(ins.getAddress());
				out.printf("  function before: %s %s body=%s%n", fb, fb == null ? "" : fb.getEntryPoint(), fb == null ? "" : fb.getBody());
			}
		}
	}
}
