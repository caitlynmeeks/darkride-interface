//
//  NewScriptTemplates.h
//  interface/src/ui
//
//  The starter scripts New Script opens in the Code panel (ui/CodeEditor.h), one for a thing's script and one for its
//  server script. Written by scripting (11-08) to follow Make's guide, so what Claude writes and what a person starts
//  from look alike. The engine evaluates the file as a program and takes its last value, so the leading comments are
//  fine.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_NewScriptTemplates_h
#define hifi_NewScriptTemplates_h

namespace new_script_templates {

// the thing's script: on each visitor's computer
static const char* const ENTITY_SCRIPT = R"JS(// This thing's script. It runs on each visitor's computer, separately for each of them:
// use it for what one visitor does or sees. To change the thing for everyone, ask its
// server script (Entities.callEntityServerMethod, below).
(function () {
    var id;

    // When the thing loads on this visitor's computer.
    this.preload = function (entityID) {
        id = entityID;
    };

    // When this visitor clicks the thing.
    this.clickDownOnEntity = function (entityID, event) {
        if (event.isPrimaryButton === false) {
            return;
        }
        print("Clicked");   // goes to the log
        // Entities.callEntityServerMethod(id, "clicked", []);   // tell the server script
    };

    // When the thing is removed or this script changes: stop timers and listeners here.
    this.unload = function () {
    };
})
)JS";

// the thing's server script: once, in the world's server
static const char* const SERVER_SCRIPT = R"JS(// This thing's server script. It runs once, in the world's server: use it for what
// everyone must agree on (a door open or shut, a score). Changes it makes with
// Entities.editEntity reach every visitor.
(function () {
    var id;
    this.remotelyCallable = ["clicked"];   // the only methods visitors' scripts may call

    // When the world's server loads the thing.
    this.preload = function (entityID) {
        id = entityID;
    };

    // Called from the thing's script: Entities.callEntityServerMethod(id, "clicked", [])
    this.clicked = function (entityID, params) {
        print("Clicked by " + Script.remoteCallerID);   // the visitor who asked
    };

    // When the thing is removed or this script changes: stop timers here.
    this.unload = function () {
    };
})
)JS";

}

#endif // hifi_NewScriptTemplates_h
