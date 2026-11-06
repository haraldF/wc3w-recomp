// Before analysis of a mapped PE module (WC3W.EXE, XANLIB.DLL; src/pe.ts): names from labels.txt,
// one per line "<address hex> <name> <kind>": the entry point and exports become functions, the
// import address table's slots get their import's name and a pointer each, so calls through them
// read as CALL [__imp_KERNEL32_Sleep].
// Args: <labels file>
//@category WC

import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.symbol.SourceType;

public class WcPeSetup extends GhidraScript {

	@Override
	protected void run() throws Exception {
		for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
			String[] w = line.trim().split("\\s+");
			if (w.length < 3) {
				continue;
			}
			Address a = toAddr(Long.parseLong(w[0], 16));
			switch (w[2]) {
				case "entry", "export" -> {
					currentProgram.getSymbolTable().addExternalEntryPoint(a);
					disassemble(a);
					if (getFunctionAt(a) == null) {
						createFunction(a, w[1]);
					}
					else {
						getFunctionAt(a).setName(w[1], SourceType.IMPORTED);
					}
				}
				case "iat" -> {
					createLabel(a, w[1], true, SourceType.IMPORTED);
					clearListing(a, a.add(3));
					createData(a, PointerDataType.dataType);
				}
				default -> throw new IllegalArgumentException("unknown label kind: " + line);
			}
		}
	}
}
