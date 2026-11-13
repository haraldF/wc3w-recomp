// The HD movies' .srt reader (runtime/srt.cpp): test/srt_test.cpp, built with the system's
// compiler and run. No FreeType, FFmpeg or game files needed.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";

const root = path.resolve(import.meta.dirname, "..");

test("srt.cpp", () => {
  const dir = path.join(root, "out", "test"), exe = path.join(dir, "srt_test");
  mkdirSync(dir, { recursive: true });
  const cc = spawnSync(process.env.CXX ?? "c++", ["-std=c++23", "-O1", "-Wall", "-Wextra",
    "-I", path.join(root, "runtime"), path.join(root, "test", "srt_test.cpp"), path.join(root, "runtime", "srt.cpp"), "-o", exe],
  { encoding: "utf8" });
  assert.equal(cc.status, 0, `the compiler failed:\n${cc.stderr}`);
  const run = spawnSync(exe, [], { encoding: "utf8" });
  assert.equal(run.status, 0, run.stderr);
});
