// Diagnostic: prints the code units in an address range with the function that owns each.
// Args: <start hex> <end hex>
//@category SFCOP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.*;

public class SfcUnitDump extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = getScriptArgs();
		AddressSet set = new AddressSet(toAddr(Long.parseLong(a[0], 16)), toAddr(Long.parseLong(a[1], 16)));
		for (CodeUnit cu : currentProgram.getListing().getCodeUnits(set, true)) {
			Function f = getFunctionContaining(cu.getMinAddress());
			println(cu.getMinAddress() + "\t" + cu.getLength() + "\t" + cu + "\t" + (f == null ? "-" : f.getName() + "@" + f.getEntryPoint()));
		}
	}
}
