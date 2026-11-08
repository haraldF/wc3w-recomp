// Usage: node src/print-mission.ts [--game dir] [mission ...]
// The flight missions (MISSIONS.TRE in the game folder): without arguments a list of them, else each
// mission given (by name, e.g. MISNL004, by its index in the archive, or an extracted .iff file)
// with its messages, cast, units and script.
//
// A mission is an IFF FORM MISN. MSGS: its strings (objectives, and the names of the movie and the
// next mission file the script picks, by index). CAST: the ship types. FLAG: the initial values of
// the script's variables (dwords). PART: the units, 93 bytes each: index, ship (8 bytes), ..., and at
// +77 four handlers (the script's handler numbers, -1 none) the game runs on the unit's events:
// 2 is its destruction (kills are counted there), 3 its start; 1 runs as it flies (the player's
// checks the objectives), 0 we don't know.
//
// PROG is the script: dword pairs, FEED00xxh (the opcode) and an operand. Handler n starts after the
// n-th opcode 00 (the end of a handler). Each handler runs on an accumulator, a result register and
// compare flags; "var[n]" is the mission's variables (from FLAG), "reg[n]" the gameflow's registers (REGS
// in a save, 4AB840h), which the gameflow scenes branch on: reg 12 is the mission's outcome. The
// interpreter is at 44C780h, its opcodes in a table at 44D7ECh (index 44D9A8h); the ones named
// below are read from its handlers (compare: the first operand against the second), the others are
// shown by number.

import { readFileSync } from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";
import { nameEntries, readTre, type TreEntry } from "./tre.ts";

const root = path.resolve(import.meta.dirname, "..");
const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: { game: { type: "string", default: path.resolve(root, "../game3.30/Kilrathi_Saga") } },
});

// what an opcode does, % its operand (L%: a label, msg %: a message's index)
const ops: Record<number, string> = {
  0x00: "end", 0x01: "return", 0x02: "call handler %", 0x04: "stop", 0x08: "L%:", 0x0f: "flight state = %",
  0x10: "acc = %", 0x11: "acc = var[%]", 0x12: "swap acc, var[%]", 0x13: "acc = result", 0x15: "acc = random(%)",
  0x20: "acc += %",
  0x40: "compare acc, %", 0x41: "compare %, acc", 0x42: "compare acc, var[%]", 0x43: "compare var[%], acc",
  0x44: "compare result, %", 0x45: "compare var[%], 1",
  0x46: "if result goto L%", 0x47: "if !result goto L%", 0x48: "if = goto L%", 0x49: "if != goto L%",
  0x4a: "if < goto L%", 0x4b: "if > goto L%", 0x4c: "if <= goto L%", 0x4d: "if >= goto L%", 0x4e: "goto L%",
  0x50: "var[%] = acc", 0x51: "var[%] = result", 0x52: "var[%] = 1", 0x53: "var[%] = 0", 0x54: "var[%] ^= 1",
  0x55: "var[%]++",
  0x80: "area % on", 0x81: "area % off", 0x90: "launch unit %",
  0xc0: "game state = %", 0xc1: "next scene = %", 0xc2: "4A3588h = %", 0xc3: "acc = reg[%]", 0xc4: "reg[%] = acc",
  0xc5: "movie = msg %", 0xc6: "next mission = msg %",
};

function chunks(d: Buffer): Map<string, Buffer> {
  const map = new Map<string, Buffer>();
  const walk = (o: number, end: number, prefix: string) => {
    while (o + 8 <= end) {
      const tag = d.toString("latin1", o, o + 4), size = d.readUInt32BE(o + 4);
      if (tag === "FORM" || tag === "CAT " || tag === "LIST") walk(o + 12, o + 8 + size, `${prefix}${d.toString("latin1", o + 8, o + 12)}/`);
      else if (!map.has(prefix + tag)) map.set(prefix + tag, d.subarray(o + 8, o + 8 + size));
      o += 8 + size + (size & 1);
    }
  };
  if (d.toString("latin1", 0, 4) !== "FORM" || d.toString("latin1", 8, 12) !== "MISN") throw new Error("not a mission (FORM MISN)");
  walk(12, Math.min(d.length, 8 + d.readUInt32BE(4)), "");
  return map;
}

const strings = (b: Buffer | undefined) => (b ? b.toString("latin1").split("\0").slice(0, -1) : []);

function printMission(title: string, data: Buffer) {
  const c = chunks(data);
  const msgs = strings(c.get("MSGS"));
  console.log(`== ${title}  ${strings(c.get("NAME"))[0] ?? ""}`);
  console.log("messages:");
  msgs.forEach((m, i) => console.log(`  ${i}: ${m}`));
  const flag = c.get("FLAG");
  if (flag) console.log(`variables: ${Array.from({ length: flag.length >> 2 }, (_, i) => flag.readInt32LE(i * 4)).join(" ")}`);
  const part = c.get("PART");
  if (part) {
    console.log("units (handlers for events 0, 1, 2 destroyed, 3 start):");
    for (let o = 0; o + 93 <= part.length; o += 93) {
      const h = [0, 1, 2, 3].map((k) => part.readInt32LE(o + 77 + k * 4));
      const name = part.toString("latin1", o + 4, o + 12).replace(/\0.*$/s, "");
      console.log(`  ${String(part.readUInt32LE(o)).padStart(2)} ${name.padEnd(9)} ${h.map((x) => (x < 0 ? "-" : x)).join(" ")}`);
    }
  }
  const prog = c.get("PROG");
  if (!prog) return;
  console.log("script:");
  let handler = 0, start = true;
  for (let o = 0; o + 8 <= prog.length; o += 8) {
    const word = prog.readUInt32LE(o), arg = prog.readInt32LE(o + 4);
    if (word >>> 16 !== 0xfeed) {
      console.log(`  ${String(o >> 2).padStart(4)}  .word ${word.toString(16)}`);
      o -= 4;
      continue;
    }
    const op = word & 0xffff;
    if (start) console.log(` handler ${handler}:`);
    start = false;
    const form = ops[op] ?? `op${op.toString(16).padStart(2, "0")} %`;
    let text = form.replace("%", String(arg));
    if (form.includes("msg") && msgs[arg] !== undefined) text += `  (${msgs[arg]})`;
    console.log(`  ${String(o >> 2).padStart(4)}  ${op === 0x08 ? "" : "  "}${text}`);
    if (op === 0x00) {
      handler++;
      start = true;
    }
  }
}

const tre = path.join(values.game, "MISSIONS.TRE");
let entries: TreEntry[] | undefined;
function missions(): TreEntry[] {
  if (entries) return entries;
  entries = readTre(tre);
  // name them: the archive keeps only hashes of the paths; guesses from words in the missions and
  // the gameflow's scenes (a mission names the next one's file)
  const words = new Set<string>();
  const gameflow = path.join(values.game, "GAMEFLOW.TRE");
  for (const e of [...entries, ...readTre(gameflow)])
    for (const m of e.data.toString("latin1").matchAll(/[A-Za-z][A-Za-z0-9_]{2,7}/g)) words.add(m[0].toUpperCase());
  nameEntries(tre, entries, [...words].map((w) => `..\\..\\DATA\\MISSIONS\\${w}.IFF`));
  return entries;
}
const shortName = (e: TreEntry) => (e.name ?? "").replace(/^.*\\/, "").replace(/\.IFF$/i, "");
const isMission = (e: TreEntry) => e.data.toString("latin1", 0, 4) === "FORM" && e.data.toString("latin1", 8, 12) === "MISN";

if (positionals.length === 0) {
  for (const e of missions().filter(isMission)) {
    const c = chunks(e.data);
    const msgs = strings(c.get("MSGS"));
    console.log(`${String(e.index).padStart(4)}  ${shortName(e).padEnd(9)} ${(strings(c.get("NAME"))[0] ?? "").padEnd(9)} ${msgs.slice(1, 4).join(" / ")}`);
  }
}
for (const arg of positionals) {
  try {
    if (/\.iff$/i.test(arg)) {
      printMission(path.basename(arg), readFileSync(arg));
      continue;
    }
    const e = /^\d+$/.test(arg) ? missions()[Number(arg)] : missions().find((x) => shortName(x).toUpperCase() === arg.toUpperCase());
    if (!e) throw new Error(`no mission ${arg} in ${tre}`);
    printMission(`${e.index} ${shortName(e)}`, e.data);
  } catch (err) {
    console.log(`${arg}: ${(err as Error).message}`);
  }
}
