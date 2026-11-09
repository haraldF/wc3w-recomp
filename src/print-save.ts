// Usage: node src/print-save.ts [--game dir] [file.WSG ...]
// Shows the player's kills and aces, the ship's and the wingmen's morale and the wingmen's kills from
// saved games (the game folder's *.WSG, newest last, or the files given). A save is an IFF FORM
// SAVE; the game reads it as follows (WC3W.EXE):
//   PILT  12 dwords, a pilot's morale + 1 (4A9B38h): 0..15, set by the gameflow (407890h, clamped).
//         0 means the pilot is out of the game (dead or gone; the killboard says DECEASED).
//         Negative: the pilot doesn't eject when shot down (the gameflow sets and clears the sign,
//         407920h/407960h); a positive one ejects (ejTpilot, 46A794h). In flight a wingman with
//         morale 5 or more gets +10 on a combat value (452A34h); after a mission, wingmen's kills
//         are credited with fewer below 5 and more above 10 (407A40h).
//         Slot 0 is the ship's morale: conversations ("ship morale" in the guides, e.g. Rollins's)
//         change it with the others (gameflow opcode 3Ah, 4077E0h), and a few bridge conversations
//         pick their version by it (opcode 3Dh: morale(0) against 5). Flight doesn't read it.
//   KILL  12 dwords, kills (4A9AA0h; -1 for slots not on the killboard)
//   ACE_  12 dwords, aces shot down, as the killboard shows them (4A9AE8h, added to with KILL at 4079F0h)
//   PLYR  the player's kills (4A9B34h) and aces (4A9A98h; added to by the gameflow, 407C20h, and
//         both on the killboard, with their sum as TOTAL KILLS), then the callsign (15 bytes)
//   STAT  three dwords: the game's state (4A3580h), the scene (4A3584h: the mission counter, the
//         gameflow's SCENxxxx.IFF) and the previous one
//   INFO  the save's version (6), the CD it was made on
// The killboard's names by slot (415C3Eh); slots 3, 5, 10 and 11 have morale but no name.

import { readdirSync, readFileSync, statSync } from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";

const root = path.resolve(import.meta.dirname, "..");
const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: { game: { type: "string", default: path.resolve(root, "../game3.30/Kilrathi_Saga") } },
});

const pilots: [number, string][] = [
  [9, "Flint"], [2, "Vagabond"], [8, "Maniac"], [4, "Flash"], [1, "Cobra"], [6, "Hobbes"], [7, "Vaquero"],
];
const unnamed = [3, 5, 10, 11];

function chunks(data: Buffer): Map<string, Buffer> {
  if (data.toString("latin1", 0, 4) !== "FORM" || data.toString("latin1", 8, 12) !== "SAVE")
    throw new Error("not a saved game (FORM SAVE)");
  const map = new Map<string, Buffer>();
  const end = Math.min(data.length, 8 + data.readUInt32BE(4));
  for (let o = 12; o + 8 <= end; ) {
    const size = data.readUInt32BE(o + 4);
    map.set(data.toString("latin1", o, o + 4), data.subarray(o + 8, o + 8 + size));
    o += 8 + size + (size & 1);
  }
  return map;
}

function dwords(c: Map<string, Buffer>, tag: string): number[] {
  const b = c.get(tag);
  if (!b || b.length < 48) throw new Error(`no ${tag} chunk`);
  return Array.from({ length: 12 }, (_, i) => b.readInt32LE(i * 4));
}

function morale(v: number): string {
  if (v === 0) return "dead or gone";
  return `${String(Math.abs(v) - 1).padStart(2)}/15${v < 0 ? ", won't eject" : ""}`;
}

let files = positionals;
if (files.length === 0) {
  files = readdirSync(values.game)
    .filter((f) => /\.wsg$/i.test(f) && !/^wsg_ndx/i.test(f))
    .map((f) => path.join(values.game, f))
    .sort((a, b) => statSync(a).mtimeMs - statSync(b).mtimeMs);
  if (files.length === 0) throw new Error(`no saved games (*.WSG) in ${values.game} (--game)`);
}

for (const file of files) {
  try {
    const c = chunks(readFileSync(file));
    const pilt = dwords(c, "PILT"), kill = dwords(c, "KILL"), ace = dwords(c, "ACE_");
    const plyr = c.get("PLYR"), stat = c.get("STAT"), info = c.get("INFO");
    const callsign = plyr && plyr.length > 8 ? plyr.toString("latin1", 8).replace(/\0.*$/s, "") : "?";
    const when = statSync(file).mtime.toLocaleString();
    const where = [stat && stat.length >= 8 ? `scene ${stat.readInt32LE(4)}` : "", info && info.length >= 8 ? `CD ${info.readInt32LE(4)}` : ""];
    console.log(`${path.basename(file)}  ${callsign}  ${where.filter(Boolean).join(", ")}  (${when})`);
    // name, morale, kills and aces in columns
    const row = (name: string, m: string, kills?: number, aces?: number) =>
      console.log((`  ${name.padEnd(9)} ${m ? `morale ${m}` : ""}`.padEnd(40) +
        (kills === undefined ? "" : `kills ${String(kills).padStart(4)}  aces ${aces}`)).trimEnd());
    if (plyr && plyr.length >= 8) row(callsign, "", plyr.readInt32LE(0), plyr.readInt32LE(4));
    row("ship", morale(pilt[0]));
    for (const [i, name] of pilots) row(name, morale(pilt[i]), kill[i], ace[i]);
    console.log(`  other slots: ${unnamed.map((i) => `${i}: ${morale(pilt[i]).trim()}`).join("; ")}`);
  } catch (e) {
    console.log(`${path.basename(file)}: ${(e as Error).message}`);
  }
}
