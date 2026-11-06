# Monaco for the Code panel

The Code panel (Make v1) shows Monaco, the editor inside VS Code (MIT), as a page from the app's own files:
`interface/resources/html/code/`. Its copy of Monaco is in `interface/resources/html/code/monaco/`, built here and
checked in, so a build needs no Node and no network.

To build it again (after changing the version in package.json):

    cd tools/monaco-bundle
    npm install          # or npm ci, to keep package-lock.json's exact versions
    node bundle.mjs

Then check the Code panel opens, colours a script and shows a syntax error, and commit the new files with the
package-lock.json. The page's content security policy allows scripts and workers from the app's own files only and
no eval, so a new Monaco that needs eval or a module loader won't start: keep the bundle classic scripts.

Kept: the editor's features and JavaScript (colouring, plus syntax errors from the TypeScript worker). The page adds
its own GLSL colouring for shaders. Every other language is left out.
