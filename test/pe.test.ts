// The PE reader (src/pe.ts) on a module made here: one section with an import by name and one by
// ordinal, an export, and base relocations.

import assert from "node:assert/strict";
import { test } from "node:test";
import { parsePE, rebase } from "../src/pe.ts";

const BASE = 0x400000, TEXT = 0x1000, RAW = 0x200;

function module(): Buffer {
  const f = Buffer.alloc(RAW + 0x200);
  const u32 = (o: number, v: number): void => { f.writeUInt32LE(v >>> 0, o); };
  const u16 = (o: number, v: number): void => { f.writeUInt16LE(v, o); };
  const str = (rva: number, s: string): void => { f.write(s + "\0", RAW + rva - TEXT, "latin1"); };
  f.write("MZ", "latin1");
  u32(0x3c, 0x40);
  f.write("PE\0\0", 0x40, "latin1");
  const coff = 0x44, opt = coff + 20;
  u16(coff, 0x14c);    // i386
  u16(coff + 2, 1);    // sections
  u32(coff + 4, 0x31e5b8a0);
  u16(coff + 16, 224); // the optional header's size
  u16(coff + 18, 0x2102); // executable, 32 bits, a DLL
  u16(opt, 0x10b);
  u32(opt + 16, TEXT + 0x10);  // entry
  u32(opt + 28, BASE);
  u32(opt + 56, 0x3000);       // SizeOfImage
  u32(opt + 60, RAW);          // SizeOfHeaders
  u16(opt + 68, 2);            // GUI
  u32(opt + 72, 0x100000);     // stack reserve
  u32(opt + 92, 16);
  const dir = (i: number, rva: number, size: number): void => { u32(opt + 96 + 8 * i, rva); u32(opt + 100 + 8 * i, size); };
  const s = opt + 224;
  f.write(".text", s, "latin1");
  u32(s + 8, 0x200);   // virtual size
  u32(s + 12, TEXT);
  u32(s + 16, 0x200);  // raw size
  u32(s + 20, RAW);
  u32(s + 36, 0x60000020);
  const at = (rva: number): number => RAW + rva - TEXT;

  // code: a pointer to relocate at 1020h (to 401040h), and one at 1024h
  u32(at(0x1020), BASE + 0x1040);
  u32(at(0x1024), BASE + 0x1044);
  // imports at 1080h: one DLL, its lookup table at 10c0h, its address table at 10d0h
  dir(1, 0x1080, 40);
  u32(at(0x1080), 0x10c0);
  u32(at(0x1080) + 12, 0x10e0);
  u32(at(0x1080) + 16, 0x10d0);
  u32(at(0x10c0), 0x10f0);          // by name: a hint, then the name
  u32(at(0x10c4), 0x80000007);      // by ordinal 7
  str(0x10e0, "KERNEL32.dll");
  str(0x10f2, "Sleep");
  // exports at 1100h: ordinal base 3, two functions, the second one named
  dir(0, 0x1100, 0x60);
  u32(at(0x1100) + 12, 0x1150);
  u32(at(0x1100) + 16, 3);
  u32(at(0x1100) + 20, 2);
  u32(at(0x1100) + 24, 1);
  u32(at(0x1100) + 28, 0x1130);
  u32(at(0x1100) + 32, 0x1138);
  u32(at(0x1100) + 36, 0x113c);
  u32(at(0x1130), 0x1010);
  u32(at(0x1134), 0x1014);
  u32(at(0x1138), 0x1158);
  u16(at(0x113c), 1);
  str(0x1150, "T.dll");
  str(0x1158, "second");
  // base relocations at 1180h: a block for page 1000h with two HIGHLOW entries and a padding one
  dir(5, 0x1180, 16);
  u32(at(0x1180), 0x1000);
  u32(at(0x1184), 16);
  u16(at(0x1188), 0x3020);
  u16(at(0x118a), 0x3024);
  u16(at(0x118c), 0);
  return f;
}

test("parsePE", () => {
  const pe = parsePE(module());
  assert.equal(pe.base, BASE);
  assert.equal(pe.entry, BASE + 0x1010);
  assert.equal(pe.isDll, true);
  assert.equal(pe.stackReserve, 0x100000);
  assert.equal(pe.image.length, 0x3000);
  assert.deepEqual(pe.sections.map((s) => [s.name, s.rva]), [[".text", TEXT]]);
  assert.deepEqual(pe.imports, [
    { dll: "KERNEL32.dll", name: "Sleep", iat: BASE + 0x10d0 },
    { dll: "KERNEL32.dll", ordinal: 7, iat: BASE + 0x10d4 },
  ]);
  assert.equal(pe.exportName, "T.dll");
  assert.deepEqual(pe.exports, [
    { name: undefined, ordinal: 3, address: BASE + 0x1010 },
    { name: "second", ordinal: 4, address: BASE + 0x1014 },
  ]);
  assert.deepEqual(pe.relocations, [0x1020, 0x1024]);
  assert.equal(pe.image.readUInt32LE(0x1020), BASE + 0x1040);
});

test("rebase", () => {
  const pe = parsePE(module());
  rebase(pe, 0x200000);
  assert.equal(pe.base, 0x200000);
  assert.equal(pe.entry, 0x201010);
  assert.equal(pe.image.readUInt32LE(0x1020), 0x201040);
  assert.equal(pe.image.readUInt32LE(0x1024), 0x201044);
  assert.equal(pe.imports[0].iat, 0x2010d0);
  assert.equal(pe.exports[1].address, 0x201014);
});

test("not a PE", () => {
  assert.throws(() => parsePE(Buffer.alloc(128)), /not an MZ executable/);
  const f = module();
  f.writeUInt16LE(0x8664, 0x44);
  assert.throws(() => parsePE(f), /not an i386 module/);
  const g = module();
  g.writeUInt16LE(0xa020, 0x200 + 0x18c); // a relocation of type 10
  assert.throws(() => parsePE(g), /base relocation type 10/);
});
