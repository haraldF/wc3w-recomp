// The runtime's CPU helpers (runtime/wc_rt.h, cpu.cpp): test/rt_test.cpp, built with the system's
// compiler and run. No generated code and no SDL needed.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";

const root = path.resolve(import.meta.dirname, "..");

test("wc_rt.h and cpu.cpp", () => {
  const dir = path.join(root, "out", "test"), exe = path.join(dir, "rt_test");
  mkdirSync(dir, { recursive: true });
  const cc = spawnSync(process.env.CXX ?? "c++", ["-std=c++23", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-I", path.join(root, "runtime"), path.join(root, "test", "rt_test.cpp"), path.join(root, "runtime", "cpu.cpp"), "-o", exe],
  { encoding: "utf8" });
  assert.equal(cc.status, 0, `the compiler failed:\n${cc.stderr}`);
  const run = spawnSync(exe, [], { encoding: "utf8" });
  // the runtime's own reports (the divide overflows and the wrapped access the test makes) aren't failures
  const failures = run.stderr.split("\n").filter((l) => /rt_test\.cpp:\d+:|checks failed/.test(l));
  assert.equal(run.status, 0, failures.join("\n") || run.stderr);
});
