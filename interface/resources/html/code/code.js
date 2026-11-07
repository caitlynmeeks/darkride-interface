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

    // Build mode's greys (interface/resources/styles/editor.qss): the editor on the Inspector's field grey, Build
    // mode's selection blue, soft syntax colours (Caity 11-07: "ok as drawn", mockups/code-panel)
    monaco.editor.defineTheme("darkride", {
        base: "vs-dark",
        inherit: true,
        rules: [
            { token: "", foreground: "d2d2d2" },
            { token: "keyword", foreground: "8fb6de" },
            { token: "string", foreground: "c9a36e" },
            { token: "string.escape", foreground: "b9c79a" },
            { token: "number", foreground: "a8c79a" },
            { token: "comment", foreground: "7b7b7b", fontStyle: "italic" },
            { token: "regexp", foreground: "c9a36e" },
            { token: "type", foreground: "d9cf9c" },
            { token: "type.identifier", foreground: "d9cf9c" },
            { token: "predefined", foreground: "d9cf9c" },
            { token: "delimiter", foreground: "a6a6a6" }
        ],
        colors: {
            "editor.background": "#2a2a2a",
            "editor.foreground": "#d2d2d2",
            "editor.selectionBackground": "#2c5d87aa",
            "editor.inactiveSelectionBackground": "#2c5d8755",
            "editor.wordHighlightBackground": "#2c5d8766",
            "editor.wordHighlightStrongBackground": "#2c5d8788",
            "editor.lineHighlightBackground": "#303030",
            "editor.lineHighlightBorder": "#00000000",
            "editorCursor.foreground": "#e0e0e0",
            "editorLineNumber.foreground": "#666666",
            "editorLineNumber.activeForeground": "#b4b4b4",
            "editorIndentGuide.background1": "#3a3a3a",
            "editorIndentGuide.activeBackground1": "#4a4a4a",
            "editorBracketMatch.background": "#2c5d8755",
            "editorBracketMatch.border": "#00000000",
            // brackets in the delimiters' grey at every depth (a model's own colouring would otherwise draw them in
            // gold, orchid and blue)
            "editorBracketHighlight.foreground1": "#a6a6a6",
            "editorBracketHighlight.foreground2": "#a6a6a6",
            "editorBracketHighlight.foreground3": "#a6a6a6",
            "editorBracketHighlight.foreground4": "#a6a6a6",
            "editorBracketHighlight.foreground5": "#a6a6a6",
            "editorBracketHighlight.foreground6": "#a6a6a6",
            "editorBracketHighlight.unexpectedBracket.foreground": "#ff6b6b",
            "editorError.foreground": "#ff6b6b",
            "editorWarning.foreground": "#d8a657",
            "editorWidget.background": "#383838",
            "editorWidget.border": "#232323",
            "editorHoverWidget.background": "#383838",
            "editorHoverWidget.border": "#232323",
            "editorSuggestWidget.background": "#383838",
            "editorSuggestWidget.border": "#232323",
            "editorSuggestWidget.selectedBackground": "#2c5d87",
            "minimap.background": "#2a2a2a",
            "scrollbarSlider.background": "#5a5a5a99",
            "scrollbarSlider.hoverBackground": "#5a5a5acc",
            "scrollbarSlider.activeBackground": "#6a6a6a",
            "diffEditor.insertedTextBackground": "#5fb76026",
            "diffEditor.removedTextBackground": "#ff6b6b30",
            "diffEditor.insertedLineBackground": "#5fb76014",
            "diffEditor.removedLineBackground": "#ff6b6b1a"
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
    var readOnlyElement = document.getElementById("readonly");
    var sizeElement = document.getElementById("size");
    var hashElement = document.getElementById("hash");

    // a model colours its brackets by depth unless told not to: the editor's own option doesn't reach it
    function plainModel(value, language) {
        var made = monaco.editor.createModel(value, language);
        made.updateOptions({ bracketColorizationOptions: { enabled: false, independentColorPoolPerBracketType: false } });
        return made;
    }
    var model = plainModel("", "javascript");
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
        bracketPairColorization: { enabled: false },   // brackets in one colour, not a rainbow
        guides: { bracketPairs: false },
        padding: { top: 6, bottom: 6 }
    });

    var diffEditor = null;   // made the first time a change is shown

    var state = {
        language: "javascript",
        readOnly: true,          // the document's own: on the web, or no edit rights
        streaming: false,        // Claude is writing it: read only until Claude finishes
        savedVersion: model.getAlternativeVersionId(),
        dirty: false,
        savedText: "",           // the status strip's "Saved 22:04", given by the panel
        note: "",                // a save's answer or why it's read only, from the panel (setNote)
        savedHash: "",           // a short hash of the text as opened or last saved: two versions told apart
        noteIsProblem: false
    };

    var LANGUAGE_NAMES = { javascript: "JavaScript", glsl: "GLSL" };

    // a short hash of a text, 12 hex digits (cyrb53: quick, not for security; it only tells two versions apart)
    function shortHash(value) {
        var h1 = 0xdeadbeef;
        var h2 = 0x41c6ce57;
        for (var i = 0; i < value.length; i++) {
            var c = value.charCodeAt(i);
            h1 = Math.imul(h1 ^ c, 2654435761);
            h2 = Math.imul(h2 ^ c, 1597334677);
        }
        h1 = Math.imul(h1 ^ (h1 >>> 16), 2246822507) ^ Math.imul(h2 ^ (h2 >>> 13), 3266489909);
        h2 = Math.imul(h2 ^ (h2 >>> 16), 2246822507) ^ Math.imul(h1 ^ (h1 >>> 13), 3266489909);
        var hex = (4294967296 * (2097151 & h2) + (h1 >>> 0)).toString(16);
        return ("00000000000000" + hex).slice(-14).slice(0, 12);
    }

    function sizeText(length) {
        return length >= 1048576 ? (length / 1048576).toFixed(1) + " MB" : (length / 1024).toFixed(1) + " KB";
    }

    function lineCount() {
        return model.getLineCount();
    }

    function showStatus() {
        readOnlyElement.textContent = state.readOnly && !state.streaming ? "Read only" : "";
        var position = editor.getPosition();
        positionElement.textContent = position ? "Ln " + position.lineNumber + ", Col " + position.column : "";
        languageElement.textContent = LANGUAGE_NAMES[state.language] || "";
        var lines = lineCount();
        linesElement.textContent = lines + (lines === 1 ? " line" : " lines");
        sizeElement.textContent = sizeText(model.getValueLength());
        // the saved version's: hidden while the text shown is a change of it
        hashElement.textContent = state.savedHash !== "" && !state.dirty && !state.streaming ? "hash " + state.savedHash : "";
        if (state.streaming) {
            savedElement.textContent = "Claude is writing…";
            savedElement.className = "";
        } else if (state.note !== "") {
            savedElement.textContent = state.note;
            savedElement.className = state.noteIsProblem ? "problem" : "";
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
        // the panel's message for the status strip's right end (a save's answer, why it's read only), as plain text;
        // red when it's a problem
        setNote: function (text, problem) {
            state.note = typeof text === "string" ? text.slice(0, 300) : "";
            state.noteIsProblem = problem === true;
            showStatus();
        },
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
            state.savedHash = shortHash(model.getValue());
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
            showStatus();
        },
        // the script stopped with an error (the Inspector's status): its first line, at the line it points to
        showError: function (line, message) {
            showError(line, message);
        },
        clearError: clearError,
        // the panel saved it: "Saved 22:04"
        saved: function (savedText) {
            state.savedText = text(savedText);
            state.savedHash = shortHash(model.getValue());
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
            state.savedHash = shortHash(model.getValue());   // what Claude wrote is what's in the world now
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
                original: plainModel(text(oldText), state.language),
                modified: plainModel(text(newText), state.language)
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
