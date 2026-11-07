// N1: which transport a Dynaverse mission's combat session uses (DESIGN §12.6). Read-only: run with
// -readOnly so the project is never saved.
//   arg 0  output directory
//   arg 1+ name=start-end (hex, end exclusive): named address regions. Regions whose name starts
//          with "src" are the sources; "dst" regions are destinations walked backwards; every
//          other region is a target.
// For every function in a source region, a breadth-first walk over call edges (direct calls,
// references from code to a function's entry, which is how function pointers and callbacks are
// pushed, and the fall-through into a body split off after a non-returning call) finds the
// shortest path to each target region, up to MAX_DEPTH calls deep, never walking through a
// function inside another target region.
// Writes:
//   paths.tsv     source function, target region, depth, the path (entry addresses, " > ")
//   rpaths.tsv    for every function in a "dst" region, a backward walk over the same edges finds
//                 the shortest chain of callers that starts in each "src" region: destination,
//                 source region, depth, the path from the source down to the destination
//   edges.tsv     every direct edge from a source function into a target region
//   strings.tsv   every string a source function references
//   decomp/<addr>.c decompiled pseudocode of every source function and every function on a path
//@category SFCOP
import java.io.*;
import java.util.*;

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class DvMission extends GhidraScript {
	static final int MAX_DEPTH = 6;

	FunctionManager fm;
	ReferenceManager rm;
	Listing listing;
	final Map<String, AddressSet> regions = new LinkedHashMap<>();

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args[0]);
		File dec = new File(out, "decomp");
		dec.mkdirs();
		fm = currentProgram.getFunctionManager();
		rm = currentProgram.getReferenceManager();
		listing = currentProgram.getListing();
		for (int i = 1; i < args.length; i++) {
			String[] kv = args[i].split("=");
			String[] p = kv[1].split("-");
			regions.computeIfAbsent(kv[0], k -> new AddressSet())
				.add(toAddr(Long.parseLong(p[0], 16)), toAddr(Long.parseLong(p[1], 16) - 1));
		}

		List<Function> sources = new ArrayList<>();
		for (Map.Entry<String, AddressSet> e : regions.entrySet()) {
			if (!e.getKey().startsWith("src")) continue;
			for (Function f : fm.getFunctions(e.getValue(), true)) sources.add(f);
		}
		sources.sort(Comparator.comparing(Function::getEntryPoint));

		Set<Function> onPaths = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
		try (PrintWriter pw = new PrintWriter(new FileWriter(new File(out, "paths.tsv")));
		     PrintWriter ew = new PrintWriter(new FileWriter(new File(out, "edges.tsv")));
		     PrintWriter sw = new PrintWriter(new FileWriter(new File(out, "strings.tsv")))) {
			pw.println("source\tsource_name\ttarget_region\tdepth\tpath");
			ew.println("source\tsource_name\tcall_site\ttarget\ttarget_name\ttarget_region\tkind");
			sw.println("function\tname\tstring_address\tstring");
			for (Function s : sources) {
				if (monitor.isCancelled()) break;
				for (String[] e : edges(s)) {
					Function t = fm.getFunctionAt(toAddr(Long.parseLong(e[1], 16)));
					if (t == null) continue;
					String reg = regionOf(t.getEntryPoint());
					if (reg != null && !reg.startsWith("src"))
						ew.println(s.getEntryPoint() + "\t" + s.getName() + "\t" + e[0] + "\t" + t.getEntryPoint() + "\t" + t.getName() + "\t" + reg + "\t" + e[2]);
				}
				for (String[] st : strings(s)) sw.println(s.getEntryPoint() + "\t" + s.getName() + "\t" + st[0] + "\t" + st[1]);
				for (Map.Entry<String, List<Function>> hit : shortestPaths(s).entrySet()) {
					List<Function> path = hit.getValue();
					StringBuilder b = new StringBuilder();
					for (Function f : path) {
						if (b.length() > 0) b.append(" > ");
						b.append(f.getEntryPoint());
					}
					pw.println(s.getEntryPoint() + "\t" + s.getName() + "\t" + hit.getKey() + "\t" + (path.size() - 1) + "\t" + b);
					onPaths.addAll(path);
				}
			}
		}

		List<Function> dsts = new ArrayList<>();
		for (Map.Entry<String, AddressSet> e : regions.entrySet()) {
			if (!e.getKey().startsWith("dst")) continue;
			for (Function f : fm.getFunctions(e.getValue(), true)) dsts.add(f);
		}
		try (PrintWriter rw = new PrintWriter(new FileWriter(new File(out, "rpaths.tsv")))) {
			rw.println("destination\tdestination_name\tsource_region\tdepth\tpath");
			for (Function d : dsts) {
				if (monitor.isCancelled()) break;
				for (Map.Entry<String, List<Function>> hit : callerPaths(d).entrySet()) {
					List<Function> path = hit.getValue();
					StringBuilder b = new StringBuilder();
					for (Function f : path) {
						if (b.length() > 0) b.append(" > ");
						b.append(f.getEntryPoint());
					}
					rw.println(d.getEntryPoint() + "\t" + d.getName() + "\t" + hit.getKey() + "\t" + (path.size() - 1) + "\t" + b);
					onPaths.addAll(path);
				}
			}
		}

		onPaths.addAll(sources);
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		for (Function f : onPaths) {
			if (monitor.isCancelled()) break;
			DecompileResults r = di.decompileFunction(f, 60, monitor);
			String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed\n";
			try (PrintWriter cw = new PrintWriter(new FileWriter(new File(dec, f.getEntryPoint() + ".c")))) {
				cw.println("// " + f.getName() + " (region " + regionOf(f.getEntryPoint()) + ")");
				cw.print(c);
			}
		}
		di.dispose();
		println("DvMission: " + sources.size() + " source functions, " + onPaths.size() + " functions on paths");
	}

	String regionOf(Address a) {
		for (Map.Entry<String, AddressSet> e : regions.entrySet())
			if (e.getValue().contains(a)) return e.getKey();
		return null;
	}

	// {call site, callee entry (hex), kind}: direct calls, and references from code to a function's
	// entry point (a pushed or stored function pointer).
	final Map<Function, List<String[]>> edgeCache = new HashMap<>();

	List<String[]> edges(Function f) {
		List<String[]> cached = edgeCache.get(f);
		if (cached != null) return cached;
		List<String[]> out = new ArrayList<>();
		edgeCache.put(f, out);
		Set<Address> seen = new HashSet<>();
		for (Address from : f.getBody().getAddresses(true)) {
			for (Reference r : rm.getReferencesFrom(from)) {
				Address to = r.getToAddress();
				Function t = fm.getFunctionAt(to);
				if (t == null || t.equals(f)) continue;
				if (t.isThunk()) t = t.getThunkedFunction(true);
				if (t == null || t.isExternal() || !t.getEntryPoint().isMemoryAddress()) continue;
				String kind = r.getReferenceType().isCall() ? "call" : r.getReferenceType().isJump() ? "jump" : "ref";
				if (seen.add(t.getEntryPoint()) || kind.equals("call"))
					out.add(new String[] {from.toString(), t.getEntryPoint().toString(), kind});
			}
		}
		// MSVC's __EH_prolog stub: the shared program often ends a function at a call to a helper it
		// takes for non-returning (the frame-setup helper of C++ functions with unwind state), and
		// makes the rest of the body a separate function at the next address. That body is reached
		// by falling through, so it is an edge too.
		Address last = f.getBody().getMaxAddress();
		Instruction li = listing.getInstructionContaining(last);
		if (li != null && li.getFlowType().isCall()) {
			Function next = fm.getFunctionAt(li.getMaxAddress().next());
			if (next != null && !next.equals(f)) out.add(new String[] {li.getAddress().toString(), next.getEntryPoint().toString(), "fallthrough"});
		}
		return out;
	}

	List<String[]> strings(Function f) {
		List<String[]> out = new ArrayList<>();
		for (Address from : f.getBody().getAddresses(true)) {
			for (Reference r : rm.getReferencesFrom(from)) {
				Data d = listing.getDataAt(r.getToAddress());
				if (d == null || !d.hasStringValue()) continue;
				String v = StringDataInstance.getStringDataInstance(d).getStringValue();
				if (v != null) out.add(new String[] {r.getToAddress().toString(), v.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')});
			}
		}
		return out;
	}

	// The functions with an edge to f: code references to its entry (calls, jumps, pushed
	// pointers), and the function before it when f is a body split off after a non-returning call.
	List<Function> callers(Function f) {
		List<Function> out = new ArrayList<>();
		for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
			Function c = fm.getFunctionContaining(r.getFromAddress());
			if (c != null && !c.equals(f) && !out.contains(c)) out.add(c);
		}
		Instruction prev = listing.getInstructionBefore(f.getEntryPoint());
		if (prev != null && prev.getFlowType().isCall() && prev.getMaxAddress().next().equals(f.getEntryPoint())) {
			Function c = fm.getFunctionContaining(prev.getAddress());
			if (c != null && !c.equals(f) && c.getBody().getMaxAddress().equals(prev.getMaxAddress()) && !out.contains(c)) out.add(c);
		}
		return out;
	}

	// Breadth-first up the callers from d; the first function found in each source region gives
	// that region's path. The walk does not continue above a source function.
	Map<String, List<Function>> callerPaths(Function d) {
		Map<String, List<Function>> found = new TreeMap<>();
		Map<Function, Function> child = new HashMap<>();
		Map<Function, Integer> depth = new HashMap<>();
		Deque<Function> q = new ArrayDeque<>();
		q.add(d);
		depth.put(d, 0);
		child.put(d, null);
		while (!q.isEmpty()) {
			Function f = q.poll();
			int n = depth.get(f);
			String reg = regionOf(f.getEntryPoint());
			if (f != d && reg != null && reg.startsWith("src")) {
				if (!found.containsKey(reg)) {
					List<Function> path = new ArrayList<>();
					for (Function p = f; p != null; p = child.get(p)) path.add(p);
					found.put(reg, path);
				}
				continue;
			}
			if (n >= MAX_DEPTH) continue;
			for (Function c : callers(f)) {
				if (depth.containsKey(c)) continue;
				depth.put(c, n + 1);
				child.put(c, f);
				q.add(c);
			}
		}
		return found;
	}

	// Breadth-first from s; the first time a target region is entered, its path is kept. The walk
	// does not continue through a function in a target region.
	Map<String, List<Function>> shortestPaths(Function s) {
		Map<String, List<Function>> found = new TreeMap<>();
		Map<Function, Function> parent = new HashMap<>();
		Deque<Function> q = new ArrayDeque<>();
		Map<Function, Integer> depth = new HashMap<>();
		q.add(s);
		depth.put(s, 0);
		parent.put(s, null);
		while (!q.isEmpty()) {
			Function f = q.poll();
			int d = depth.get(f);
			String reg = regionOf(f.getEntryPoint());
			if (f != s && reg != null && !reg.startsWith("src")) {
				if (!found.containsKey(reg)) {
					LinkedList<Function> path = new LinkedList<>();
					for (Function p = f; p != null; p = parent.get(p)) path.addFirst(p);
					found.put(reg, path);
				}
				continue;
			}
			if (d >= MAX_DEPTH) continue;
			for (String[] e : edges(f)) {
				Function t = fm.getFunctionAt(toAddr(Long.parseLong(e[1], 16)));
				if (t == null || depth.containsKey(t)) continue;
				depth.put(t, d + 1);
				parent.put(t, f);
				q.add(t);
			}
		}
		return found;
	}
}
