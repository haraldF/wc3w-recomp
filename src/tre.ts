// Usage: node src/tre.ts file.TRE [outdir] [--names file]
// The game's data archives (XTRE): lists the entries (index, size, IFF form, hash, path), or with
// outdir writes each one unpacked (outdir/<n>.iff, n the entry's index; .bin when it isn't an IFF).
// --names: a file of paths as the game asks for them (one a line); those whose hash is in the
// archive name their entries. Also a module (readTre) for the other tools.
//
// Layout (all little-endian dwords; Wing Commander IV's WC4W.EXE reads both versions, its reader at
// 49B380h, lookup 49B890h; WC3W.EXE's hash at 485080h): "XTRE", the
// version (0: Wing Commander III, 1: IV), then the offsets of the hash table, of the names table,
// of the entries and of the data. The hash table has slots of the path's hash (treHash, by version)
// and a file offset: of the entry (in the entries table), or, where paths collide, of a record in
// the names table (a length byte, the path, the entry's offset; the game compares paths from there
// on). An empty slot has offset 0 or FFFFFFFFh. In version 1 the slots (2^n - 1) are a binary tree
// in an array: from slot k, larger hashes go on at 2k, smaller at 2k + 1.
// An entry is its offset in the file and its size; bit 31 of the size: packed (its length then runs
// to the next entry), bit 30 picks the method, the low 30 bits are the unpacked size:
//   bit 30 clear: LZW (49D569h): LSB-first codes of 9 to 12 bits, 100h starts over, 101h ends.
//   bit 30 set: RefPack (49D310h), byte-oriented copies with literal runs.

import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";

export interface TreEntry {
  index: number;
  name?: string;
  /** the hash of its path, from the hash table (a slot that names the entry directly) */
  hash?: number;
  data: Buffer;
}

export function lzw(src: Buffer, size: number): Buffer {
  const out = Buffer.alloc(size);
  let n = 0, pos = 0, bit = 0, width = 9, next = 0x102;
  const table: Buffer[] = [];
  let prev: Buffer | undefined;
  while (n < size) {
    const word = (src[pos] ?? 0) | (src[pos + 1] ?? 0) << 8 | (src[pos + 2] ?? 0) << 16;
    const code = (word >>> bit) & ((1 << width) - 1);
    bit += width;
    pos += bit >> 3;
    bit &= 7;
    if (code === 0x101) break;
    if (code === 0x100) {
      width = 9;
      next = 0x102;
      prev = undefined;
      continue;
    }
    let s: Buffer;
    if (code < 0x100) s = Buffer.from([code]);
    else if (code < next && table[code]) s = table[code];
    else if (code === next && prev) s = Buffer.concat([prev, prev.subarray(0, 1)]);
    else throw new Error(`LZW: bad code ${code.toString(16)} at ${n}`);
    s.copy(out, n);
    n += s.length;
    if (prev && next < 4096) table[next++] = Buffer.concat([prev, s.subarray(0, 1)]);
    if (next >= 1 << width && width < 12) width++;
    prev = s;
  }
  return out.subarray(0, Math.min(n, size));
}

export function refpack(src: Buffer, size: number): Buffer {
  const out = Buffer.alloc(size);
  let n = 0, i = 0;
  const literal = (count: number) => {
    src.copy(out, n, i, i + count);
    n += count;
    i += count;
  };
  while (i < src.length && n < size) {
    const b0 = src[i];
    let offset: number, count: number;
    if (!(b0 & 0x80)) {
      const b1 = src[i + 1];
      i += 2;
      literal(b0 & 3);
      offset = ((b0 & 0x60) << 3) + b1 + 1;
      count = ((b0 & 0x1c) >> 2) + 3;
    } else if (!(b0 & 0x40)) {
      const b1 = src[i + 1], b2 = src[i + 2];
      i += 3;
      literal(b1 >> 6);
      offset = ((b1 & 0x3f) << 8) + b2 + 1;
      count = (b0 & 0x3f) + 4;
    } else if (!(b0 & 0x20)) {
      const b1 = src[i + 1], b2 = src[i + 2], b3 = src[i + 3];
      i += 4;
      literal(b0 & 3);
      offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
      count = ((b0 & 0x0c) << 6) + b3 + 5;
    } else {
      i++;
      const run = ((b0 & 0x1f) << 2) + 4;
      if (run <= 0x70) {
        literal(run);
        continue;
      }
      literal(b0 & 3);
      break;
    }
    for (; count > 0 && n < size; count--, n++) out[n] = out[n - offset];
  }
  return out.subarray(0, n);
}

/* The hash of an entry's path (WC4W.EXE's 49B980h, WC3W.EXE's 485080h: version 0), over the whole path as the game asks for it, e.g.
 * ..\..\DATA\MISSIONS\MISNL004.IFF. Version 0: rotated left by 3 and the next character added.
 * Version 1: the first character times 256, then for each further one (i from 0) h += (h >> 4) * c + i. */
export function treHash(name: string, version = 1): number {
  const b = Buffer.from(name, "latin1");
  if (version < 1) {
    let h = 0;
    for (const c of b) h = (((h << 3) | (h >>> 29)) + c) >>> 0;
    return h;
  }
  if (!b.length) return 0;
  let h = b[0] << 8;
  for (let i = 1; i < b.length; i++) h = (h + Math.imul(h >>> 4, b[i]) + i - 1) >>> 0;
  return h;
}

interface Directory {
  version: number;
  hashAt: number;
  namesAt: number;
  entriesAt: number;
  dataAt: number;
  /** the slots in use: hash, file offset (of an entry or a names record) */
  slots: { hash: number; at: number }[];
  /** the names table's records: path, the entry's index */
  names: { name: string; index: number }[];
}

function directory(file: string, d: Buffer): Directory {
  if (d.toString("latin1", 0, 4) !== "XTRE") throw new Error(`${file}: not an XTRE archive`);
  const version = d.readUInt32LE(4);
  if (version > 1) throw new Error(`${file}: XTRE version ${version}, only 0 and 1 are known`);
  const hashAt = d.readUInt32LE(8), namesAt = d.readUInt32LE(12), entriesAt = d.readUInt32LE(16), dataAt = d.readUInt32LE(20);
  if (!(24 <= hashAt && hashAt <= namesAt && namesAt <= entriesAt && entriesAt <= dataAt && dataAt <= d.length)) {
    throw new Error(`${file}: bad XTRE header`);
  }
  const slots: Directory["slots"] = [];
  for (let o = hashAt; o + 8 <= namesAt; o += 8) {
    const at = d.readUInt32LE(o + 4);
    if (at !== 0 && at !== 0xffffffff) slots.push({ hash: d.readUInt32LE(o), at });
  }
  const names: Directory["names"] = [];
  for (let o = namesAt; o < entriesAt; ) {
    const len = d[o], end = o + 1 + len;
    const text = d.toString("latin1", o + 1, end);
    if (len === 0 || end + 4 > entriesAt || !/^[ -~]+$/.test(text)) {
      o++;
      continue;
    }
    const at = d.readUInt32LE(end);
    if (at >= entriesAt && at < dataAt && (at - entriesAt) % 8 === 0) names.push({ name: text, index: (at - entriesAt) / 8 });
    o = end + 4;
  }
  return { version, hashAt, namesAt, entriesAt, dataAt, slots, names };
}

/* Entries named by guessed paths: those whose hash is in the archive (directly, or a collision
 * whose names records have the path). Returns how many it named. */
export function nameEntries(file: string, entries: TreEntry[], paths: Iterable<string>): number {
  const dir = directory(file, readFileSync(file));
  const byHash = new Map<number, number>();
  for (const s of dir.slots) if (s.at >= dir.entriesAt) byHash.set(s.hash, (s.at - dir.entriesAt) / 8);
  let named = 0;
  for (const p of paths) {
    const i = byHash.get(treHash(p, dir.version));
    if (i !== undefined && entries[i] && !entries[i].name) {
      entries[i].name = p;
      named++;
    }
  }
  return named;
}

export function readTre(file: string): TreEntry[] {
  const d = readFileSync(file);
  const dir = directory(file, d);
  const { entriesAt, dataAt } = dir;
  // Every slot must lead to an entry: directly, or through the names table, where the path's hash
  // must be the slot's.
  const hashOf = new Map<number, number>();
  const collisions = new Set<number>();
  for (const s of dir.slots) {
    if (s.at >= entriesAt && s.at < dataAt && (s.at - entriesAt) % 8 === 0) hashOf.set((s.at - entriesAt) / 8, s.hash);
    else if (s.at >= dir.namesAt && s.at < entriesAt) collisions.add(s.hash);
    else throw new Error(`${file}: a hash slot (${s.hash.toString(16)}) points to ${s.at.toString(16)}, no entry`);
  }
  for (const n of dir.names) {
    const h = treHash(n.name, dir.version);
    if (!collisions.has(h)) throw new Error(`${file}: ${n.name}'s hash ${h.toString(16)} has no slot (wrong treHash?)`);
  }
  const names = new Map(dir.names.map((n) => [n.index, n.name]));
  const raw: { offset: number; size: number }[] = [];
  for (let o = entriesAt; o + 8 <= dataAt; o += 8) raw.push({ offset: d.readUInt32LE(o), size: d.readUInt32LE(o + 4) });
  const ends = [...raw.map((e) => e.offset), d.length].sort((a, b) => a - b);
  return raw.map(({ offset, size }, index) => {
    let data: Buffer;
    if (size & 0x80000000) {
      const end = ends.find((x) => x > offset) ?? d.length;
      data = (size & 0x40000000 ? refpack : lzw)(d.subarray(offset, end), size & 0x3fffffff);
    } else data = d.subarray(offset, offset + size);
    const name = names.get(index);
    return { index, name, hash: hashOf.get(index) ?? (name === undefined ? undefined : treHash(name, dir.version)), data };
  });
}

if (import.meta.main) {
  const { positionals, values } = parseArgs({ allowPositionals: true, options: { names: { type: "string" } } });
  const [file, outdir] = positionals;
  if (!file) {
    console.error("usage: node src/tre.ts file.TRE [outdir] [--names file]");
    process.exit(1);
  }
  if (outdir) mkdirSync(outdir, { recursive: true });
  const entries = readTre(file);
  if (values.names) {
    const paths = readFileSync(values.names, "latin1").split(/\r?\n/).filter((x) => x);
    console.error(`${nameEntries(file, entries, paths)} of ${paths.length} paths name entries`);
  }
  for (const e of entries) {
    const form = e.data.toString("latin1", 0, 4) === "FORM" ? e.data.toString("latin1", 8, 12) : "";
    const hash = e.hash === undefined ? "" : e.hash.toString(16).padStart(8, "0");
    console.log(`${String(e.index).padStart(4)}  ${String(e.data.length).padStart(9)}  ${form.padEnd(4)}  ${hash.padEnd(8)}  ${e.name ?? ""}`);
    if (outdir) writeFileSync(path.join(outdir, `${e.index}.${form ? "iff" : "bin"}`), e.data);
  }
}
