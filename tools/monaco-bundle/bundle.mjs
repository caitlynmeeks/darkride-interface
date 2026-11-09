//
//  bundle.mjs
//  tools/monaco-bundle
//
//  Builds the Code panel's copy of Monaco, the editor inside VS Code, into interface/resources/html/code/monaco/, the
//  files the app serves from qrc:///html/code/monaco/ (Make v1, team/editor-ui/CODE-EDITOR-PLAN.md). From skunkworks'
//  Monaco prototype (11-06): Monaco's ESM build bundled with esbuild into one classic script and one style sheet, and
//  its workers as self-contained classic scripts, so the page needs no module loader, no eval and no network.
//  Kept: the editor with all its features, JavaScript (its Monarch colouring and the TypeScript worker's syntax
//  errors). Left out: every other language (the page adds its own small GLSL colouring) and JSON.
//
//    npm ci            (in this folder: esbuild and monaco-editor at the versions in package-lock.json)
//    node bundle.mjs   [out dir, default ../../interface/resources/html/code/monaco]
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";

const here = path.dirname(new URL(import.meta.url).pathname);
const out = path.resolve(process.argv[2] || path.join(here, "../../interface/resources/html/code/monaco"));
const require = createRequire(path.join(here, "package.json"));
const esbuild = require("esbuild");
const monacoRoot = path.dirname(require.resolve("monaco-editor/package.json"));
const vs = path.join(monacoRoot, "esm/vs");
const version = JSON.parse(fs.readFileSync(path.join(monacoRoot, "package.json"), "utf8")).version;

fs.rmSync(out, { recursive: true, force: true });
fs.mkdirSync(out, { recursive: true });

const common = { bundle: true, minify: true, target: "chrome120", legalComments: "eof", logLevel: "warning" };

// the editor: one classic script that sets self.monaco, its style sheet and its icon font
const entry = path.join(out, "_entry.js");
fs.writeFileSync(entry, [
    `import '${vs}/editor/edcore.main.js';`,
    `import * as monaco from '${vs}/editor/editor.api.js';`,
    `import '${vs}/language/typescript/monaco.contribution.js';`,
    `import '${vs}/basic-languages/javascript/javascript.contribution.js';`,
    `self.monaco = monaco;`,
].join("\n"));
await esbuild.build({
    ...common, entryPoints: { editor: entry }, format: "iife", outdir: out,
    loader: { ".ttf": "file" }, assetNames: "[name]",
});
fs.rmSync(entry);

// the workers, each a classic script that needs nothing from importScripts
const workers = {
    "editor.worker": `${vs}/editor/editor.worker.js`,
    "ts.worker": `${vs}/language/typescript/ts.worker.js`,
};
for (const [name, file] of Object.entries(workers)) {
    await esbuild.build({ ...common, entryPoints: [file], format: "iife", outfile: path.join(out, `${name}.js`) });
}

// Monaco's licence and its third-party notices beside the files (MIT)
fs.copyFileSync(path.join(monacoRoot, "LICENSE"), path.join(out, "LICENSE.txt"));
fs.copyFileSync(path.join(monacoRoot, "ThirdPartyNotices.txt"), path.join(out, "ThirdPartyNotices.txt"));
fs.writeFileSync(path.join(out, "VERSION.txt"),
    `monaco-editor ${version}, bundled by tools/monaco-bundle/bundle.mjs (esbuild ${esbuild.version})\n`);

for (const name of fs.readdirSync(out).sort()) {
    console.log(`${(fs.statSync(path.join(out, name)).size / 1e6).toFixed(2).padStart(6)} MB  ${name}`);
}
