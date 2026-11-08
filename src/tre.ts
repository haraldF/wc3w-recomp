// Usage: node src/tre.ts file.TRE [outdir]
// The game's data archives (XTRE): lists the entries, or with outdir writes each one unpacked
// (outdir/<n>.iff, n the entry's index; .bin when it isn't an IFF). Also a module (readTre) for
// the other tools.
//
// Layout (all little-endian dwords): "XTRE", 0, then the offsets of the hash table (pairs: treHash of
// the path, the entry's place in the entries table), of the names table (only a few entries keep their path there, after a
// length byte, with the entry's index), of the entries and of the data. An entry is its offset in
// the file and its size; bit 31 of the size: packed (its length then runs to the next entry), bit 30
// picks the method, the low 30 bits are the unpacked size (TRE reader at 484D00h):
//   bit 30 clear: LZW (486549h): LSB-first codes of 9 to 12 bits, 100h starts over, 101h ends.
//   bit 30 set: RefPack (4862F0h), byte-oriented copies with literal runs.

import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";

export interface TreEntry {
  index: number;
  name?: string;
  data: Buffer;
}

function lzw(src: Buffer, size: number): Buffer {
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

function refpack(src: Buffer, size: number): Buffer {
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

/* The hash of an entry's path (485080h): rotated left by 3 and the next character added, over the
 * whole path as the game asks for it, e.g. ..\..\DATA\MISSIONS\MISNL004.IFF */
export function treHash(name: string): number {
  let h = 0;
  for (const c of Buffer.from(name, "latin1")) h = (((h << 3) | (h >>> 29)) + c) >>> 0;
  return h;
}

/* Entries named by guessed paths (those whose hash is in the archive) */
export function nameEntries(file: string, entries: TreEntry[], paths: Iterable<string>) {
  const d = readFileSync(file);
  const hashAt = d.readUInt32LE(8), namesAt = d.readUInt32LE(12), entriesAt = d.readUInt32LE(16);
  const byHash = new Map<number, number>();
  for (let o = hashAt; o + 8 <= namesAt; o += 8) byHash.set(d.readUInt32LE(o), (d.readUInt32LE(o + 4) - entriesAt) / 8);
  for (const p of paths) {
    const i = byHash.get(treHash(p));
    if (i !== undefined && entries[i] && !entries[i].name) entries[i].name = p;
  }
}

export function readTre(file: string): TreEntry[] {
  const d = readFileSync(file);
  if (d.toString("latin1", 0, 4) !== "XTRE") throw new Error(`${file}: not an XTRE archive`);
  const namesAt = d.readUInt32LE(12), entriesAt = d.readUInt32LE(16), dataAt = d.readUInt32LE(20);
  const names = new Map<number, string>();
  for (let o = namesAt; o < entriesAt; ) {
    const len = d[o], end = o + 1 + len;
    const text = d.toString("latin1", o + 1, end);
    if (len === 0 || end + 4 > entriesAt || !/^[ -~]+$/.test(text)) {
      o++;
      continue;
    }
    const at = d.readUInt32LE(end); /* the entry's place in the entries table */
    if (at >= entriesAt && at < dataAt && (at - entriesAt) % 8 === 0) names.set((at - entriesAt) / 8, text);
    o = end + 4;
  }
  const raw: { offset: number; size: number }[] = [];
  for (let o = entriesAt; o + 8 <= dataAt; o += 8) raw.push({ offset: d.readUInt32LE(o), size: d.readUInt32LE(o + 4) });
  const ends = [...raw.map((e) => e.offset), d.length].sort((a, b) => a - b);
  return raw.map(({ offset, size }, index) => {
    let data: Buffer;
    if (size & 0x80000000) {
      const end = ends.find((x) => x > offset) ?? d.length;
      data = (size & 0x40000000 ? refpack : lzw)(d.subarray(offset, end), size & 0x3fffffff);
    } else data = d.subarray(offset, offset + size);
    return { index, name: names.get(index), data };
  });
}

if (import.meta.main) {
  const [file, outdir] = process.argv.slice(2);
  if (!file) {
    console.error("usage: node src/tre.ts file.TRE [outdir]");
    process.exit(1);
  }
  if (outdir) mkdirSync(outdir, { recursive: true });
  for (const e of readTre(file)) {
    const form = e.data.toString("latin1", 0, 4) === "FORM" ? e.data.toString("latin1", 8, 12) : "";
    console.log(`${String(e.index).padStart(4)}  ${String(e.data.length).padStart(9)}  ${form.padEnd(4)}  ${e.name ?? ""}`);
    if (outdir) writeFileSync(path.join(outdir, `${e.index}.${form ? "iff" : "bin"}`), e.data);
  }
}
