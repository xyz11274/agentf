// JavaScript 客户端的机械生成入口：以 TypeScript 为唯一实现源。
// 保留中文注释，同时验证去掉类型和空白不会改变可执行代码。

/* 仅供维护者执行的机械构建；使用者直接加载 evolution.mjs，无需安装 TypeScript。 */
import fs from "node:fs";
import path from "node:path";
import {fileURLToPath} from "node:url";
import {stripTypeScriptTypes} from "node:module";

const root = path.dirname(fileURLToPath(import.meta.url));
const input = fs.readFileSync(path.join(root, "typescript/evolution.ts"), "utf8");
// 当前源文件仅使用可擦除的类型语法；strip 保留中文注释和行布局。
// 不使用 transform 重新打印源码，否则交付的 JavaScript 会丢失全部函数说明。
const readable = stripTypeScriptTypes(input, {mode : "strip"})
                     .replace(/[ \t]+$/gm, "")
                     .replace(/\n{3,}/g, "\n\n");
// 如果将来加入依赖空白的模板字面量，不能为了排版悄悄改变字符串内容。
if (stripTypeScriptTypes(readable, {mode : "transform"}) !==
    stripTypeScriptTypes(input, {mode : "transform"}))
  throw new Error("javascript_format_changed_behavior");
const output = "// 由 ../typescript/evolution.ts 生成，请修改该源文件后重新生成。\n" + readable;
const target = path.join(root, "javascript/evolution.mjs");
if (process.argv.includes("--check")) {
  if (fs.readFileSync(target, "utf8") !== output)
    throw new Error("javascript_client_is_stale");
} else {
  fs.mkdirSync(path.dirname(target), {recursive : true});
  fs.writeFileSync(target, output);
}
