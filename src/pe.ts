// Windows Portable Executables (PE32, i386): WC3W.EXE and the DLLs it brings. A module is mapped the
// way Windows loads it, each section at its address relative to the image base, so code addresses
// are the program's own; its imports, base relocations and exports are listed.

export interface Section {
  name: string;
  rva: number;
  virtualSize: number;
  rawSize: number;
  characteristics: number;
}

/** One imported function: by name or by ordinal, and the import address table slot the loader fills. */
export interface Import {
  dll: string;
  name?: string;
  ordinal?: number;
  /** the slot's address (image base + RVA): code calls through it (CALL [iat]) */
  iat: number;
}

export interface Export {
  name?: string;
  ordinal: number;
  /** image base + RVA, or the forwarder's text ("DLL.name") */
  address: number | string;
}

export interface PE {
  /** the module as loaded: SizeOfImage bytes from the image base */
  image: Buffer;
  base: number;
  entry: number;
  timeDateStamp: number;
  machine: number;
  isDll: boolean;
  subsystem: number;
  stackReserve: number;
  stackCommit: number;
  heapReserve: number;
  heapCommit: number;
  sections: Section[];
  imports: Import[];
  exports: Export[];
  /** image RVAs of the 32-bit words base relocations patch (type 3, HIGHLOW) */
  relocations: number[];
  /** the export directory's DLL name */
  exportName?: string;
  /** the resource directory (RVA, size), for FindResource and its kin */
  resources: { rva: number; size: number };
}

const IMAGE_FILE_DLL = 0x2000;

export function parsePE(file: Buffer): PE {
  if (file.toString("latin1", 0, 2) !== "MZ") throw new Error("not an MZ executable");
  const pe = file.readUInt32LE(0x3c);
  if (file.toString("latin1", pe, pe + 4) !== "PE\0\0") throw new Error("no PE header");
  const coff = pe + 4;
  const machine = file.readUInt16LE(coff);
  if (machine !== 0x14c) throw new Error(`not an i386 module (machine ${machine.toString(16)})`);
  const nSections = file.readUInt16LE(coff + 2);
  const optSize = file.readUInt16LE(coff + 16);
  const characteristics = file.readUInt16LE(coff + 18);
  const opt = coff + 20;
  if (file.readUInt16LE(opt) !== 0x10b) throw new Error("not a PE32 module");
  const entryRva = file.readUInt32LE(opt + 16);
  const base = file.readUInt32LE(opt + 28);
  const sizeOfImage = file.readUInt32LE(opt + 56);
  const sizeOfHeaders = file.readUInt32LE(opt + 60);
  const nDirs = file.readUInt32LE(opt + 92);
  const dir = (i: number): { rva: number; size: number } =>
    i < nDirs ? { rva: file.readUInt32LE(opt + 96 + 8 * i), size: file.readUInt32LE(opt + 100 + 8 * i) } : { rva: 0, size: 0 };

  const image = Buffer.alloc(sizeOfImage);
  file.copy(image, 0, 0, Math.min(sizeOfHeaders, file.length));
  const sections: Section[] = [];
  for (let i = 0; i < nSections; i++) {
    const s = opt + optSize + 40 * i;
    const sec: Section = {
      name: file.toString("latin1", s, s + 8).replace(/\0+$/, ""),
      virtualSize: file.readUInt32LE(s + 8),
      rva: file.readUInt32LE(s + 12),
      rawSize: file.readUInt32LE(s + 16),
      characteristics: file.readUInt32LE(s + 36),
    };
    const raw = file.readUInt32LE(s + 20);
    const n = Math.min(sec.rawSize, sec.virtualSize || sec.rawSize, sizeOfImage - sec.rva);
    if (raw && n > 0) file.copy(image, sec.rva, raw, raw + n);
    sections.push(sec);
  }
  const cstr = (rva: number): string => image.toString("latin1", rva, image.indexOf(0, rva));

  // imports: a descriptor per DLL (20 bytes, ended by a zero one), its lookup table (or, without
  // one, the address table as the file has it) naming each function
  const imports: Import[] = [];
  const imp = dir(1);
  for (let d = imp.rva; imp.rva && image.readUInt32LE(d + 12) !== 0; d += 20) {
    const dll = cstr(image.readUInt32LE(d + 12));
    const lookup = image.readUInt32LE(d) || image.readUInt32LE(d + 16);
    const iat = image.readUInt32LE(d + 16);
    for (let k = 0; ; k++) {
      const t = image.readUInt32LE(lookup + 4 * k);
      if (t === 0) break;
      const slot = base + iat + 4 * k;
      if (t & 0x80000000) imports.push({ dll, ordinal: t & 0xffff, iat: slot });
      else imports.push({ dll, name: cstr(t + 2), iat: slot });
    }
  }

  // exports: addresses by ordinal, names pointing at ordinals; an address inside the export
  // directory is a forwarder's text
  const exports: Export[] = [];
  const ex = dir(0);
  let exportName: string | undefined;
  if (ex.rva) {
    exportName = cstr(image.readUInt32LE(ex.rva + 12));
    const ordBase = image.readUInt32LE(ex.rva + 16);
    const nFuncs = image.readUInt32LE(ex.rva + 20), nNames = image.readUInt32LE(ex.rva + 24);
    const funcs = image.readUInt32LE(ex.rva + 28), names = image.readUInt32LE(ex.rva + 32);
    const ords = image.readUInt32LE(ex.rva + 36);
    const nameOf = new Map<number, string>();
    for (let i = 0; i < nNames; i++) {
      nameOf.set(image.readUInt16LE(ords + 2 * i), cstr(image.readUInt32LE(names + 4 * i)));
    }
    for (let i = 0; i < nFuncs; i++) {
      const rva = image.readUInt32LE(funcs + 4 * i);
      if (!rva) continue;
      const forwarder = rva >= ex.rva && rva < ex.rva + ex.size;
      exports.push({ name: nameOf.get(i), ordinal: ordBase + i, address: forwarder ? cstr(rva) : base + rva });
    }
  }

  // base relocations: blocks of a page RVA, the block's size and 16-bit entries (type, offset)
  const relocations: number[] = [];
  const rel = dir(5);
  for (let b = rel.rva; rel.rva && b < rel.rva + rel.size;) {
    const page = image.readUInt32LE(b), size = image.readUInt32LE(b + 4);
    if (size < 8) break;
    for (let e = b + 8; e < b + size; e += 2) {
      const v = image.readUInt16LE(e);
      const type = v >> 12;
      if (type === 3) relocations.push(page + (v & 0xfff));
      else if (type !== 0) throw new Error(`base relocation type ${type} is not supported`);
    }
    b += size;
  }

  return {
    image, base, entry: base + entryRva, timeDateStamp: file.readUInt32LE(coff + 4), machine,
    isDll: (characteristics & IMAGE_FILE_DLL) !== 0, subsystem: file.readUInt16LE(opt + 68),
    stackReserve: file.readUInt32LE(opt + 72), stackCommit: file.readUInt32LE(opt + 76),
    heapReserve: file.readUInt32LE(opt + 80), heapCommit: file.readUInt32LE(opt + 84),
    sections, imports, exports, relocations, exportName, resources: dir(2),
  };
}

/** Moves a mapped module to another base: adds the difference to every relocated word. */
export function rebase(p: PE, newBase: number): void {
  const delta = (newBase - p.base) >>> 0;
  for (const r of p.relocations) p.image.writeUInt32LE((p.image.readUInt32LE(r) + delta) >>> 0, r);
  const move = (a: number): number => (a + delta) >>> 0;
  p.entry = move(p.entry);
  for (const i of p.imports) i.iat = move(i.iat);
  for (const e of p.exports) if (typeof e.address === "number") e.address = move(e.address);
  p.base = newBase;
}
