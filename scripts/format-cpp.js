#!/usr/bin/env node
"use strict";

// 对 C++ 源码运行 clang-format。不带参数时处理 src 下全部文件，--files 为 lint-staged 传入的列表。

const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const sourceRoot = path.join(root, "src");
const suffixes = new Set([".cpp", ".hpp"]);

// clang-format 不在 PATH 时回退到 VS2022 自带 LLVM
const clangCandidates = [
  "clang-format",
  "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\Llvm\\x64\\bin\\clang-format.exe",
  "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Tools\\Llvm\\x64\\bin\\clang-format.exe",
  "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Tools\\Llvm\\x64\\bin\\clang-format.exe",
  "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\Llvm\\x64\\bin\\clang-format.exe",
];

function findClangFormat() {
  for (const candidate of clangCandidates) {
    const result = spawnSync(candidate, ["--version"], { stdio: "ignore" });
    if (result.status === 0) return candidate;
  }
  return null;
}

function collect(directory) {
  const files = [];
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const full = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      files.push(...collect(full));
    } else if (suffixes.has(path.extname(entry.name))) {
      files.push(full);
    }
  }
  return files;
}

function resolveFiles(args) {
  if (args[0] === "--files") args = args.slice(1);
  const candidates = args.length > 0 ? args.map((file) => path.resolve(file)) : collect(sourceRoot);
  // 生成产物每次构建都会被 scripts/generate-locales.js 覆盖，格式化它只会产生反复变动
  return candidates.filter(
    (file) =>
      suffixes.has(path.extname(file)) &&
      !path.relative(root, file).split(path.sep).includes("generated") &&
      fs.existsSync(file),
  );
}

function main() {
  const files = resolveFiles(process.argv.slice(2));
  if (files.length === 0) return 0;

  const clangFormat = findClangFormat();
  if (!clangFormat) {
    console.warn("⚠ 未找到 clang-format，跳过 C++ 格式化。请安装 LLVM 或 VS2022 的 C++ 工作负载。");
    return 0;
  }

  const result = spawnSync(clangFormat, ["-i", ...files], { stdio: "inherit" });
  if (result.error) {
    console.error(`✗ ${clangFormat}: ${result.error.message}`);
    return 1;
  }
  return result.status === 0 ? 0 : 1;
}

process.exit(main());
