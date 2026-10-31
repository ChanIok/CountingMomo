#!/usr/bin/env node
"use strict";

// pnpm run build 为 debug 构建，pnpm run build --release 为 release 构建。

const { spawnSync } = require("node:child_process");
const path = require("node:path");

const target = "CountingMomo";
const root = path.resolve(__dirname, "..");

const args = process.argv.slice(2);
if (args.length > 1 || (args.length === 1 && args[0] !== "--release")) {
  console.error("Usage: pnpm run build [--release]");
  process.exit(1);
}

function run(argv) {
  const result = spawnSync("xmake", argv, { cwd: root, stdio: "inherit" });
  if (result.error) {
    console.error(`Failed to run xmake: ${result.error.message}`);
    process.exit(1);
  }
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

const mode = args.includes("--release") ? "release" : "debug";
console.log(`Building CountingMomo (${mode})`);
run(["f", "-p", "windows", "-a", "x64", "-m", mode]);
run(["build", target]);
