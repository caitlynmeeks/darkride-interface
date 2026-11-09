//
//  MidiPlugin.cpp
//  plugins/midi-plugin/src
//
//  The MIDI scripting interface as a runtime plugin: the plugin owns the one Midi object and puts it into every
//  script engine as the global "Midi", through the ScriptInitializers that the plugin loader's host provides. The
//  client loads it from its PlugIns folder; the servers never do, so their script engines have no Midi global.
//  Devices are scanned once, when the plugin loads: rescanning on hot-plug was disabled in the client years ago
//  because closing the interfaces of an unplugged device can deadlock the audio driver on Windows (Microsoft
//  KB 4460006), and that stays so here.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE or https://opensource.org/licenses/MIT
//

#include <QtCore/QObject>
#include <QtCore/QLoggingCategory>

#include <DependencyManager.h>
#include <shared/ScriptInitializerMixin.h>

#include "Midi.h"

namespace darkride_midi {

Q_LOGGING_CATEGORY(logger, "darkride.midi")

class MidiPlugin : public QObject {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.darkride.plugins.midi" FILE "plugin.json")

public:
    MidiPlugin() {
        auto initializers = DependencyManager::get<ScriptInitializers>();
        if (!initializers) {
            qCWarning(logger) << "No script initializers in this host: scripts get no Midi object";
            return;
        }
        // through each engine's own registerGlobalObject: both engines, and a world's scripts get the policy's stand-in
        initializers->registerScriptInitializer([this](const ScriptGlobalObjectAdder& addGlobalObject) {
            addGlobalObject("Midi", &_midi);
        });
    }

private:
    Midi _midi;
};

}  // namespace darkride_midi

#include "MidiPlugin.moc"
