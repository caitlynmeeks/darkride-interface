//
//  qtscript-out-check.js
//  tests-manual/wasm-scripting
//
//  Created by scripting on 2026-11-03.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  A test script (--testScript) for taking QtScript out: every script runs on the WebAssembly engine, the MIDI
//  plugin's global reaches a client script there, and the scripting console's engine starts (empty) when the console
//  opens. Prints QTSCRIPT-OUT lines; run with run-client.sh and read the log:
//    tests-manual/wasm-scripting/run-client.sh qtscript-out 40 --testScript "$PWD/tests-manual/wasm-scripting/qtscript-out-check.js"
//  PASS: "QTSCRIPT-OUT: typeof Midi object", "QTSCRIPT-OUT: Midi.listMidiDevices is function", "running scripts N,
//  any engine field: false", and at quit the console's engine among those waited for ("waiting on script:
//  \"about:console\""). What is typed into the console is a hand test (JavaScriptGuestTests::testConsoleLines covers
//  the guest's side).
//
(function() {
    function say(text) { print("QTSCRIPT-OUT: " + text); }
    say("typeof Midi " + typeof Midi);
    say("Midi.listMidiDevices is " + (typeof Midi === "object" && Midi ? typeof Midi.listMidiDevices : "absent"));
    var running = ScriptDiscoveryService.getRunning();
    say("running scripts " + running.length + ", any engine field: " + running.some(function(entry) { return entry.engine !== undefined; }));
    Script.setTimeout(function() {
        // the console's engine starts when the console opens (JSConsole::setScriptEngine)
        Menu.triggerOption("Console...");
        say("opened the console");
    }, 3000);
    Script.setTimeout(function() {
        Menu.triggerOption("Console...");
        say("stopping");
        Script.stop();
    }, 12000);
})();
