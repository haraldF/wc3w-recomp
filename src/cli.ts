// Usage: node src/cli.ts <command> [--game dir] [--out dir] [--seconds n] [--force] [--realtime] [--other dir]
//   --game       the Windows version's folder (default ../game3.30/Kilrathi_Saga: the 30th Anniversary
//                Edition's Kilrathi_Saga, as GOG's installer has it or extracted with innoextract)
//   --force      recomp: run the generator even when its inputs haven't changed
//   unpack       WC3W.EXE and XANLIB.DLL mapped as Windows loads them and linked into one image
//                (out/image.bin, from IMAGE_LO), its modules and imports in out/program.json
//   imports      unpack, then list what the modules import (the Windows API they need), by DLL
//   decompile    unpack, import the image into a Ghidra project (out/ghidra) and analyze it
//   recomp       translate the analyzed program to C++ (out/recomp/) and build out/bin/wc3w; the
//                generator runs only when its inputs changed (out/recomp/inputs.json)
//   build        only (re)build out/bin/wc3w from out/recomp/ and runtime/ (CMake, in out/build/);
//                refused when the generator's inputs (hooks.txt, entries.txt...) changed since
//   smoke        run the game headless in a fresh copy of the game folder (out/smoke/game, without
//                saves) through the scenarios (all, or those named: "flight"...), from the intro
//                on; fails on a stop, an error or a picture that isn't the scenario's. With
//                virtual time (the same run every time, not waiting); --realtime: by the real clock
//   discover     run out/bin/wc3w headless (each run for --seconds, default 60; WC_* from the
//                environment); when it reaches code that wasn't recompiled (reached only through
//                pointers in data), add that entry point to entries.txt, recompile, repeat
//   play         the same in the window: after each recompile the game starts again
//   drift        compare the files shared with wc4w-recomp (--other, default ../wc4w-recomp): the
//                number of lines that differ, by file (wc4w-recomp's WC3W-DIFFERENCES.md explains them)
// Each run's stderr is kept in out/logs/.

import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  appendFileSync, copyFileSync, createWriteStream, existsSync, mkdirSync, readdirSync, readFileSync, rmSync, symlinkSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";
import { inflateSync } from "node:zlib";
import { findGhidra, runHeadless } from "./ghidra.ts";
import { type PE, parsePE, rebase } from "./pe.ts";

const root = path.resolve(import.meta.dirname, "..");
const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: {
    game: { type: "string", default: path.resolve(root, "../game3.30/Kilrathi_Saga") },
    out: { type: "string", default: path.join(root, "out") },
    seconds: { type: "string", default: "60" },
    force: { type: "boolean", default: false },
    realtime: { type: "boolean", default: false },
    other: { type: "string", default: path.resolve(root, "../wc4w-recomp") },
  },
});

// Wing Commander III: Heart of the Tiger, The Kilrathi Saga's Windows version (1996, MSVC 4.2), from
// GOG's 30th Anniversary Edition (2026). Only these files (by SHA-256): the addresses worked out
// depend on them. That WC3W.EXE imports DirectDrawCreate from WC3EN.dll (the enhancement patch)
// where the original imports it from DDRAW.dll; nothing else differs. XanLib: Origin's audio
// streaming library.
// at: where a module is mapped. XanLib's own base (90000000h) is far above the rest; it has base
// relocations, so it goes below the program: the image is one block from IMAGE_LO.
interface Module { file: string; sha256: string; at?: number }
const MODULES: Module[] = [
  { file: "XANLIB.DLL", sha256: "5b7f494a7f78d08739892e489b6f29b488b1939d034a0f074beafceb01290c4f", at: 0x00200000 },
  { file: "WC3W.EXE", sha256: "1fffd1840356ea09b4e475c6648eda98e9485e1e421336dc927a25db274cd91f" },
];
const IMAGE_LO = 0x00200000;
// Imports from Windows: each slot of the import address tables holds a thunk address, THUNK_BASE +
// THUNK_SIZE * its index in the runtime's import table (wc_imports.cpp); calling it runs the
// runtime's implementation (as in runtime/wc_rt.h). The DLL that WC3EN.dll stands in for:
const THUNK_BASE = 0xf0000000, THUNK_SIZE = 16;
const DLL_ALIASES: Record<string, string> = { "WC3EN.DLL": "DDRAW.DLL" };
const EXE = "WC3W.EXE";
const CYCLES = 0; // instructions per ms by default: unlimited (Windows programs pace themselves)

const gameDir = path.resolve(values.game);
const out = path.resolve(values.out);
const imageFile = path.join(out, "image.bin");
const programFile = path.join(out, "program.json");
const labelsFile = path.join(out, "labels.txt");
// Code pointers: the targets of the base relocations that point into code sections. The generator
// decodes those outside Ghidra's functions too: code reached only through pointers (callbacks,
// tables of functions), which the DOS version had to find by running it.
const codePointersFile = path.join(out, "code-pointers.txt");
// Addresses where the runtime may take over (runtime/fixes.cpp, movie.cpp: hook_register), kept in the repository
const hooksFile = path.join(root, "hooks.txt");
const projectDir = path.join(out, "ghidra");
const scriptPath = path.join(root, "ghidra_scripts");
const projectName = "WC3W";
const recompDir = path.join(out, "recomp");
const entriesFile = path.join(root, "entries.txt"); // kept in the repository
const wcExe = path.join(out, "bin", "wc3w");
const logsDir = path.join(out, "logs");
// What each step was made from (file: SHA-256), to skip or refuse steps whose inputs changed
const decompileStamp = path.join(projectDir, "inputs.json");
const recompStamp = path.join(recompDir, "inputs.json");
const decompileInputs = (): string[] => [imageFile, labelsFile, path.join(scriptPath, "WcPeSetup.java")];
const recompInputs = (): string[] => [decompileStamp, path.join(scriptPath, "WcRecomp.java"), entriesFile, codePointersFile, hooksFile];
const hex = (n: number): string => n.toString(16);

interface Program {
  lo: number;
  hi: number;
  modules: { file: string; base: number; size: number; entry: number; isDll: boolean; stackReserve: number;
    resources: number; resourcesSize: number }[];
  /** the Windows functions behind the thunks, by index */
  thunks: { dll: string; name: string }[];
}

/** The modules mapped, moved where they go and linked into one image (image.bin), with
 * program.json and the names for Ghidra (labels.txt). */
function doUnpack(): { program: Program; pes: Map<Module, PE> } {
  const pes = new Map<Module, PE>();
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
    pes.set(m, pe);
  }
  const hi = Math.max(...[...pes.values()].map((pe) => pe.base + pe.image.length));
  const image = Buffer.alloc(hi - IMAGE_LO);
  for (const pe of pes.values()) {
    if (pe.base < IMAGE_LO) throw new Error("a module below IMAGE_LO");
    pe.image.copy(image, pe.base - IMAGE_LO);
  }

  // Link: imports from the modules here go to their exports, those from Windows to thunks.
  const exportsOf = new Map<string, PE>();
  for (const [m, pe] of pes) {
    exportsOf.set(m.file.toUpperCase(), pe);
    if (pe.exportName) exportsOf.set(pe.exportName.toUpperCase(), pe);
  }
  const thunks: Program["thunks"] = [];
  const thunkIndex = new Map<string, number>();
  const labels: string[] = [];
  for (const [m, pe] of pes) {
    labels.push(`${hex(pe.entry)} ${pe.isDll ? `${stem(m)}_DllMain` : "entry"} entry`);
    for (const e of pe.exports) {
      if (typeof e.address === "number") labels.push(`${hex(e.address)} ${ghidraName(e.name ?? `${stem(m)}_ord${e.ordinal}`)} export`);
    }
    for (const i of pe.imports) {
      let target: number;
      const from = exportsOf.get(i.dll.toUpperCase());
      if (from) {
        const e = from.exports.find((x) => (i.name !== undefined ? x.name === i.name : x.ordinal === i.ordinal));
        if (!e || typeof e.address !== "number") throw new Error(`${m.file} imports ${i.dll}!${i.name ?? i.ordinal}, which isn't there`);
        target = e.address;
      }
      else {
        if (i.name === undefined) throw new Error(`${m.file} imports ${i.dll} #${i.ordinal} by ordinal`);
        const dll = DLL_ALIASES[i.dll.toUpperCase()] ?? i.dll.toUpperCase();
        const key = `${dll}!${i.name}`;
        let k = thunkIndex.get(key);
        if (k === undefined) {
          k = thunks.length;
          thunks.push({ dll, name: i.name });
          thunkIndex.set(key, k);
        }
        target = THUNK_BASE + THUNK_SIZE * k;
      }
      image.writeUInt32LE(target, i.iat - IMAGE_LO);
      labels.push(`${hex(i.iat)} __imp_${i.dll.replace(/\.dll$/i, "")}_${ghidraName(i.name ?? `ord${i.ordinal}`)} iat`);
    }
  }
  const program: Program = {
    lo: IMAGE_LO, hi, thunks,
    modules: [...pes].map(([m, pe]) => ({
      file: m.file, base: pe.base, size: pe.image.length, entry: pe.entry, isDll: pe.isDll,
      stackReserve: pe.stackReserve, resources: pe.resources.rva ? pe.base + pe.resources.rva : 0,
      resourcesSize: pe.resources.size,
    })),
  };
  mkdirSync(out, { recursive: true });
  writeFileSync(imageFile, image);
  writeFileSync(programFile, JSON.stringify(program, null, 1));
  writeFileSync(labelsFile, labels.join("\n") + "\n");
  const pointers = new Set<number>();
  for (const pe of pes.values()) {
    const code = pe.sections.filter((x) => x.characteristics & 0x20000020); // CNT_CODE or MEM_EXECUTE
    for (const r of pe.relocations) {
      const t = image.readUInt32LE(pe.base + r - IMAGE_LO);
      if (code.some((x) => t >= pe.base + x.rva && t < pe.base + x.rva + Math.max(x.virtualSize, x.rawSize))) pointers.add(t);
    }
  }
  writeFileSync(codePointersFile, [...pointers].sort((a, b) => a - b).map(hex).join("\n") + "\n");
  for (const [m, pe] of pes) {
    console.log(`${m.file}: ${pe.image.length} bytes at ${hex(pe.base)}, entry ${hex(pe.entry)}, ` +
      `${pe.imports.length} imports, ${pe.exports.length} exports, ${pe.relocations.length} relocations`);
  }
  console.log(`${imageFile}: ${hex(IMAGE_LO)}..${hex(hi)}, ${thunks.length} Windows functions imported`);
  return { program, pes };
}

const stem = (m: Module): string => m.file.toLowerCase().replace(/\.[^.]*$/, "");
/** A symbol name Ghidra takes: C++ decorated names have characters it doesn't. */
const ghidraName = (s: string): string => s.replace(/[^A-Za-z0-9_@?$]/g, "_");

/** What each module imports, by DLL. */
function doImports(): void {
  const { program, pes } = doUnpack();
  const linked = new Set([...pes].flatMap(([m, pe]) => [m.file.toUpperCase(), (pe.exportName ?? "").toUpperCase()]));
  for (const [m, pe] of pes) {
    console.log(`\n${m.file}:`);
    const byDll = new Map<string, string[]>();
    for (const i of pe.imports) {
      const list = byDll.get(i.dll) ?? [];
      list.push(i.name ?? `#${i.ordinal}`);
      byDll.set(i.dll, list);
    }
    for (const [dll, names] of [...byDll].sort(([a], [b]) => a.localeCompare(b))) {
      console.log(`  ${dll}${linked.has(dll.toUpperCase()) ? " (linked here)" : ""}: ${names.length}`);
      console.log(names.sort().map((n) => `    ${n}`).join("\n"));
    }
  }
  const missing = unimplemented(program);
  console.log(`\nnot implemented by the runtime (a call stops the program): ${missing.length ? missing.join(", ") : "none"}`);
  console.log("LoadLibrary/GetProcAddress add those found at run time");
}

type Stamp = Record<string, string>;
/** The files' hashes, and what else the step depends on (the Ghidra version). */
function stampOf(files: string[], also: Stamp = {}): Stamp {
  return { ...Object.fromEntries(files.map((f) => [path.relative(root, f),
    existsSync(f) ? createHash("sha256").update(readFileSync(f)).digest("hex") : "none"])), ...also };
}
function readStamp(file: string): Stamp | undefined {
  return existsSync(file) ? JSON.parse(readFileSync(file, "utf8")) as Stamp : undefined;
}
/** The files whose hash differs from the stamp's (all of them without a stamp). */
function changed(stampFile: string, files: string[], also: Stamp = {}): string[] {
  const old = readStamp(stampFile) ?? {};
  return Object.entries(stampOf(files, also)).filter(([f, h]) => old[f] !== h).map(([f]) => f);
}
/** Stops when a step's inputs changed since the stamp; without a stamp (made before stamps) only warns. */
function requireStamp(stampFile: string, files: string[], what: string, fix: string, also: Stamp = {}): void {
  const stale = changed(stampFile, files, also);
  if (!stale.length) return;
  if (!existsSync(stampFile)) {
    console.warn(`warning: no ${path.relative(root, stampFile)}: whether ${what} is current is unknown (${fix} records it)`);
    return;
  }
  throw new Error(`${stale.join(", ")} changed since ${what} was made: ${fix}`);
}

// p-code the generator has no translation for: 3 ops in data decoded as code, in fragments that
// never run. More means new code it can't translate.
const UNHANDLED_OPS = 3;
interface Summary { functions: number; fragments: number; instructions: number; files: number; unhandled: number;
  hooks: number; missingHooks: string[] }

const doRecomp = async (): Promise<void> => {
  const inputs = changed(recompStamp, recompInputs());
  if (!inputs.length && !values.force && existsSync(path.join(recompDir, "wc_dispatch.cpp"))) {
    console.log("the generated C++ is up to date (--force runs the generator anyway)");
    await doBuild();
    return;
  }
  const ghidra = findGhidra();
  requireStamp(decompileStamp, decompileInputs(), "the Ghidra project", "npm run decompile", { ghidra: ghidra.version });
  const summaryFile = path.join(recompDir, "summary.json");
  mkdirSync(recompDir, { recursive: true });
  rmSync(recompStamp, { force: true });
  rmSync(summaryFile, { force: true });
  await runHeadless(ghidra, [
    projectDir, projectName,
    "-process", path.basename(imageFile), "-noanalysis", "-readOnly",
    "-scriptPath", scriptPath,
    "-postScript", "WcRecomp.java", recompDir, entriesFile, codePointersFile, hooksFile,
  ]);
  // analyzeHeadless succeeds even when the script throws: the summary, written last, says it finished
  if (!existsSync(summaryFile)) throw new Error("the generator (WcRecomp.java) didn't finish: see Ghidra's output above");
  const s = JSON.parse(readFileSync(summaryFile, "utf8")) as Summary;
  if (s.missingHooks.length) {
    throw new Error(`hooks.txt: ${s.missingHooks.join(", ")} ${s.missingHooks.length > 1 ? "are" : "is"} not an instruction ` +
      "of the recompiled code, so the hook would never be called");
  }
  if (s.unhandled > UNHANDLED_OPS) {
    throw new Error(`the generator met ${s.unhandled} p-code ops it can't translate (${UNHANDLED_OPS} are known): ` +
      'see "unhandled" in out/recomp/seg_*.cpp');
  }
  if (s.unhandled < UNHANDLED_OPS) console.log(`note: only ${s.unhandled} unhandled p-code ops now (UNHANDLED_OPS in cli.ts says ${UNHANDLED_OPS})`);
  writeFileSync(recompStamp, JSON.stringify(stampOf(recompInputs()), null, 1) + "\n");
  await doBuild();
};

/** How far the files this project shares with wc4w-recomp (--other) have drifted apart: the lines
 * that differ, by file. Fixes go across by hand; wc4w-recomp's WC3W-DIFFERENCES.md says which and why. */
function doDrift(): void {
  const other = path.resolve(values.other);
  if (!existsSync(other)) throw new Error(`no ${other} (--other: the wc4w-recomp checkout)`);
  const list = (dir: string, ext: RegExp): string[] => existsSync(path.join(root, dir))
    ? readdirSync(path.join(root, dir)).filter((f) => ext.test(f)).map((f) => path.join(dir, f)) : [];
  const files = [...list("runtime", /\.(cpp|h)$/), ...list("ghidra_scripts", /\.java$/), ...list("src", /\.ts$/),
    ...list("test", /\.(ts|cpp)$/), "CMakeLists.txt", "tsconfig.json"];
  const rows: [string, string][] = [];
  for (const f of files) {
    if (!existsSync(path.join(other, f))) {
      rows.push(["only here", f]);
      continue;
    }
    const diff = spawnSync("diff", [path.join(other, f), path.join(root, f)], { encoding: "utf8" });
    if (diff.status !== 0 && diff.status !== 1) throw new Error(`diff failed on ${f}: ${diff.stderr}`);
    const lines = diff.stdout.split("\n").filter((l) => /^[<>]/.test(l)).length;
    rows.push([lines ? `${lines} lines` : "same", f]);
  }
  for (const dir of ["runtime", "ghidra_scripts", "src", "test"]) {
    if (!existsSync(path.join(other, dir))) continue;
    for (const f of readdirSync(path.join(other, dir))) {
      if (/\.(cpp|h|java|ts)$/.test(f) && !existsSync(path.join(root, dir, f))) rows.push(["only there", path.join(dir, f)]);
    }
  }
  const width = Math.max(...rows.map(([a]) => a.length));
  for (const [a, f] of rows) console.log(`${a.padStart(width)}  ${f}`);
  console.log(`\n${rows.filter(([a]) => a === "same").length} of ${rows.length} files are the same as in ${other}`);
}

/** The Windows functions the runtime implements (API(DLL, name, nargs) in runtime/*.cpp), as "DLL!name". */
function implemented(): Set<string> {
  const names = new Set<string>();
  const dir = path.join(root, "runtime");
  for (const f of readdirSync(dir).filter((x) => x.endsWith(".cpp"))) {
    for (const m of readFileSync(path.join(dir, f), "utf8").matchAll(/^API\((\w+), (\w+), \d+\)/gm)) names.add(`${m[1]}!${m[2]}`);
  }
  return names;
}
/** The imports without an implementation: a call of one stops the program (exit 6). */
function unimplemented(program: Program): string[] {
  const have = implemented();
  return program.thunks.map((t) => `${t.dll.replace(/\.DLL$/i, "").toUpperCase()}!${t.name}`).filter((n) => !have.has(n));
}

/** wc_program.cpp: the modules and the Windows functions they import, for the runtime. */
function writeProgram(): void {
  const p = JSON.parse(readFileSync(programFile, "utf8")) as Program;
  const missing = unimplemented(p);
  if (missing.length) console.log(`${missing.length} imported Windows functions aren't implemented (a call stops the program): ${missing.join(", ")}`);
  const h = (n: number): string => `0x${hex(n)}`;
  const text = [
    "/* Generated by cli.ts: the program's modules and its imports from Windows, from program.json. */",
    '#include "wc_rt.h"', "",
    `const wc_module wc_modules[] = {`,
    ...p.modules.map((m) => `\t{ "${m.file}", ${h(m.base)}, ${h(m.size)}, ${h(m.entry)}, ${m.isDll}, ${h(m.stackReserve)}, ` +
      `${h(m.resources)}, ${h(m.resourcesSize)} },`),
    "};",
    `const uint32_t wc_module_count = ${p.modules.length};`,
    `const char *const wc_exe = "${EXE}";`,
    `const uint32_t wc_default_cycles = ${CYCLES};`,
    `const uint32_t wc_import_count = ${p.thunks.length};`,
    "const wc_import wc_imports[] = {",
    ...p.thunks.map((t) => `\t{ "${t.dll}", "${t.name}" },`),
    "};", "",
  ].join("\n");
  const file = path.join(recompDir, "wc_program.cpp");
  if (!existsSync(file) || readFileSync(file, "utf8") !== text) writeFileSync(file, text);
}

/** Configure (once) and build with CMake. */
const doBuild = async (): Promise<void> => {
  requireStamp(recompStamp, recompInputs(), "the generated C++", "npm run recomp");
  writeProgram();
  const buildDir = path.join(out, "build");
  const binDir = path.join(out, "bin");
  if (!existsSync(path.join(buildDir, "CMakeCache.txt"))) {
    const ninja = spawnSync("ninja", ["--version"]).status === 0 ? ["-G", "Ninja"] : [];
    await exec("cmake", ["-S", root, "-B", buildDir, ...ninja, `-DWC_RECOMP_DIR=${recompDir}`, `-DWC_BIN_DIR=${binDir}`]);
  }
  await exec("cmake", ["--build", buildDir]);
  console.log(`built ${wcExe}`);
};

async function main(): Promise<void> {
  const command = positionals[0];
  switch (command) {
    case "unpack":
      doUnpack();
      break;
    case "imports":
      doImports();
      break;
    case "decompile": {
      doUnpack();
      const ghidra = findGhidra();
      mkdirSync(projectDir, { recursive: true });
      rmSync(decompileStamp, { force: true });
      await runHeadless(ghidra, [
        projectDir, projectName,
        "-import", imageFile, "-overwrite", "-loader", "BinaryLoader", "-loader-baseAddr", `0x${hex(IMAGE_LO)}`,
        "-processor", "x86:LE:32:default", "-cspec", "windows",
        "-scriptPath", scriptPath, "-preScript", "WcPeSetup.java", labelsFile,
        "-analysisTimeoutPerFile", "7200",
      ]);
      writeFileSync(decompileStamp, JSON.stringify(stampOf(decompileInputs(), { ghidra: ghidra.version }), null, 1) + "\n");
      break;
    }
    case "smoke":
      await doSmoke();
      break;
    case "drift":
      doDrift();
      break;
    case "recomp":
      await doRecomp();
      break;
    case "build":
      await doBuild();
      break;
    case "discover":
    case "play": {
      const play = command === "play";
      if (!(Number(values.seconds) > 0)) throw new Error(`--seconds ${values.seconds}: not a number of seconds`);
      for (let round = 1; round <= 50; round++) {
        const env = play ? process.env : { ...process.env, WC_HEADLESS: "1" };
        const res = await runGame([gameDir, ...positionals.slice(1)], env, play, play ? undefined : Number(values.seconds) * 1000,
          path.join(logsDir, `${command}-${round}.log`));
        const missing = /wc: missing code at ([0-9a-f]+)/.exec(res.stderr)?.[1];
        if (res.status !== 4 || !missing) {
          if (play) break;
          console.log(`round ${round}: ` + (res.timedOut ? `wc3w was still running after ${values.seconds} s, no missing code so far`
            : `wc3w stopped (exit ${res.status ?? res.signal}):`));
          console.log(res.stderr.split("\n").slice(-12).join("\n"));
          console.log(`(all of it in ${res.log})`);
          break;
        }
        const known = existsSync(entriesFile) ? readFileSync(entriesFile, "utf8").split(/\s+/) : [];
        if (known.includes(missing)) {
          console.log(`round ${round}: missing code at ${missing} again; the generator could not recompile it:`);
          console.log(res.stderr.split("\n").slice(-12).join("\n"));
          break;
        }
        console.log(`round ${round}: missing code at ${missing}, adding it as an entry point`);
        appendFileSync(entriesFile, `${missing}\n`);
        await doRecomp();
      }
      break;
    }
    default:
      console.error("usage: node src/cli.ts <unpack|imports|decompile|recomp|build|smoke|discover|play|drift> [--game dir] " +
        "[--out dir] [--seconds n] [--force] [--realtime] [--other dir]");
      process.exit(2);
  }
}

// The smoke test's runs, one after the other in the same folder. Times are the virtual clock's
// (WC_VIRTUAL_TIME: the same in every run), and near enough for --realtime too.
interface Scenario {
  name: string;
  what: string;
  keys: string;
  seconds: number;
  /** what the picture at the end must be: the display mode's depth (8 bits: the only one) and at
   * least so many colours (a blank or uniform screen has few) */
  bits: number;
  colours: number;
}
const SCENARIOS: Scenario[] = [
  // A new game (no saves): Esc through the intro's movies to the hangar, Flight Control (Esc past the
  // deck officer), Attend BRIEFING, Esc through it, a wingman, Fly MISSION, Esc past the launch, the
  // autopilot, then a target, guns and a missile (CLAUDE.md, "Playing it by script")
  { name: "flight", what: "the first mission in space", seconds: 95, bits: 8, colours: 100,
    keys: "3000:esc,6000:esc,9000:esc,10000:@320.444,10500:+lmb,10650:-lmb,12500:esc,14000:@30.200,14500:+lmb," +
      "14650:-lmb,20000:esc,26000:esc,32000:+lmb,32150:-lmb,36000:esc,42000:esc,48000:esc,52000:@612.240,52500:+lmb," +
      "52650:-lmb,58000:esc,68000:a,84000:t,86000:+space,89000:-space,90000:enter" },
];

/** The screenshot the runtime reports ("wc: screenshot saved to ... (640x480, 8-bit mode, n instructions)")
 * and how many colours it has. */
function readShot(stderr: string): { bits: number; movie: boolean; instructions: number; colours: number } | undefined {
  const m = /^wc: screenshot saved to (.+) \((\d+)x(\d+)(, an HD movie)?, (\d+)-bit mode, (\d+) instructions\)$/m.exec(stderr);
  if (!m || !existsSync(m[1])) return undefined;
  // the PNG: its IDAT chunks hold the rows (a filter byte 0 before each: the runtime's own writer),
  // RGB, or indices into PLTE (the game's 8-bit picture)
  const png = readFileSync(m[1]);
  const idat: Buffer[] = [];
  let palette: Buffer | undefined, indexed = false;
  for (let o = 8; o + 8 <= png.length; o += 12 + png.readUInt32BE(o)) {
    const type = png.toString("latin1", o + 4, o + 8), data = png.subarray(o + 8, o + 8 + png.readUInt32BE(o));
    if (type === "IHDR") indexed = data[9] === 3;
    if (type === "PLTE") palette = data;
    if (type === "IDAT") idat.push(data);
  }
  const rows = inflateSync(Buffer.concat(idat)), w = Number(m[2]), h = Number(m[3]), bytes = indexed ? 1 : 3;
  const colours = new Set<number>();
  for (let y = 0; y < h; y++) {
    for (let x = 0, p = y * (1 + bytes * w) + 1; x < w; x++, p += bytes) {
      colours.add(indexed ? (palette ? palette.readUIntBE(3 * rows[p], 3) : rows[p]) : rows.readUIntBE(p, 3));
    }
  }
  return { bits: Number(m[5]), movie: m[4] !== undefined, instructions: Number(m[6]), colours: colours.size };
}

/** Runs the scenarios headless in a fresh copy of the game folder: the archives and movie folders
 * linked, the other files copied but the saves (and their backups): the game starts with the intro. Fails on a stop, an error, a message from the runtime or a picture that isn't what the
 * scenario ends in. */
async function doSmoke(): Promise<void> {
  const smokeDir = path.join(out, "smoke"), game = path.join(smokeDir, "game");
  rmSync(smokeDir, { recursive: true, force: true });
  mkdirSync(game, { recursive: true });
  for (const e of readdirSync(gameDir, { withFileTypes: true })) {
    const from = path.join(gameDir, e.name), to = path.join(game, e.name);
    if (/^(SFOSAVED\.DAT|\d+\.WSG)$|backup$/i.test(e.name)) continue;
    if (e.isDirectory() || /\.tre$/i.test(e.name)) symlinkSync(from, to);
    else if (e.isFile()) copyFileSync(from, to);
  }
  const wanted = positionals.slice(1);
  for (const s of SCENARIOS.filter((x) => !wanted.length || wanted.includes(x.name))) {
    const shot = path.join(smokeDir, `${s.name}.png`);
    const env = { ...process.env, WC_HEADLESS: "1", WC_KEYS: s.keys, WC_SCREENSHOT: shot, WC_SCREENSHOT_AT: String(s.seconds),
      ...(values.realtime ? {} : { WC_VIRTUAL_TIME: "1" }) };
    console.log(`${s.name}: ${s.seconds} s headless${values.realtime ? "" : " (virtual time)"} in ${game}`);
    const started = Date.now();
    const res = await runGame([game], env, false, (s.seconds + 120) * 1000, path.join(logsDir, `smoke-${s.name}.log`));
    // the runtime's messages: all are problems but the screenshot's, a joystick's and its warnings
    const messages = res.stderr.split("\n").filter((l) => l.startsWith("wc: ") && !/^wc: (screenshot saved|joystick|game controller) /.test(l));
    const warnings = messages.filter((l) => /^wc: (divide overflow|read of|write of) /.test(l));
    const pic = readShot(res.stderr);
    const problems = [
      ...(res.timedOut ? ["still running long after the screenshot"] : res.status !== 0 ? [`exit ${res.status ?? res.signal}`] : []),
      ...(pic ? [] : ["no screenshot"]),
      ...(pic && pic.bits !== s.bits ? [`the display is in a ${pic.bits}-bit mode, not ${s.bits}`] : []),
      ...(pic && pic.movie ? ["an HD movie is playing"] : []),
      ...(pic && pic.colours < s.colours ? [`the picture has only ${pic.colours} colours`] : []),
      ...messages.filter((l) => !warnings.includes(l)),
    ];
    for (const w of warnings) console.log(`warning: ${w}`);
    if (problems.length) {
      console.log(res.stderr.split("\n").slice(-12).join("\n"));
      throw new Error(`smoke test failed in "${s.name}": ${problems.join("; ")} (all of it in ${res.log})`);
    }
    console.log(`${s.name} passed in ${((Date.now() - started) / 1000).toFixed(0)} s: ${shot} should show ${s.what} ` +
      `(${pic!.bits}-bit mode, ${pic!.colours} colours, ${pic!.instructions} instructions)`);
  }
}

// Runs the game. Its stderr goes on to ours as it comes (in play) and into the log; its end is
// kept, where the runtime reports missing code. Without a window its stdout is dropped.
function runGame(args: string[], env: NodeJS.ProcessEnv, play: boolean, timeoutMs: number | undefined, log: string):
  Promise<{ status: number | null; signal: NodeJS.Signals | null; stderr: string; timedOut: boolean; log: string }> {
  const KEEP = 4 << 20;
  mkdirSync(path.dirname(log), { recursive: true });
  const logStream = createWriteStream(log);
  return new Promise((resolve, reject) => {
    const child = spawn(wcExe, args, { env, stdio: ["inherit", play ? "inherit" : "ignore", "pipe"] });
    let stderr = "", timedOut = false;
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk: string) => {
      if (play) process.stderr.write(chunk);
      logStream.write(chunk);
      stderr += chunk;
      if (stderr.length > 2 * KEEP) stderr = stderr.slice(-KEEP);
    });
    const timer = timeoutMs === undefined ? undefined : setTimeout(() => {
      timedOut = true;
      child.kill();
    }, timeoutMs);
    child.on("error", reject);
    child.on("close", (status, signal) => {
      clearTimeout(timer);
      logStream.end();
      resolve({ status, signal, stderr, timedOut, log });
    });
  });
}

function exec(cmd: string, args: string[]): Promise<void> {
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, { stdio: ["ignore", "inherit", "inherit"] });
    child.on("error", reject);
    child.on("exit", (code) => (code === 0 ? resolve() : reject(new Error(`${cmd} ${args.join(" ")} failed (${code})`))));
  });
}

main().catch((err: unknown) => {
  console.error(err instanceof Error ? err.message : err);
  process.exit(1);
});
