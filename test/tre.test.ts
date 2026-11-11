// The archive reader (src/tre.ts) on archives made here: no game files needed.

import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { test } from "node:test";
import { lzw, nameEntries, readTre, refpack, treHash } from "../src/tre.ts";

/** LZW as the game's decoder (49D569h) takes it: LSB-first codes of 9 to 12 bits, 100h starts over,
 * 101h ends. The decoder makes a table entry one code after this side does and widens its codes by
 * its own count (k codes since the start: 102h + k - 1 entries); a full table starts over. */
function lzwEncode(data: Buffer): Buffer {
  const out: number[] = [];
  let acc = 0, nbits = 0, width = 9, k = 0;
  let dict = new Map<string, number>();
  const put = (code: number): void => {
    acc |= code << nbits;
    nbits += width;
    for (; nbits >= 8; nbits -= 8, acc >>>= 8) out.push(acc & 0xff);
    k++;
    if (Math.min(0x102 + k - 1, 4096) >= 1 << width && width < 12) width++;
  };
  const code = (w: string): number => (w.length === 1 ? w.charCodeAt(0) : dict.get(w)!);
  let w = "";
  for (const byte of data) {
    const c = String.fromCharCode(byte);
    if (w === "" || dict.has(w + c)) {
      w += c;
      continue;
    }
    const index = 0x102 + k; // the entry this code makes
    put(code(w));
    if (index < 4096) dict.set(w + c, index);
    else {
      put(0x100);
      dict = new Map();
      k = 0;
      width = 9;
    }
    w = c;
  }
  if (w !== "") put(code(w));
  put(0x101);
  if (nbits) out.push(acc & 0xff);
  return Buffer.from(out);
}

/** Bytes that compress: runs and repeats over a small alphabet, from a fixed generator. */
function sample(size: number): Buffer {
  const b = Buffer.alloc(size);
  let x = 12345;
  for (let i = 0; i < size; i++) {
    x = (Math.imul(x, 1103515245) + 12345) >>> 0;
    b[i] = i % 97 < 20 ? 0x41 : 0x41 + ((x >>> 16) % 6);
  }
  return b;
}

test("treHash", () => {
  assert.equal(treHash("A", 0), 0x41);
  assert.equal(treHash("AB", 0), 0x24a); // rotated left by 3, the next character added
  assert.equal(treHash("", 1), 0);
  assert.equal(treHash("A", 1), 0x4100);
  assert.equal(treHash("AB", 1), 0x14d20); // h += (h >> 4) * c + i
  // the two paths of Language.tre that collide (CLAUDE.md)
  assert.equal(treHash("..\\..\\DATA\\LANGUAGE\\SUBTITLE\\0270.IFF"), 0x610c0bbc);
  assert.equal(treHash("..\\..\\DATA\\LANGUAGE\\SUBTITLE\\0720.IFF"), 0x610c0bbc);
});

test("lzw", () => {
  assert.deepEqual(lzw(lzwEncode(Buffer.from("A")), 1), Buffer.from("A"));
  // a code used the moment it is made (the decoder's "code === next" case)
  assert.deepEqual(lzw(lzwEncode(Buffer.from("AAAAAAAAAAAAAAAA")), 16), Buffer.from("AAAAAAAAAAAAAAAA"));
  // codes of every width, and a table that fills and starts over
  const big = sample(60000);
  const packed = lzwEncode(big);
  assert.ok(packed.length < big.length / 2);
  assert.deepEqual(lzw(packed, big.length), big);
  assert.throws(() => lzw(Buffer.from([0xff, 0x03]), 4), /LZW: bad code/); // 1ffh: not in the table
});

test("refpack", () => {
  const packed = Buffer.from([
    0xe0, 0x41, 0x42, 0x43, 0x44, // 4 literals: ABCD
    0x05, 0x04, 0x45,             // 1 literal (E), then 4 from 5 back: ABCD
    0x82, 0x00, 0x00,             // 6 from 1 back: the last byte repeated
    0xc0, 0x00, 0x0e, 0x00,       // 5 from 15 back: ABCDE
    0xfe, 0x58, 0x59,             // the end, with 2 literals: XY
  ]);
  assert.equal(refpack(packed, 22).toString("latin1"), "ABCDEABCDDDDDDDABCDEXY");
});

/** An XTRE version 1 archive: entries (packed as given), hash slots and names records. */
function archive(entries: { data: Buffer; pack?: "lzw" | "refpack"; stored: Buffer }[],
  slots: (namesAt: number, entriesAt: number) => { hash: number; at: number }[],
  names: (entriesAt: number) => { name: string; at: number }[]): Buffer {
  const hashAt = 24, nSlots = slots(0, 0).length;
  const namesAt = hashAt + 8 * nSlots;
  const namesSize = names(0).reduce((n, r) => n + 1 + r.name.length + 4, 0);
  const entriesAt = namesAt + namesSize, dataAt = entriesAt + 8 * entries.length;
  const b = Buffer.alloc(dataAt + entries.reduce((n, e) => n + e.stored.length, 0));
  b.write("XTRE", "latin1");
  [1, hashAt, namesAt, entriesAt, dataAt].forEach((v, i) => b.writeUInt32LE(v, 4 + 4 * i));
  slots(namesAt, entriesAt).forEach((s, i) => {
    b.writeUInt32LE(s.hash, hashAt + 8 * i);
    b.writeUInt32LE(s.at, hashAt + 8 * i + 4);
  });
  let o = namesAt;
  for (const r of names(entriesAt)) {
    b[o] = r.name.length;
    b.write(r.name, o + 1, "latin1");
    b.writeUInt32LE(r.at, o + 1 + r.name.length);
    o += 1 + r.name.length + 4;
  }
  let at = dataAt;
  entries.forEach((e, i) => {
    b.writeUInt32LE(at, entriesAt + 8 * i);
    b.writeUInt32LE(e.pack ? (0x80000000 | (e.pack === "refpack" ? 0x40000000 : 0) | e.data.length) >>> 0 : e.data.length, entriesAt + 8 * i + 4);
    e.stored.copy(b, at);
    at += e.stored.length;
  });
  return b;
}

test("readTre", (t) => {
  const dir = mkdtempSync(path.join(tmpdir(), "tre-test-"));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const plain = Buffer.from("FORMxxxxTESTplain"), text = sample(3000), abc = Buffer.from("ABCDEABCD");
  const a = "..\\..\\DATA\\LANGUAGE\\SUBTITLE\\0270.IFF", c = "..\\..\\DATA\\LANGUAGE\\SUBTITLE\\0720.IFF";
  const entries = [
    { data: plain, stored: plain },
    { data: text, pack: "lzw" as const, stored: lzwEncode(text) },
    { data: abc, pack: "refpack" as const, stored: Buffer.from([0xe0, 0x41, 0x42, 0x43, 0x44, 0x05, 0x04, 0x45, 0xfc]) },
  ];
  // entry 1 by its hash; entries 0 and 2 through the names table, where their paths collide
  const good = archive(entries,
    (namesAt, entriesAt) => [{ hash: treHash("B.IFF"), at: entriesAt + 8 }, { hash: 0, at: 0 }, { hash: treHash(a), at: namesAt }],
    (entriesAt) => [{ name: a, at: entriesAt }, { name: c, at: entriesAt + 16 }]);
  const file = path.join(dir, "good.tre");
  writeFileSync(file, good);
  const read = readTre(file);
  assert.deepEqual(read.map((e) => e.data), [plain, text, abc]);
  assert.deepEqual(read.map((e) => e.name), [a, undefined, c]);
  assert.deepEqual(read.map((e) => e.hash), [0x610c0bbc, treHash("B.IFF"), 0x610c0bbc]);
  assert.equal(nameEntries(file, read, ["X.IFF", "B.IFF"]), 1);
  assert.equal(read[1].name, "B.IFF");

  // a slot that leads nowhere, and a names record no slot's hash leads to
  const badSlot = path.join(dir, "slot.tre");
  writeFileSync(badSlot, archive(entries, () => [{ hash: 1, at: 7 }], () => []));
  assert.throws(() => readTre(badSlot), /points to 7, no entry/);
  const badName = path.join(dir, "name.tre");
  writeFileSync(badName, archive(entries, (namesAt) => [{ hash: 0x1234, at: namesAt }], (entriesAt) => [{ name: a, at: entriesAt }]));
  assert.throws(() => readTre(badName), /has no slot/);
  const notTre = path.join(dir, "not.tre");
  writeFileSync(notTre, Buffer.alloc(64));
  assert.throws(() => readTre(notTre), /not an XTRE archive/);
});
