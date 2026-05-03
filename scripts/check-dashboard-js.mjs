#!/usr/bin/env node
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const files = process.argv.slice(2);
if (files.length === 0) {
  console.error("usage: node scripts/check-dashboard-js.mjs <html> [html...]");
  process.exit(2);
}

const scriptTag = /<script\b([^>]*)>([\s\S]*?)<\/script>/gi;
let failures = 0;
const dir = mkdtempSync(join(tmpdir(), "jarvis-dashboard-js-"));

try {
  for (const file of files) {
    const html = readFileSync(file, "utf8");
    let match;
    let index = 0;
    while ((match = scriptTag.exec(html)) !== null) {
      const attrs = match[1] || "";
      if (/\bsrc\s*=/.test(attrs)) {
        continue;
      }
      const js = match[2].trim();
      if (!js) {
        continue;
      }
      index += 1;
      const out = join(dir, `${file.replace(/[^A-Za-z0-9_.-]/g, "_")}.${index}.js`);
      writeFileSync(out, `${js}\n`, "utf8");
      const result = spawnSync(process.execPath, ["--check", out], {
        encoding: "utf8",
      });
      if (result.status !== 0) {
        failures += 1;
        console.error(`JavaScript syntax check failed: ${file} inline script ${index}`);
        process.stderr.write(result.stderr || result.stdout || "");
      }
    }
    if (index === 0) {
      console.warn(`No inline scripts found in ${file}`);
    }
  }
} finally {
  rmSync(dir, { recursive: true, force: true });
}

process.exit(failures === 0 ? 0 : 1);
