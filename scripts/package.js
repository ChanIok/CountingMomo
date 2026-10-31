#!/usr/bin/env node
"use strict";

const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");

function main() {
  if (process.platform !== "win32") throw new Error("Packaging requires Windows PowerShell.");

  const manifest = fs.readFileSync(path.join(root, "resources/app.manifest"), "utf8");
  const windowsVersion = manifest.match(/<assemblyIdentity\s+version="(\d+\.\d+\.\d+\.\d+)"/)?.[1];
  if (!windowsVersion) throw new Error("Could not read a four-part version number from resources/app.manifest.");
  const versionParts = windowsVersion.split(".").map(Number);
  if (versionParts.some((part) => part > 65535)) throw new Error("Version parts must be between 0 and 65535.");
  if (windowsVersion.split(".")[3] !== "0") throw new Error("CountingMomo assemblyIdentity version must end in .0.");
  const version = versionParts.slice(0, 3).join(".");

  const exe = path.join(root, "build/windows/x64/release/CountingMomo.exe");
  if (!fs.existsSync(exe)) throw new Error("Release executable not found. Run 'pnpm run build --release' first.");

  const versionResult = spawnSync(
    "powershell.exe",
    [
      "-NoProfile",
      "-NonInteractive",
      "-Command",
      `$ErrorActionPreference = 'Stop'
$info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($env:COUNTING_MOMO_PACKAGE_EXE)
@{
  hasVersion = [bool]($info.FileVersion -and $info.ProductVersion)
  file = @($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart)
  product = @($info.ProductMajorPart, $info.ProductMinorPart, $info.ProductBuildPart, $info.ProductPrivatePart)
} | ConvertTo-Json -Compress`,
    ],
    {
      cwd: root,
      encoding: "utf8",
      stdio: ["ignore", "pipe", "inherit"],
      env: { ...process.env, COUNTING_MOMO_PACKAGE_EXE: exe },
    },
  );
  if (versionResult.error) throw new Error(`Failed to run PowerShell: ${versionResult.error.message}`);
  if (versionResult.status !== 0) throw new Error("Could not read the executable version information.");
  const executableVersion = JSON.parse(versionResult.stdout);
  if (!executableVersion.hasVersion) {
    throw new Error("Executable version information is missing. Run 'pnpm run build --release' first.");
  }
  const fileVersion = executableVersion.file.join(".");
  const productVersion = executableVersion.product.join(".");
  const expectedVersion = versionParts.join(".");
  if (fileVersion !== expectedVersion || productVersion !== expectedVersion) {
    throw new Error(
      `Executable version mismatch: expected ${expectedVersion}, file version ${fileVersion}, product version ${productVersion}. Run 'pnpm run build --release' first.`,
    );
  }

  if (!fs.existsSync(path.join(root, "resources/scan/hidden_question.png"))) {
    throw new Error("Question-mark placeholder image not found: resources/scan/hidden_question.png.");
  }

  const dist = path.join(root, "dist");
  const zipName = `CountingMomo-${version}-windows-x64.zip`;
  fs.mkdirSync(dist, { recursive: true });
  // 暂存目录每次独立创建，只清理本次暂存内容，不触碰已有发行目录和用户数据。
  const staging = fs.mkdtempSync(path.join(dist, ".package-"));
  try {
    const directory = path.join(staging, "CountingMomo");
    fs.mkdirSync(path.join(directory, "resources"), { recursive: true });
    fs.copyFileSync(exe, path.join(directory, "CountingMomo.exe"));
    for (const name of ["scan", "categories"]) {
      fs.cpSync(path.join(root, "resources", name), path.join(directory, "resources", name), { recursive: true });
    }

    const temporaryZip = path.join(staging, zipName);
    // 路径通过环境变量传入，不拼接到 PowerShell 命令中；压缩目录本身以保留顶层文件夹。
    const result = spawnSync(
      "powershell.exe",
      [
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "$ErrorActionPreference = 'Stop'; Compress-Archive -LiteralPath $env:COUNTING_MOMO_PACKAGE_SOURCE -DestinationPath $env:COUNTING_MOMO_PACKAGE_ZIP -CompressionLevel Optimal",
      ],
      {
        cwd: root,
        stdio: "inherit",
        env: {
          ...process.env,
          COUNTING_MOMO_PACKAGE_SOURCE: directory,
          COUNTING_MOMO_PACKAGE_ZIP: temporaryZip,
        },
      },
    );
    if (result.error) throw new Error(`Failed to run PowerShell: ${result.error.message}`);
    if (result.status !== 0) throw new Error(`Compression failed. PowerShell exit status: ${result.status ?? result.signal}`);

    const destination = path.join(dist, zipName);
    fs.copyFileSync(temporaryZip, destination);
    console.log(`Package created: ${destination}`);
  } finally {
    fs.rmSync(staging, { recursive: true, force: true });
  }
}

try {
  main();
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
