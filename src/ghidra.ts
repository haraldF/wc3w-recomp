// Locates a Ghidra installation and runs its headless analyzer.

import { spawn } from "node:child_process";
import { existsSync, readFileSync, realpathSync } from "node:fs";
import path from "node:path";

export interface Ghidra {
  analyzeHeadless: string;
  javaHome?: string;
  /** application.version of its application.properties ("12.2"), "unknown" without one */
  version: string;
}

export function findGhidra(): Ghidra {
  const javaHome = process.env.JAVA_HOME;
  const installDir = process.env.GHIDRA_INSTALL_DIR;
  if (installDir) return { analyzeHeadless: headlessIn(installDir), javaHome, version: versionIn(installDir) };

  // Homebrew installs a wrapper script that pins JAVA_HOME and execs the real ghidraRun.
  for (const dir of (process.env.PATH ?? "").split(path.delimiter)) {
    const wrapper = path.join(dir, "ghidraRun");
    if (!existsSync(wrapper)) continue;
    const real = realpathSync(wrapper);
    const text = readFileSync(real, "utf8");
    const target = /exec\s+"([^"]+ghidraRun)"/.exec(text)?.[1] ?? real;
    const pinnedJava = /JAVA_HOME="\$\{JAVA_HOME:-([^}]+)\}"/.exec(text)?.[1];
    const installed = path.dirname(target);
    return { analyzeHeadless: headlessIn(installed), javaHome: javaHome ?? pinnedJava, version: versionIn(installed) };
  }
  throw new Error("Ghidra not found: set GHIDRA_INSTALL_DIR or put ghidraRun on PATH");
}

function versionIn(installDir: string): string {
  const file = path.join(installDir, "Ghidra", "application.properties");
  const version = existsSync(file) ? /^application\.version=(.+)$/m.exec(readFileSync(file, "utf8"))?.[1].trim() : undefined;
  return version ?? "unknown";
}

function headlessIn(installDir: string): string {
  const p = path.join(installDir, "support", "analyzeHeadless");
  if (!existsSync(p)) throw new Error(`no analyzeHeadless in ${installDir}`);
  return p;
}

export function runHeadless(ghidra: Ghidra, args: string[]): Promise<void> {
  const env = { ...process.env, ...(ghidra.javaHome ? { JAVA_HOME: ghidra.javaHome } : {}) };
  console.log(`$ analyzeHeadless ${args.map((a) => (/\s/.test(a) ? JSON.stringify(a) : a)).join(" ")}`);
  return new Promise((resolve, reject) => {
    const child = spawn(ghidra.analyzeHeadless, args, { env, stdio: "inherit" });
    child.on("error", reject);
    child.on("exit", (code) => (code === 0 ? resolve() : reject(new Error(`analyzeHeadless exited with ${code}`))));
  });
}
