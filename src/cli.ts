// Usage: node src/cli.ts <command> [--game dir] [--out dir]
//   --game       the Windows version's folder (default ../game3.30/Kilrathi_Saga: the 30th Anniversary
//                Edition's Kilrathi_Saga, as GOG's installer has it or extracted with innoextract)
//   unpack       WC3W.EXE and XANLIB.DLL mapped as Windows loads them, to out/<module>/<module>.bin
//                (out/wc3w/wc3w.bin), with their imports, exports and relocations in pe.json there
//   imports      unpack, then list what the modules import (the Windows API they need), by DLL
//   decompile    unpack, import each module into a Ghidra project (out/ghidra) and analyze (long)

import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";
import { findGhidra, runHeadless } from "./ghidra.ts";
import { type PE, parsePE, rebase } from "./pe.ts";

const root = path.resolve(import.meta.dirname, "..");
const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: {
    game: { type: "string", default: path.resolve(root, "../game3.30/Kilrathi_Saga") },
    out: { type: "string", default: path.join(root, "out") },
  },
});

// Wing Commander III: Heart of the Tiger, The Kilrathi Saga's Windows version (1996, MSVC 4.2), from
// GOG's 30th Anniversary Edition (2026). Only these files (by SHA-256): the addresses worked out
// depend on them. That WC3W.EXE imports DirectDrawCreate from WC3EN.dll (the enhancement patch)
// where the original imports it from DDRAW.dll; nothing else differs. XanLib: Origin's audio
// streaming library.
// at: where a module is mapped. XanLib's own base (90000000h) is far above the rest; it has base
// relocations, so it goes below the program, which keeps everything in the low addresses.
interface Module { file: string; sha256: string; at?: number }
const MODULES: Module[] = [
  { file: "WC3W.EXE", sha256: "1fffd1840356ea09b4e475c6648eda98e9485e1e421336dc927a25db274cd91f" },
  { file: "XANLIB.DLL", sha256: "5b7f494a7f78d08739892e489b6f29b488b1939d034a0f074beafceb01290c4f", at: 0x00200000 },
];

const gameDir = path.resolve(values.game);
const out = path.resolve(values.out);
const projectDir = path.join(out, "ghidra");
const scriptPath = path.join(root, "ghidra_scripts");
const projectName = "WC3W";
const hex = (n: number): string => n.toString(16);
// out/wc3w/wc3w.bin, out/xanlib/xanlib.bin: Ghidra names a program after its file
const stem = (m: Module): string => m.file.toLowerCase().replace(/\.[^.]*$/, "");
const moduleDir = (m: Module): string => path.join(out, stem(m));
const imageFile = (m: Module): string => path.join(moduleDir(m), `${stem(m)}.bin`);

/** Each module mapped (and moved where it goes), its image, pe.json and labels.txt in out/<module>/. */
function doUnpack(): Map<Module, PE> {
  const result = new Map<Module, PE>();
  for (const m of MODULES) {
    const file = path.join(gameDir, m.file);
    if (!existsSync(file)) throw new Error(`no ${m.file} in ${gameDir} (the Kilrathi_Saga folder: --game)`);
    const bytes = readFileSync(file);
    const sha = createHash("sha256").update(bytes).digest("hex");
    if (sha !== m.sha256) {
      throw new Error(`${file} is not the expected one (SHA-256 ${sha}, expected ${m.sha256}); ` +
        "this project needs the 30th Anniversary Edition's");
    }
    const pe = parsePE(bytes);
    if (m.at !== undefined && m.at !== pe.base) rebase(pe, m.at);
    const dir = moduleDir(m);
    mkdirSync(dir, { recursive: true });
    writeFileSync(imageFile(m), pe.image);
    const { image: _image, ...info } = pe;
    writeFileSync(path.join(dir, "pe.json"), JSON.stringify(info, null, 1));
    // names for Ghidra: the import address table's slots, the exports, the entry point
    const labels = [
      `${hex(pe.entry)} ${pe.isDll ? "DllMain" : "entry"} entry`,
      ...pe.imports.map((i) => `${hex(i.iat)} __imp_${i.dll.replace(/\.dll$/i, "")}_${i.name ?? `ord${i.ordinal}`} iat`),
      ...pe.exports.filter((e) => typeof e.address === "number")
        .map((e) => `${hex(e.address as number)} ${ghidraName(e.name ?? `ord${e.ordinal}`)} export`),
    ];
    writeFileSync(path.join(dir, "labels.txt"), labels.join("\n") + "\n");
    console.log(`${m.file}: ${pe.image.length} bytes at ${hex(pe.base)}, entry ${hex(pe.entry)}, ` +
      `${pe.imports.length} imports, ${pe.exports.length} exports, ${pe.relocations.length} relocations`);
    result.set(m, pe);
  }
  return result;
}

/** A symbol name Ghidra takes: C++ decorated names have characters it doesn't. */
const ghidraName = (s: string): string => s.replace(/[^A-Za-z0-9_@?$]/g, "_");

/** What each module imports, by DLL, and which modules export what others import. */
function doImports(): void {
  const pes = doUnpack();
  const exported = new Map<string, Set<string>>(); // DLL name (upper case) -> its exports
  for (const [m, pe] of pes) {
    exported.set((pe.exportName ?? m.file).toUpperCase(), new Set(pe.exports.map((e) => e.name ?? `#${e.ordinal}`)));
    exported.set(m.file.toUpperCase(), exported.get((pe.exportName ?? m.file).toUpperCase())!);
  }
  let total = 0;
  for (const [m, pe] of pes) {
    console.log(`\n${m.file}:`);
    const byDll = new Map<string, string[]>();
    for (const i of pe.imports) {
      const list = byDll.get(i.dll) ?? [];
      list.push(i.name ?? `#${i.ordinal}`);
      byDll.set(i.dll, list);
    }
    for (const [dll, names] of [...byDll].sort(([a], [b]) => a.localeCompare(b))) {
      const own = exported.has(dll.toUpperCase()) ? " (recompiled with it)" : "";
      if (!own) total += names.length;
      console.log(`  ${dll}${own}: ${names.length}`);
      console.log(names.sort().map((n) => `    ${n}`).join("\n"));
    }
  }
  console.log(`\n${total} functions from Windows (and the enhancement patch's DirectDrawCreate); ` +
    "LoadLibrary/GetProcAddress add those found at run time");
}

async function doDecompile(): Promise<void> {
  const pes = doUnpack();
  mkdirSync(projectDir, { recursive: true });
  for (const [m, pe] of pes) {
    const dir = moduleDir(m);
    await runHeadless(findGhidra(), [
      projectDir, projectName,
      "-import", imageFile(m), "-overwrite", "-loader", "BinaryLoader", "-loader-baseAddr", `0x${hex(pe.base)}`,
      "-processor", "x86:LE:32:default", "-cspec", "windows",
      "-scriptPath", scriptPath, "-preScript", "WcPeSetup.java", path.join(dir, "labels.txt"),
      "-analysisTimeoutPerFile", "7200",
    ]);
  }
}

async function main(): Promise<void> {
  switch (positionals[0]) {
    case "unpack":
      doUnpack();
      break;
    case "imports":
      doImports();
      break;
    case "decompile":
      await doDecompile();
      break;
    default:
      console.error("usage: node src/cli.ts <unpack|imports|decompile> [--game dir] [--out dir]");
      process.exit(2);
  }
}

main().catch((err: unknown) => {
  console.error(err instanceof Error ? err.message : err);
  process.exit(1);
});
