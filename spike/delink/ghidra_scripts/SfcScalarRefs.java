// Lists every memory reference that comes from an instruction's immediate (scalar) operand,
// with what sits at the target, so false references (masks, sizes) can be told from pointers.
// Also lists immediates and data dwords that look like image addresses but carry no reference.
// Arg 0: output TSV.
//@category SFCOP

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class SfcScalarRefs extends GhidraScript {
	String kind(Address a) {
		Listing l = currentProgram.getListing();
		if (!currentProgram.getMemory().contains(a)) return "nomem";
		Function f = currentProgram.getFunctionManager().getFunctionAt(a);
		if (f != null) return "func";
		CodeUnit cu = l.getCodeUnitContaining(a);
		if (cu instanceof Instruction) return cu.getMinAddress().equals(a) ? "insn" : "mid-insn";
		if (cu instanceof Data d) {
			if (!d.isDefined()) return "undef";
			if (!cu.getMinAddress().equals(a)) return "mid-data:" + d.getDataType().getName();
			return "data:" + d.getDataType().getName();
		}
		return "none";
	}

	String sym(Address a) {
		Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(a);
		if (s == null) return "-";
		return (s.isDynamic() ? "dyn:" : "") + s.getName(true);
	}

	@Override
	public void run() throws Exception {
		Address lo = toAddr(0x400000), hi = toAddr(0xc85fff);
		try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
			out.println("from\tmnemonic\top\thas_ref\tref_source\tvalue\ttarget_kind\ttarget_symbol\tinsn");
			for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
				for (int op = 0; op < ins.getNumOperands(); op++) {
					int t = ins.getOperandType(op);
					if (!OperandType.isScalar(t) || OperandType.isDynamic(t)) continue;
					Scalar s = ins.getScalar(op);
					if (s == null) continue;
					long v = s.getUnsignedValue();
					if (v < lo.getOffset() || v > hi.getOffset()) continue;
					Address a = toAddr(v);
					String src = "-";
					for (Reference r : ins.getOperandReferences(op)) {
						if (r.getToAddress().equals(a)) src = r.getSource().toString();
					}
					out.printf("%s\t%s\t%d\t%s\t%s\t%08x\t%s\t%s\t%s%n", ins.getAddress(), ins.getMnemonicString(), op,
						src.equals("-") ? "0" : "1", src, v, kind(a), sym(a), ins);
				}
			}
		}
	}
}
