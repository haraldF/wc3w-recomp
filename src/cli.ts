// Usage: node src/cli.ts <command> [--game dir] [--out dir] [--seconds n]
//   --game       the Windows version's folder (default ../game3.30/Kilrathi_Saga: the 30th Anniversary
//                Edition's Kilrathi_Saga, as GOG's installer has it or extracted with innoextract)
//   unpack       WC3W.EXE and XANLIB.DLL mapped as Windows loads them and linked into one image
//                (out/image.bin, from IMAGE_LO), its modules and imports in out/program.json
//   imports      unpack, then list what the modules import (the Windows API they need), by DLL
//   decompile    unpack, import the image into a Ghidra project (out/ghidra) and analyze it
//   recomp       translate the analyzed program to C++ (out/recomp/) and build out/bin/wc3w
//   build        only (re)build out/bin/wc3w from out/recomp/ and runtime/ (CMake, in out/build/)
//   discover     run out/bin/wc3w headless (each run for --seconds, default 60; WC_* from the
//                environment); when it reaches code that wasn't recompiled (reached only through
//                pointers in data), add that entry point to entries.txt, recompile, repeat
//   play         the same in the window: after each recompile the game starts again

import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
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
    seconds: { type: "string", default: "60" },
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
// Addresses where the runtime may take over (functions it replaces: runtime/hooks.cpp), kept in the repository
const hooksFile = path.join(root, "hooks.txt");
const projectDir = path.join(out, "ghidra");
const scriptPath = path.join(root, "ghidra_scripts");
const projectName = "WC3W";
const recompDir = path.join(out, "recomp");
const entriesFile = path.join(root, "entries.txt"); // kept in the repository
const wcExe = path.join(out, "bin", "wc3w");
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
  const { pes } = doUnpack();
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
  console.log("\nLoadLibrary/GetProcAddress add those found at run time");
}

const doRecomp = async (): Promise<void> => {
  mkdirSync(recompDir, { recursive: true });
  await runHeadless(findGhidra(), [
    projectDir, projectName,
    "-process", path.basename(imageFile), "-noanalysis", "-readOnly",
    "-scriptPath", scriptPath,
    "-postScript", "WcRecomp.java", recompDir, entriesFile, codePointersFile, hooksFile,
  ]);
  await doBuild();
};

/** wc_program.cpp: the modules and the Windows functions they import, for the runtime. */
function writeProgram(): void {
  const p = JSON.parse(readFileSync(programFile, "utf8")) as Program;
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
      mkdirSync(projectDir, { recursive: true });
      await runHeadless(findGhidra(), [
        projectDir, projectName,
        "-import", imageFile, "-overwrite", "-loader", "BinaryLoader", "-loader-baseAddr", `0x${hex(IMAGE_LO)}`,
        "-processor", "x86:LE:32:default", "-cspec", "windows",
        "-scriptPath", scriptPath, "-preScript", "WcPeSetup.java", labelsFile,
        "-analysisTimeoutPerFile", "7200",
      ]);
      break;
    }
    case "recomp":
      await doRecomp();
      break;
    case "build":
      await doBuild();
      break;
    case "discover":
    case "play": {
      const play = command === "play";
      for (let round = 1; round <= 50; round++) {
        const env = play ? process.env : { ...process.env, WC_HEADLESS: "1" };
        const res = await runGame([gameDir, ...positionals.slice(1)], env, play, play ? undefined : Number(values.seconds) * 1000);
        const missing = /wc: missing code at ([0-9a-f]+)/.exec(res.stderr)?.[1];
        if (res.status !== 4 || !missing) {
          if (play) break;
          console.log(`round ${round}: ` + (res.timedOut ? `wc3w was still running after ${values.seconds} s, no missing code so far`
            : `wc3w stopped (exit ${res.status ?? res.signal}):`));
          console.log(res.stderr.split("\n").slice(-12).join("\n"));
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
      console.error("usage: node src/cli.ts <unpack|imports|decompile|recomp|build|discover|play> [--game dir] [--out dir] [--seconds n]");
      process.exit(2);
  }
}

// Runs the game. Its stderr goes on to ours as it comes (in play) and its end is kept, where the
// runtime reports missing code. Without a window its stdout is dropped.
function runGame(args: string[], env: NodeJS.ProcessEnv, play: boolean, timeoutMs?: number):
  Promise<{ status: number | null; signal: NodeJS.Signals | null; stderr: string; timedOut: boolean }> {
  const KEEP = 4 << 20;
  return new Promise((resolve, reject) => {
    const child = spawn(wcExe, args, { env, stdio: ["inherit", play ? "inherit" : "ignore", "pipe"] });
    let stderr = "", timedOut = false;
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk: string) => {
      if (play) process.stderr.write(chunk);
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
      resolve({ status, signal, stderr, timedOut });
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
