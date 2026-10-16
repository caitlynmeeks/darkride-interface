//
//  OpenXrProvider.cpp
//  plugins/openxr-plugin/src
//
//  Created on 2026-10-16 for Quest 3 hand tracking over Link (Meta's PC OpenXR runtime).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <mutex>

#include <QtCore/QObject>
#include <QtCore/QtPlugin>
#include <QtCore/QStringList>

#include <plugins/RuntimePlugin.h>

#include "OpenXrDisplayPlugin.h"
#include "OpenXrInputPlugin.h"

class OpenXrProvider : public QObject, public DisplayProvider, InputProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DisplayProvider_iid FILE "plugin.json")
    Q_INTERFACES(DisplayProvider)
    Q_PLUGIN_METADATA(IID InputProvider_iid FILE "plugin.json")
    Q_INTERFACES(InputProvider)

public:
    OpenXrProvider(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~OpenXrProvider() {}

    virtual DisplayPluginList getDisplayPlugins() override {
        static std::once_flag once;
        std::call_once(once, [&] {
            DisplayPluginPointer plugin(new OpenXrDisplayPlugin(getRuntime()));
            if (plugin->isSupported()) {
                _displayPlugins.push_back(plugin);
            }
        });
        return _displayPlugins;
    }

    virtual InputPluginList getInputPlugins() override {
        static std::once_flag once;
        std::call_once(once, [&] {
            InputPluginPointer plugin(new OpenXrInputPlugin(getRuntime()));
            if (plugin->isSupported()) {
                _inputPlugins.push_back(plugin);
            }
        });
        return _inputPlugins;
    }

    virtual void destroyInputPlugins() override {
        _inputPlugins.clear();
    }

    virtual void destroyDisplayPlugins() override {
        _displayPlugins.clear();
    }

private:
    // made on first use, by whichever of the display and input lists is asked for first
    std::shared_ptr<OpenXrRuntime> getRuntime() {
        std::call_once(_runtimeOnce, [&] { _runtime = std::make_shared<OpenXrRuntime>(); });
        return _runtime;
    }

    std::once_flag _runtimeOnce;
    std::shared_ptr<OpenXrRuntime> _runtime;
    DisplayPluginList _displayPlugins;
    InputPluginList _inputPlugins;
};

#include "OpenXrProvider.moc"
