// Synthesizes relocations for an address set and exports it as one COFF object with
// ghidra-delinker-extension. Unlike the extension's DelinkProgram script it calls the analyzer
// directly, so the full analyzer and exporter logs (Ghidra caps its own message log) and the
// relocation table land in files.
//
// Args: <out.obj> <outdir for logs> <spec>...
//   +START-END   add an address range       -START-END   remove an address range
//   -fn:ADDR     remove the body of the function at ADDR (to replace it with new code)
//@category SFCOP

import java.io.*;
import java.util.*;

import ghidra.app.analyzers.RelocationTableSynthesizerAnalyzer;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.Option;
import ghidra.app.util.exporter.CoffRelocatableObjectExporter;
import ghidra.app.util.importer.MessageLog;
import ghidra.framework.options.Options;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.relocobj.*;
import ghidra.program.model.symbol.Symbol;

public class SfcDelinkExport extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File obj = new File(args[0]);
		File dir = new File(args[1]);
		dir.mkdirs();
		String stem = obj.getName().replaceAll("\\.obj$", "");

		AddressSet set = new AddressSet();
		for (int i = 2; i < args.length; i++) {
			String a = args[i];
			if (a.startsWith("-fn:")) {
				Function f = getFunctionAt(toAddr(Long.parseLong(a.substring(4), 16)));
				set.delete(f.getBody());
				println("excluded " + f.getName(true) + " " + f.getBody());
				continue;
			}
			String[] r = a.substring(1).split("-");
			AddressSet range = new AddressSet(toAddr(Long.parseLong(r[0], 16)), toAddr(Long.parseLong(r[1], 16)));
			if (a.charAt(0) == '+') set.add(range);
			else set.delete(range);
		}

		String an = "Relocation table synthesizer";
		Options analysis = currentProgram.getOptions(Program.ANALYSIS_PROPERTIES);
		// A call through an import thunk must stay a call to the thunk: the thunk's target is an
		// external location, which has no address in the object.
		analysis.setBoolean(an + ".Redirect thunks to targets", false);
		analysis.setEnum(an + ".Evaluation report policy",
			RelocationTableSynthesizerAnalyzer.EvaluationReportPolicy.ON_FAILURE);
		RelocationTableSynthesizerAnalyzer analyzer = new RelocationTableSynthesizerAnalyzer();
		analyzer.optionsChanged(analysis.getOptions(an), currentProgram);

		long t0 = System.currentTimeMillis();
		MessageLog alog = new MessageLog();
		analyzer.added(currentProgram, set, monitor, alog);
		long t1 = System.currentTimeMillis();
		try (PrintWriter w = new PrintWriter(new File(dir, stem + ".analyzer.log"))) {
			w.print(alog.toString());
		}

		RelocationTable table = RelocationTable.get(currentProgram);
		int nabs = 0, nrel = 0, nother = 0;
		try (PrintWriter w = new PrintWriter(new File(dir, stem + ".relocations.tsv"))) {
			w.println("address\tkind\twidth\ttarget\taddend\ttarget_symbol");
			Iterator<Relocation> it = table.getRelocations(set);
			while (it.hasNext()) {
				Relocation r = it.next();
				String kind = r instanceof RelocationAbsolute ? "abs"
						: r instanceof RelocationRelativePC ? "relpc" : r.getClass().getSimpleName();
				if (kind.equals("abs")) nabs++;
				else if (kind.equals("relpc")) nrel++;
				else nother++;
				Symbol s = getSymbolAt(r.getTarget());
				w.printf("%s\t%s\t%d\t%s\t%d\t%s%n", r.getAddress(), kind, r.getWidth(), r.getTarget(),
					r.getAddend(), s == null ? "-" : s.getName(true));
			}
		}

		// Every 4-byte value inside an instruction that is an address in the image but is not
		// covered by a relocation: after relinking at another base it would still point into the
		// old image. Constants that prep judged not to be addresses show up here too; the
		// relink validator (tools/validate.py) separates them.
		int missing = 0;
		long lo = currentProgram.getImageBase().getOffset();
		long hi = currentProgram.getMemory().getBlock(".rsrc").getEnd().getOffset();
		try (PrintWriter w = new PrintWriter(new File(dir, stem + ".missing.tsv"))) {
			w.println("address\toffset\tvalue\tfunction\treferences\tinstruction");
			for (Instruction ins : currentProgram.getListing().getInstructions(set, true)) {
				byte[] b = ins.getBytes();
				Set<Long> values = new HashSet<>();
				for (int op = 0; op < ins.getNumOperands(); op++) {
					for (Object o : ins.getOpObjects(op)) {
						if (o instanceof ghidra.program.model.scalar.Scalar sc && sc.bitLength() == 32) {
							values.add(sc.getUnsignedValue());
						}
					}
				}
				for (int i = 0; i + 4 <= b.length; i++) {
					long v = (b[i] & 0xffL) | (b[i + 1] & 0xffL) << 8 | (b[i + 2] & 0xffL) << 16 | (b[i + 3] & 0xffL) << 24;
					if (v < lo || v > hi || !values.contains(v)) continue;
					boolean covered = false;
					for (int j = Math.max(0, i - 3); j <= i + 3 && !covered; j++) {
						Relocation r = table.getRelocationAt(ins.getAddress().add(j));
						if (r != null && j <= i && j + r.getWidth() >= i + 4) covered = true;
					}
					if (covered) continue;
					StringBuilder refs = new StringBuilder();
					for (var r : ins.getReferencesFrom()) refs.append(r.getToAddress()).append(' ');
					Function f = getFunctionContaining(ins.getAddress());
					w.printf("%s\t%d\t%08x\t%s\t%s\t%s%n", ins.getAddress(), i, v, f == null ? "-" : f.getName(true),
						refs.toString().trim(), ins);
					missing++;
				}
			}
		}

		CoffRelocatableObjectExporter exporter = new CoffRelocatableObjectExporter();
		List<Option> options = new ArrayList<>(exporter.getOptions(() -> currentProgram));
		List<Option> chosen = new ArrayList<>();
		for (Option o : options) {
			if (o.getName().equals("Generate .comment section")) o.setValue(false);
			chosen.add(o);
		}
		exporter.setOptions(chosen);
		boolean ok = exporter.export(obj, currentProgram, set, monitor);
		long t2 = System.currentTimeMillis();
		try (PrintWriter w = new PrintWriter(new File(dir, stem + ".exporter.log"))) {
			w.print(exporter.getMessageLog().toString());
		}
		try (PrintWriter w = new PrintWriter(new File(dir, stem + ".summary.txt"))) {
			w.printf("bytes\t%d%nranges\t%d%nrelocations_absolute\t%d%nrelocations_relative\t%d%n" +
				"relocations_other\t%d%ninstruction_dwords_in_image_without_relocation\t%d%n" +
				"analyzer_ms\t%d%nexporter_ms\t%d%nexport_ok\t%s%n",
				set.getNumAddresses(), set.getNumAddressRanges(), nabs, nrel, nother, missing, t1 - t0, t2 - t1, ok);
		}
		if (!ok) throw new RuntimeException("export failed, see " + stem + ".exporter.log");
		println(String.format("%s: %d bytes, %d abs + %d rel relocations, analyzer %d ms, exporter %d ms",
			obj, set.getNumAddresses(), nabs, nrel, t1 - t0, t2 - t1));
	}
}
