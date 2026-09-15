/** Maintainer-only mechanical build. Consumers use evolution.mjs without TypeScript. */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { stripTypeScriptTypes } from "node:module";

const root = path.dirname(fileURLToPath(import.meta.url));
const input = fs.readFileSync(path.join(root, "typescript/evolution.ts"), "utf8");
const output = "// Generated from ../typescript/evolution.ts; edit that source and regenerate.\n"
  + stripTypeScriptTypes(input, { mode: "transform" });
const target = path.join(root, "javascript/evolution.mjs");
if (process.argv.includes("--check")) {
  if (fs.readFileSync(target, "utf8") !== output) throw new Error("javascript_client_is_stale");
} else { fs.mkdirSync(path.dirname(target), { recursive: true }); fs.writeFileSync(target, output); }
