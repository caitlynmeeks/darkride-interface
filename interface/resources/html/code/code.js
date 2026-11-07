//
//  code.js
//  html/code
//
//  The Code panel's page (Make v1, team/editor-ui/CODE-EDITOR-PLAN.md): one Monaco editor on the script the panel
//  opened, its error line and its status strip, plus Monaco's side-by-side view for what Claude changed.
//
//  The page has no bridge object and reaches nothing: the panel (qml/darkride/CodePanel.qml) calls darkrideCode's
//  fixed functions with JSON arguments, as Radio's page is called, and takes the page's events with takeEvents() when
//  the title says "Code: events". Every event is data the panel checks: "ready", "save" (the person's Cmd+S),
//  "changed", "escape". Saving is the panel's, with the person's own edit rights, never the page's.
//
//  World text (the script, its error, its name) is shown as text only: Monaco's own model and plain marker strings,
//  and textContent for the rest, never innerHTML or trusted markdown (security's item 7). Links in the code aren't
//  clickable, and the panel refuses every navigation and new window.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

/* global monaco */

(function () {
    "use strict";

    var MAX_TEXT = 4 * 1024 * 1024;   // characters: more than any script a world would carry
    var MINIMAP_WIDTH = 640;          // the minimap shows when the panel is at least this wide

    // Monaco's workers, from the app's own files (classic scripts: tools/monaco-bundle)
    self.MonacoEnvironment = {
        getWorkerUrl: function (moduleId, label) {
            return label === "typescript" || label === "javascript" ? "monaco/ts.worker.js" : "monaco/editor.worker.js";
        }
    };

    // ---- events for the panel ----

    var events = [];
    var eventCount = 0;
    function post(event) {
        events.push(event);
        if (events.length > 100) {
            events.shift();
        }
        eventCount += 1;
        document.title = "Code: events " + eventCount;
    }

    // ---- Darkride's look ----

    monaco.editor.defineTheme("darkride", {
        base: "vs-dark",
        inherit: true,
        rules: [
            { token: "", foreground: "f4f6f8" },
            { token: "keyword", foreground: "c792ff" },
            { token: "string", foreground: "50fa7b" },
            { token: "string.escape", foreground: "8be9fd" },
            { token: "number", foreground: "00afff" },
            { token: "comment", foreground: "9aa1bd", fontStyle: "italic" },
            { token: "regexp", foreground: "ff79c6" },
            { token: "type", foreground: "8be9fd" },
            { token: "type.identifier", foreground: "8be9fd" },
            { token: "predefined", foreground: "ffb86c" },
            { token: "delimiter", foreground: "c8cce0" }
        ],
        colors: {
            "editor.background": "#171b2d",
            "editor.foreground": "#f4f6f8",
            "editor.selectionBackground": "#333a5a",
            "editor.inactiveSelectionBackground": "#2a3050",
            "editor.lineHighlightBackground": "#1d2238",
            "editor.lineHighlightBorder": "#00000000",
            "editorCursor.foreground": "#f4f6f8",
            "editorLineNumber.foreground": "#5c6385",
            "editorLineNumber.activeForeground": "#c8cce0",
            "editorError.foreground": "#ff5555",
            "editorWarning.foreground": "#ffb86c",
            "editorWidget.background": "#1d2238",
            "editorWidget.border": "#333a5a",
            "editorHoverWidget.background": "#1d2238",
            "editorHoverWidget.border": "#333a5a",
            "editorSuggestWidget.background": "#1d2238",
            "editorSuggestWidget.border": "#333a5a",
            "editorSuggestWidget.selectedBackground": "#333a5a",
            "minimap.background": "#171b2d",
            "scrollbarSlider.background": "#ffffff1f",
            "scrollbarSlider.hoverBackground": "#ffffff33",
            "scrollbarSlider.activeBackground": "#ffffff44",
            "diffEditor.insertedTextBackground": "#50fa7b26",
            "diffEditor.removedTextBackground": "#ff555530",
            "diffEditor.insertedLineBackground": "#50fa7b14",
            "diffEditor.removedLineBackground": "#ff55551a"
        }
    });

    // ---- GLSL, for procedural shaders: Monaco has none of its own ----

    monaco.languages.register({ id: "glsl", extensions: [".fs", ".vs", ".frag", ".vert", ".glsl"] });
    monaco.languages.setMonarchTokensProvider("glsl", {
        keywords: [
            "attribute", "break", "case", "centroid", "const", "continue", "default", "discard", "do", "else", "flat",
            "for", "highp", "if", "in", "inout", "invariant", "layout", "lowp", "mediump", "noperspective", "out",
            "precision", "return", "smooth", "struct", "switch", "uniform", "varying", "while", "true", "false"
        ],
        types: [
            "void", "bool", "int", "uint", "float", "double", "vec2", "vec3", "vec4", "bvec2", "bvec3", "bvec4",
            "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "dvec2", "dvec3", "dvec4", "mat2", "mat3", "mat4",
            "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4", "sampler2D",
            "sampler3D", "samplerCube", "sampler2DShadow", "sampler2DArray"
        ],
        builtins: [
            "abs", "acos", "asin", "atan", "ceil", "clamp", "cos", "cross", "degrees", "dFdx", "dFdy", "distance", "dot",
            "exp", "exp2", "faceforward", "floor", "fract", "fwidth", "inversesqrt", "length", "log", "log2", "max",
            "min", "mix", "mod", "normalize", "pow", "radians", "reflect", "refract", "round", "sign", "sin",
            "smoothstep", "sqrt", "step", "tan", "texture", "textureLod", "transpose", "inverse", "iGlobalTime",
            "iWorldScale", "iWorldPosition", "iWorldOrientation", "iChannel0", "iChannel1", "iChannel2", "iChannel3",
            "iDate", "iFrameCount", "iChannelResolution", "_position", "_normal", "_color", "_texCoord0"
        ],
        operators: ["=", ">", "<", "!", "~", "?", ":", "==", "<=", ">=", "!=", "&&", "||", "++", "--", "+", "-", "*",
            "/", "&", "|", "^", "%", "<<", ">>", "+=", "-=", "*=", "/=", "&=", "|=", "^=", "%=", "<<=", ">>="],
        symbols: /[=><!~?:&|+\-*\/\^%]+/,
        tokenizer: {
            root: [
                [/^\s*#\s*\w+/, "keyword"],
                [/[a-zA-Z_]\w*/, { cases: { "@keywords": "keyword", "@types": "type", "@builtins": "predefined",
                    "@default": "identifier" } }],
                { include: "@whitespace" },
                [/[{}()\[\]]/, "@brackets"],
                [/@symbols/, { cases: { "@operators": "operator", "@default": "" } }],
                [/\d*\.\d+([eE][\-+]?\d+)?[fF]?/, "number.float"],
                [/\d+[eE][\-+]?\d+[fF]?/, "number.float"],
                [/0[xX][0-9a-fA-F]+[uU]?/, "number.hex"],
                [/\d+[uU]?/, "number"],
                [/[;,.]/, "delimiter"]
            ],
            whitespace: [
                [/[ \t\r\n]+/, "white"],
                [/\/\*/, "comment", "@comment"],
                [/\/\/.*$/, "comment"]
            ],
            comment: [
                [/[^\/*]+/, "comment"],
                [/\*\//, "comment", "@pop"],
                [/[\/*]/, "comment"]
            ]
        }
    });
    monaco.languages.setLanguageConfiguration("glsl", {
        comments: { lineComment: "//", blockComment: ["/*", "*/"] },
        brackets: [["{", "}"], ["[", "]"], ["(", ")"]],
        autoClosingPairs: [{ open: "{", close: "}" }, { open: "[", close: "]" }, { open: "(", close: ")" }]
    });

    // ---- JavaScript: syntax errors from the TypeScript worker; no type checks, which would mark every engine name
    // (Entities, Script, MyAvatar) as unknown ----

    var js = monaco.languages.typescript.javascriptDefaults;
    js.setDiagnosticsOptions({ noSemanticValidation: true, noSyntaxValidation: false, noSuggestionDiagnostics: true });
    js.setCompilerOptions({
        target: monaco.languages.typescript.ScriptTarget.ES2020,
        allowNonTsExtensions: true,
        allowJs: true,
        lib: ["es2020"]
    });
    js.setEagerModelSync(false);

    // ---- the editor ----

    var editorElement = document.getElementById("editor");
    var diffElement = document.getElementById("diff");
    var errorElement = document.getElementById("error");
    var positionElement = document.getElementById("position");
    var languageElement = document.getElementById("language");
    var linesElement = document.getElementById("lines");
    var savedElement = document.getElementById("saved");

    var model = monaco.editor.createModel("", "javascript");
    var editor = monaco.editor.create(editorElement, {
        model: model,
        theme: "darkride",
        automaticLayout: true,
        fontFamily: "\"SF Mono\", Menlo, Consolas, \"DejaVu Sans Mono\", monospace",
        fontSize: 12,
        lineHeight: 18,
        tabSize: 4,
        insertSpaces: true,
        minimap: { enabled: false },
        scrollBeyondLastLine: false,
        renderLineHighlight: "line",
        cursorBlinking: "blink",
        links: false,               // a link in a world's script never opens anything
        contextmenu: false,         // its Paste would need the clipboard permission, which the panel refuses: Cmd+V works
        dropIntoEditor: { enabled: false },
        fixedOverflowWidgets: true,
        readOnly: true,
        domReadOnly: false,
        stickyScroll: { enabled: false },
        padding: { top: 6, bottom: 6 }
    });

    var diffEditor = null;   // made the first time a change is shown

    var state = {
        language: "javascript",
        readOnly: true,          // the document's own: on the web, or no edit rights
        streaming: false,        // Claude is writing it: read only until Claude finishes
        savedVersion: model.getAlternativeVersionId(),
        dirty: false,
        savedText: ""            // the status strip's "Saved 22:04", given by the panel
    };

    var LANGUAGE_NAMES = { javascript: "JavaScript", glsl: "GLSL" };

    function lineCount() {
        return model.getLineCount();
    }

    function showStatus() {
        var position = editor.getPosition();
        positionElement.textContent = position ? "Ln " + position.lineNumber + ", Col " + position.column : "";
        languageElement.textContent = LANGUAGE_NAMES[state.language] || "";
        var lines = lineCount();
        linesElement.textContent = lines + (lines === 1 ? " line" : " lines");
        if (state.streaming) {
            savedElement.textContent = "Claude is writing…";
            savedElement.className = "";
        } else if (state.dirty) {
            savedElement.textContent = "Changed, not saved";
            savedElement.className = "changed";
        } else {
            savedElement.textContent = state.savedText;
            savedElement.className = "";
        }
    }

    function applyReadOnly() {
        editor.updateOptions({ readOnly: state.readOnly || state.streaming });
    }

    function markClean() {
        state.savedVersion = model.getAlternativeVersionId();
        if (state.dirty) {
            state.dirty = false;
            post({ type: "changed", dirty: false });
        }
    }

    model.onDidChangeContent(function () {
        var dirty = model.getAlternativeVersionId() !== state.savedVersion;
        if (dirty !== state.dirty && !state.streaming) {
            state.dirty = dirty;
            post({ type: "changed", dirty: dirty });
        }
        showStatus();
    });
    editor.onDidChangeCursorPosition(showStatus);
    editor.onDidLayoutChange(function (layout) {
        var wide = layout.width >= MINIMAP_WIDTH;
        if (wide !== editor.getOption(monaco.editor.EditorOption.minimap).enabled) {
            editor.updateOptions({ minimap: { enabled: wide } });
        }
    });

    // Cmd+S (Ctrl+S on Windows and Linux): the panel saves, if the person may
    editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, function () {
        if (!state.readOnly && !state.streaming) {
            post({ type: "save" });
        }
    });
    // Esc with nothing of the editor's own to close: the world gets the keyboard back
    editor.addCommand(monaco.KeyCode.Escape, function () {
        post({ type: "escape" });
    }, "!suggestWidgetVisible && !findWidgetVisible && !parameterHintsVisible && !renameInputVisible && " +
        "!inSnippetMode && !markersNavigationVisible && !editorHasMultipleSelections && !accessibilityHelpWidgetVisible");

    function text(value) {
        return typeof value === "string" ? value.slice(0, MAX_TEXT) : "";
    }

    function showError(line, message) {
        var lines = lineCount();
        var at = Math.max(1, Math.min(lines, Math.floor(Number(line) || 0)));
        var shown = text(message).split("\n")[0].slice(0, 400);
        if (Number(line) >= 1) {
            monaco.editor.setModelMarkers(model, "darkride", [{
                severity: monaco.MarkerSeverity.Error,
                message: shown,   // a plain string: Monaco shows it as text
                startLineNumber: at,
                startColumn: 1,
                endLineNumber: at,
                endColumn: model.getLineMaxColumn(at)
            }]);
        } else {
            monaco.editor.setModelMarkers(model, "darkride", []);
        }
        errorElement.textContent = Number(line) >= 1 ? "Line " + at + ": " + shown : shown;
        errorElement.hidden = shown === "";
    }

    function clearError() {
        monaco.editor.setModelMarkers(model, "darkride", []);
        errorElement.textContent = "";
        errorElement.hidden = true;
    }

    function showEditor() {
        if (!diffElement.hidden) {
            diffElement.hidden = true;
            editorElement.hidden = false;
            editor.layout();
        }
    }

    // ---- the panel's fixed calls ----

    window.darkrideCode = {
        // a script to show: { language: "javascript" | "glsl", readOnly, text, savedText, errorLine, error }, and
        // draft: the person's unsaved change from before the panel was closed, shown as changed on top of text
        open: function (doc) {
            doc = doc && typeof doc === "object" ? doc : {};
            showEditor();
            state.streaming = false;
            state.language = doc.language === "glsl" ? "glsl" : "javascript";
            state.readOnly = doc.readOnly !== false;
            state.savedText = text(doc.savedText);
            monaco.editor.setModelLanguage(model, state.language);
            model.setValue(text(doc.text));
            editor.setScrollTop(0);
            editor.setPosition({ lineNumber: 1, column: 1 });
            markClean();
            if (typeof doc.draft === "string" && doc.draft !== model.getValue()) {
                model.pushEditOperations([], [{ range: model.getFullModelRange(), text: text(doc.draft) }],
                    function () { return null; });
            }
            applyReadOnly();
            if (doc.error) {
                showError(doc.errorLine, doc.error);
            } else {
                clearError();
            }
            showStatus();
        },
        // nothing open
        clear: function () {
            this.open({ text: "", readOnly: true });
        },
        // edit rights changed while open
        setReadOnly: function (readOnly) {
            state.readOnly = readOnly !== false;
            applyReadOnly();
        },
        // the script stopped with an error (the Inspector's status): its first line, at the line it points to
        showError: function (line, message) {
            showError(line, message);
        },
        clearError: clearError,
        // the panel saved it: "Saved 22:04"
        saved: function (savedText) {
            state.savedText = text(savedText);
            markClean();
            showStatus();
        },
        // the person's text, for the panel's Save
        text: function () {
            return model.getValue();
        },
        isDirty: function () {
            return state.dirty;
        },
        // Claude writing this script: its text arriving, read only until Claude finishes
        streamBegin: function () {
            showEditor();
            state.streaming = true;
            applyReadOnly();
            model.setValue("");
            clearError();
            showStatus();
        },
        streamText: function (chunk) {
            if (!state.streaming) {
                return;
            }
            var piece = text(chunk);
            if (piece === "" || model.getValueLength() + piece.length > MAX_TEXT) {
                return;
            }
            var lastLine = lineCount();
            var lastColumn = model.getLineMaxColumn(lastLine);
            model.applyEdits([{
                range: new monaco.Range(lastLine, lastColumn, lastLine, lastColumn),
                text: piece
            }]);
            editor.revealLine(lineCount());
        },
        streamEnd: function (finalText) {
            if (typeof finalText === "string") {
                model.setValue(text(finalText));
            }
            state.streaming = false;
            markClean();
            applyReadOnly();
            showStatus();
        },
        // what Claude changed: old and new side by side, read only; Keep and Undo stay on Make's card
        showDiff: function (oldText, newText) {
            if (!diffEditor) {
                diffEditor = monaco.editor.createDiffEditor(diffElement, {
                    theme: "darkride",
                    automaticLayout: true,
                    readOnly: true,
                    originalEditable: false,
                    renderSideBySide: true,
                    useInlineViewWhenSpaceIsLimited: true,
                    fontFamily: "\"SF Mono\", Menlo, Consolas, \"DejaVu Sans Mono\", monospace",
                    fontSize: 12,
                    lineHeight: 18,
                    links: false,
                    contextmenu: false,
                    minimap: { enabled: false },
                    scrollBeyondLastLine: false
                });
            }
            var old = diffEditor.getModel();
            diffEditor.setModel({
                original: monaco.editor.createModel(text(oldText), state.language),
                modified: monaco.editor.createModel(text(newText), state.language)
            });
            if (old) {
                old.original.dispose();
                old.modified.dispose();
            }
            editorElement.hidden = true;
            diffElement.hidden = false;
            diffEditor.layout();
        },
        closeDiff: showEditor,
        focus: function () {
            editor.focus();
        },
        // the events since the last call, as JSON text
        takeEvents: function () {
            var taken = events;
            events = [];
            return JSON.stringify(taken);
        }
    };

    showStatus();
    post({ type: "ready" });
}());
