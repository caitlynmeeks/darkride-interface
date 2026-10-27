//
//  Menu.cpp
//  interface/src
//
//  Created by Stephen Birarda on 8/12/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Menu.h"

#include <functional>
#include "ClientOptions.h"

#include <gpu/Context.h>

#include <Mirrors.h>
#include <QDesktopServices>
#include <QFileDialog>
#include <QMenuBar>
#include <QShortcut>
#include <QTimer>

#include <thread>

#include <AddressManager.h>
#include <AudioClient.h>
#include <CrashHelpers.h>
#include <DependencyManager.h>
#include <ui/TabletScriptingInterface.h>
#include <display-plugins/DisplayPlugin.h>
#include <PathUtils.h>
#include <SettingHandle.h>
#include <VrMenu.h>
#include <ScriptEngines.h>
#include <WasmScriptEngine.h>
#include <MenuItemProperties.h>
#include <OffscreenUi.h>
#include <ui/types/FileTypeProfile.h>
#include <ui/types/TivoliWebEngineProfile.h>

#include "Application.h"
#include "LODManager.h"
#include <RenderableModelEntityItem.h>
#include <ToneMapAndResampleTask.h>
#include "AccountManager.h"
#include "assets/ATPAssetMigrator.h"
#include "audio/AudioScope.h"
#include "avatar/AvatarManager.h"
#include "AvatarBookmarks.h"
#include "editor/Editor.h"
#include "editor/EditorCreate.h"
#include <QtCore/QHash>
#include "MainWindow.h"
#include "render/DrawStatus.h"
#include "scripting/MenuScriptingInterface.h"
#include "scripting/HMDScriptingInterface.h"
#include "ui/DialogsManager.h"
#include "chat/ChatWindow.h"
#include "ui/Panels.h"
#include "ui/Popup.h"
#include "ui/WindowLayout.h"
#include "ui/StandAloneJSConsole.h"
#include "InterfaceLogging.h"
#include "LocationBookmarks.h"
#include "DeferredLightingEffect.h"
#include "PickManager.h"
#include "RenderLocalShadowTask.h"

#include "scripting/SettingsScriptingInterface.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#endif

#include "MeshPartPayload.h"
#include "scripting/RenderScriptingInterface.h"

extern bool DEV_DECIMATE_TEXTURES;

Menu* Menu::getInstance() {
    // At quit a script thread can still ask for the menu after the application object is gone: there is none then.
    if (qApp == nullptr) return nullptr;
    MainWindow* window = qApp->getWindow();
    if (window == nullptr) return nullptr;
    QMenuBar* menuBar = window->menuBar();
    if (menuBar == nullptr) return nullptr;
    return dynamic_cast<Menu*>(menuBar);
}

const char* EXCLUSION_GROUP_KEY = "exclusionGroup";

// Edit > Cut, Copy and Paste run when their shortcut reaches the menu (or from the menu itself). While the 3D view has
// the keyboard and the overlay's focused item is a web page (the Avatar app's address, Explore's search) or a text
// field, that item does the edit itself: the made-up key press the menu used to post reached a web page as Option+V on
// macOS and typed a "√". Otherwise the key is posted to the application object, as before; posted to a widget it
// would come back to the menu's own shortcut, over and over.
static bool viewHasKeyboard() {
    QWidget* focusWidget = QApplication::focusWidget();
    return !focusWidget || focusWidget == qApp->getPrimaryWidget() || focusWidget == qApp->getWindow();
}

static void editOrPostKey(const char* action, Qt::Key key) {
    if (viewHasKeyboard() && DependencyManager::get<OffscreenUi>()->editFocusedItem(action)) {
        return;
    }
    QKeyEvent* keyEvent = new QKeyEvent(QEvent::KeyPress, key, Qt::ControlModifier);
    QCoreApplication::postEvent(QCoreApplication::instance(), keyEvent);
}

Menu::Menu() {
    auto dialogsManager = DependencyManager::get<DialogsManager>();
    auto accountManager = DependencyManager::get<AccountManager>();

    // File/Application menu ----------------------------------
    MenuWrapper* fileMenu = addMenu("File");

    // File > Login menu items
    if (!ClientOptions::get().hasTokens) {
        addActionToQMenuAndActionHash(fileMenu, MenuOption::Login);

        // connect to the appropriate signal of the AccountManager so that we can change the Login/Logout menu item
        connect(accountManager.data(), &AccountManager::profileChanged,
                dialogsManager.data(), &DialogsManager::toggleLoginDialog);
        connect(accountManager.data(), &AccountManager::logoutComplete,
                dialogsManager.data(), &DialogsManager::toggleLoginDialog);

        // The signals above only fire after a profile is loaded or a logout completes, so prime the menu item
        // so its triggered signal is connected: otherwise File > Login does nothing on a fresh start. Deferred
        // because Menu::getInstance() is null until construction finishes.
        QTimer::singleShot(0, dialogsManager.data(), &DialogsManager::toggleLoginDialog);
    }

    // File > Import Avatar: wear an .fst, or auto rig a character file (AUTO-RIGGING.md)
    addActionToQMenuAndActionHash(fileMenu, MenuOption::ImportAvatar, 0, qApp, SLOT(importAvatarDialog()));

    // File > Put Avatar in My Files: the worn avatar, when it is on this computer, goes up so everyone sees it
    addActionToQMenuAndActionHash(fileMenu, MenuOption::PutAvatarInMyFiles, 0, qApp, SLOT(putAvatarInMyFiles()));

    // File > Quit
    addActionToQMenuAndActionHash(fileMenu, MenuOption::Quit, Qt::CTRL | Qt::Key_Q, qApp, SLOT(quit()), QAction::QuitRole);


    // Edit menu ----------------------------------
    MenuWrapper* editMenu = addMenu("Edit");

    // Edit > Cut
    auto cutAction = addActionToQMenuAndActionHash(editMenu, "Cut", QKeySequence::Cut);
    connect(cutAction, &QAction::triggered, [] {
            editOrPostKey("Cut", Qt::Key_X);
    });

    // Edit > Copy
    auto copyAction = addActionToQMenuAndActionHash(editMenu, "Copy", QKeySequence::Copy);
    connect(copyAction, &QAction::triggered, [] {
            editOrPostKey("Copy", Qt::Key_C);
    });

    // Edit > Paste
    auto pasteAction = addActionToQMenuAndActionHash(editMenu, "Paste", QKeySequence::Paste);
    connect(pasteAction, &QAction::triggered, [] {
            editOrPostKey("Paste", Qt::Key_V);
    });

    // Edit > Delete
    auto deleteAction = addActionToQMenuAndActionHash(editMenu, "Delete", QKeySequence::Delete);
    connect(deleteAction, &QAction::triggered, [] {
            QKeyEvent* keyEvent = new QKeyEvent(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
            QCoreApplication::postEvent(QCoreApplication::instance(), keyEvent);
    });

    editMenu->addSeparator();

    // Edit > Running Scripts
    auto action = addActionToQMenuAndActionHash(editMenu, MenuOption::RunningScripts, Qt::CTRL | Qt::Key_J);
    connect(action, &QAction::triggered, [] {
        if (!qApp->getLoginDialogPoppedUp()) {
            static const QUrl widgetUrl("hifi/dialogs/RunningScripts.qml");
            static const QUrl tabletUrl("hifi/dialogs/TabletRunningScripts.qml");
            static const QString name("RunningScripts");
            qApp->showDialog(widgetUrl, tabletUrl, name);
        }
    });

    editMenu->addSeparator();

    // Edit > Asset Browser
    auto assetServerAction = addActionToQMenuAndActionHash(editMenu, MenuOption::AssetServer,
                                                           Qt::CTRL | Qt::SHIFT | Qt::Key_A,
                                                           qApp, SLOT(showAssetServerWidget()));
    {
        auto nodeList = DependencyManager::get<NodeList>();
        QObject::connect(nodeList.data(), &NodeList::canWriteAssetsChanged, assetServerAction, &QAction::setEnabled);
        assetServerAction->setEnabled(nodeList->getThisNodeCanWriteAssets());
    }
    // Edit > Reload All Content and clear caches
    addActionToQMenuAndActionHash(editMenu, MenuOption::ReloadContent, 0, qApp, SLOT(reloadResourceCaches()));    

    // Edit > Reconnect to World 
    auto reconnectAction = addActionToQMenuAndActionHash(editMenu, MenuOption::Reconnect, Qt::CTRL | Qt::SHIFT | Qt::Key_R);
    connect(reconnectAction, &QAction::triggered, [] {
        DependencyManager::get<AddressManager>()->reconnect();
    });

    // Edit > Reload Avatar

    auto reloadAvatarAction = addActionToQMenuAndActionHash(editMenu, MenuOption::ReloadAvatar);
    connect(reloadAvatarAction, &QAction::triggered, [] {
        auto myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
        myAvatar->resetFullAvatarURL();
        myAvatar->markIdentityDataChanged();
        myAvatar->resetLastSent();
        myAvatar->sendAvatarDataPacket(true);
    });
    // Display menu ----------------------------------
    // FIXME - this is not yet matching Alan's spec because it doesn't have
    // menus for "2D"/"3D" - we need to add support for detecting the appropriate
    // default 3D display mode
    addMenu(DisplayPlugin::MENU_PATH());
    MenuWrapper* displayModeMenu = addMenu(MenuOption::OutputMenu);
    QActionGroup* displayModeGroup = new QActionGroup(displayModeMenu);
    displayModeGroup->setExclusive(true);


    // View menu ----------------------------------
    MenuWrapper* viewMenu = addMenu("View");
    QActionGroup* cameraModeGroup = new QActionGroup(viewMenu);

    // View > [camera group]
    cameraModeGroup->setExclusive(true);

    // View > First Person
    auto firstPersonAction = cameraModeGroup->addAction(
        addCheckableActionToQMenuAndActionHash(
            viewMenu, MenuOption::FirstPersonLookAt, Qt::Key_1,
            false, qApp, SLOT(cameraMenuChanged())
        )
    );

    firstPersonAction->setProperty(EXCLUSION_GROUP_KEY, QVariant::fromValue(cameraModeGroup));

    // View > Look At
    auto lookAtAction = cameraModeGroup->addAction(
        addCheckableActionToQMenuAndActionHash(
            viewMenu, MenuOption::LookAtCamera, Qt::Key_3,
            true, qApp, SLOT(cameraMenuChanged())
        )
    );

    lookAtAction->setProperty(EXCLUSION_GROUP_KEY, QVariant::fromValue(cameraModeGroup));

    // View > Selfie
    auto selfieAction = cameraModeGroup->addAction(
        addCheckableActionToQMenuAndActionHash(
            viewMenu, MenuOption::SelfieCamera, Qt::Key_2,
            false, qApp, SLOT(cameraMenuChanged())
        )
    );

    selfieAction->setProperty(EXCLUSION_GROUP_KEY, QVariant::fromValue(cameraModeGroup));

    viewMenu->addSeparator();

    // View > Center Player In View
    addCheckableActionToQMenuAndActionHash(viewMenu, MenuOption::CenterPlayerInView,
        0, true, qApp, SLOT(rotationModeChanged()));

    // View > Camera Collision: the third-person camera stops short of walls instead of going through them
    addCheckableActionToQMenuAndActionHash(viewMenu, MenuOption::CameraCollision, 0, true);

    // View > Enter First Person Mode in HMD
    addCheckableActionToQMenuAndActionHash(viewMenu, MenuOption::FirstPersonHMD, 0, true);

    viewMenu->addSeparator();

    // View > Panels: every panel of the window, ticked while open, wherever it is (ui/Panels.h). Choosing one opens it
    // or closes it; chat's keeps its shortcut and its own toggle, which brings it in front first and puts the keyboard
    // in its message box (chat/ChatWindow.h)
    {
        auto panels = DependencyManager::get<Panels>();
        MenuWrapper* panelsMenu = viewMenu->addMenu("Panels");
        QHash<QString, QAction*> panelActions;
        QString previous;
        for (const QString& name : panels->names()) {
            auto panelAction = new QAction(name, this);
            panelAction->setCheckable(true);
            panelAction->setChecked(panels->isOpen(name));
            // the saved layout opens the panels, after sign-in; until then there are none (as there's no bar). Build
            // mode's own, only while it's on
            panelAction->setProperty(TICK_NOT_SAVED, true);
            panelAction->setEnabled(false);
            if (Panels::isBuildModeOnly(name)) {
                connect(DependencyManager::get<Editor>().data(), &Editor::activeChanged, panelAction,
                        [panelAction](bool active) { panelAction->setEnabled(active); });
            } else {
                connect(WindowLayout::instance(), &WindowLayout::restored, panelAction, [panelAction] {
                    panelAction->setEnabled(true);
                });
            }
            // My Files after the bar's panels, then Build mode's
            if (name == "My Files" || (Panels::isBuildModeOnly(name) && !Panels::isBuildModeOnly(previous))) {
                panelsMenu->addSeparator();
            }
            previous = name;
            if (name == "Chat") {
                panelAction->setShortcut(ChatWindow::shortcut());
            }
            // the tick is the panel's: chat's toggle may only bring it in front
            connect(panelAction, &QAction::triggered, panels.data(), [panels, name, panelAction](bool checked) {
                if (name == "Chat") {
                    panels->toggle(name);
                } else if (checked) {
                    panels->open(name);
                } else {
                    panels->close(name);
                }
                panelAction->setChecked(panels->isOpen(name));
            });
            panelsMenu->addAction(panelAction);
            panelActions[name] = panelAction;
        }
        connect(panels.data(), &Panels::openChanged, this, [panelActions](const QString& name, bool open) {
            if (QAction* panelAction = panelActions.value(name)) {
                panelAction->setChecked(open);
            }
        });
    }

    // View > Layouts: named arrangements of the panels, normal play's and Build mode's apart (ui/WindowLayout.h), and
    // Reset to Default; listed as the menu opens
    {
        auto layoutsMenu = new QMenu("Layouts", this);
        connect(layoutsMenu, &QMenu::aboutToShow, layoutsMenu, [layoutsMenu] {
            layoutsMenu->clear();
            auto layout = WindowLayout::instance();
            const bool build = layout->isBuildMode();
            layoutsMenu->addAction("Save Layout…", [build] {
                const popup::Field field { "Streaming", QString() };
                popup::Alert alert(popup::appIcon(), "Save Layout",
                                   build ? "A name for Build mode's panels as they are now."
                                         : "A name for the panels as they are now.",
                                   QString(), { { "Cancel", popup::Role::Cancel }, { "Save", popup::Role::Default } },
                                   qApp->getWindow(), &field);
                alert.exec();
                if (alert.answer() == 1) {
                    WindowLayout::instance()->saveAs(alert.fieldText());
                }
            });
            const QStringList names = layout->savedNames();
            if (!names.isEmpty()) {
                layoutsMenu->addSeparator();
                for (const QString& name : names) {
                    layoutsMenu->addAction(name, [name] { WindowLayout::instance()->apply(name); });
                }
                QMenu* forget = layoutsMenu->addMenu("Delete Layout");
                for (const QString& name : names) {
                    forget->addAction(name, [name] {
                        popup::Alert alert(popup::appIcon(), "Delete the layout \"" + name + "\"?", QString(),
                                           QString(), { { "Cancel", popup::Role::Cancel },
                                                        { "Delete", popup::Role::Destructive } },
                                           qApp->getWindow());
                        alert.exec();
                        if (alert.answer() == 1) {
                            WindowLayout::instance()->forget(name);
                        }
                    });
                }
            }
            layoutsMenu->addSeparator();
            layoutsMenu->addAction("Reset to Default", [] { WindowLayout::instance()->resetToDefault(); });
        });
        // nothing to arrange before sign-in, as with View > Panels
        layoutsMenu->menuAction()->setEnabled(false);
        connect(WindowLayout::instance(), &WindowLayout::restored, layoutsMenu,
                [layoutsMenu] { layoutsMenu->menuAction()->setEnabled(true); });
        viewMenu->addAction(layoutsMenu->menuAction());
    }

    // Build menu ----------------------------------
    MenuWrapper* buildMenu = addMenu("Build");

    // Build > Build Mode
    QAction* buildModeAction = addCheckableActionToQMenuAndActionHash(buildMenu, MenuOption::BuildMode,
        [](bool checked) { DependencyManager::get<Editor>()->setActive(checked); },
        Qt::CTRL | Qt::Key_B, false);
    // scripts hear it as Menu.menuItemEvent("Build Mode") whichever way build mode changes (this item, Ctrl+B, the bar's
    // Create button, or the editor leaving by itself: Editor::setActive unchecks the item through setIsOptionChecked,
    // which triggers it); only items scripts add were forwarded before
    connect(buildModeAction, SIGNAL(triggered()), MenuScriptingInterface::getInstance(), SLOT(menuItemTriggered()));

    // Build > Show Grid
    addCheckableActionToQMenuAndActionHash(buildMenu, MenuOption::BuildGrid,
        [](bool checked) { DependencyManager::get<Editor>()->setGridVisible(checked); },
        0, true);

    buildMenu->addSeparator();

    // Build > Create, Undo, Redo, ...: only while build mode is on (the editor enables them)
    {
        auto editor = DependencyManager::get<Editor>();

        // Build > Create: every entity kind in the editor's catalogue, grouped as it says
        MenuWrapper* createMenu = buildMenu->addMenu("Create");
        QHash<QString, MenuWrapper*> groups;
        // a group's submenu, made the first time it's named, inside its parent's ("Special items/Vehicles")
        std::function<MenuWrapper*(const QString&, bool)> groupMenu = [&](const QString& path, bool separator) {
            if (!groups.contains(path)) {
                int slash = path.lastIndexOf('/');
                MenuWrapper* parent = slash < 0 ? createMenu : groupMenu(path.left(slash), separator);
                if (slash < 0 && separator) {
                    createMenu->addSeparator();
                }
                groups[path] = parent->addMenu(path.mid(slash + 1));
            }
            return groups[path];
        };
        for (const auto& item : EditorCreate::items()) {
            MenuWrapper* target = createMenu;
            if (!item.group.isEmpty()) {
                target = groupMenu(item.group, item.separatorBefore);
            } else if (item.separatorBefore) {
                createMenu->addSeparator();
            }
            QAction* action = addActionToQMenuAndActionHash(target, item.label, 0, editor.data(), SLOT(createFromMenu()));
            action->setData(item.key);
            action->setToolTip(item.tooltip);
            if (item.disabled) {
                action->setEnabled(false);   // a kind the engine can't make yet: never enabled with build mode
            } else {
                editor->addMenuAction(action);
            }
        }
        buildMenu->addSeparator();

        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildUndo, Qt::CTRL | Qt::Key_Z,
            editor.data(), SLOT(undo())));
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildRedo, Qt::CTRL | Qt::SHIFT | Qt::Key_Z,
            editor.data(), SLOT(redo())));
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildDuplicate, Qt::CTRL | Qt::Key_D,
            editor.data(), SLOT(duplicateSelection())));
        buildMenu->addSeparator();
        // F, Shift+F and Option+Shift+F in the viewport and the Hierarchy; no global shortcuts, so
        // typing in chat never fires them
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildFrame, 0,
            editor.data(), SLOT(frameSelection())));
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildGoTo, 0,
            editor.data(), SLOT(goToSelection())));
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildBringHere, 0,
            editor.data(), SLOT(bringSelectionHere())));
        buildMenu->addSeparator();
        // the same as View > Layouts > Reset to Default, in Build mode
        QAction* resetPanels = addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildResetPanels, 0);
        connect(resetPanels, &QAction::triggered, [] { WindowLayout::instance()->resetToDefault(); });
        editor->addMenuAction(resetPanels);
        buildMenu->addSeparator();
        // Build > Import Entities…, Import Entities from URL…, Export Selected Entities…: entities files, as the old
        // Create app's Edit menu had them
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildImportEntities, 0,
            editor.data(), SLOT(importEntitiesFromFile())));
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildImportEntitiesFromUrl, 0,
            editor.data(), SLOT(importEntitiesFromUrl())));
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildExportEntities, 0,
            editor.data(), SLOT(exportSelection())));
        buildMenu->addSeparator();
        // Build > Use Stable Links: the world's links to your files by path become their stable links (NATIVE-EDITOR.md 7.4)
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildUseStableLinks, 0,
            editor.data(), SLOT(useStableLinks())));
        // Build > Bake Impostor: the selected entities' replacement cards (IMPOSTORS.md 2.2); Bake All Impostors:
        // every prop of the world without one; Remove Impostor: the selected entities' entries
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildBakeImpostor, 0,
            editor.data(), SLOT(bakeImpostor())));
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildBakeAllImpostors, 0,
            editor.data(), SLOT(bakeAllImpostors())));
        editor->addMenuAction(addActionToQMenuAndActionHash(buildMenu, MenuOption::BuildRemoveImpostor, 0,
            editor.data(), SLOT(removeImpostor())));
    }

    //TODO: Remove Navigation menu when these functions are included in GoTo menu
    // Navigate menu ----------------------------------
    MenuWrapper* navigateMenu = addMenu("Navigate");

    // Navigate > LocationBookmarks related menus -- Note: the LocationBookmarks class adds its own submenus here.
    auto locationBookmarks = DependencyManager::get<LocationBookmarks>();
    locationBookmarks->setupMenus(this, navigateMenu);

    // Navigate > Copy Address
    auto addressManager = DependencyManager::get<AddressManager>();
    addActionToQMenuAndActionHash(navigateMenu, MenuOption::CopyAddress, 0,
        addressManager.data(), SLOT(copyAddress()));

    // Navigate > Copy Path
    addActionToQMenuAndActionHash(navigateMenu, MenuOption::CopyPath, 0,
        addressManager.data(), SLOT(copyPath()));

    // Navigate > Start-up Location
    MenuWrapper* startupLocationMenu = navigateMenu->addMenu(MenuOption::StartUpLocation);
    QActionGroup* startupLocatiopnGroup = new QActionGroup(startupLocationMenu);
    startupLocatiopnGroup->setExclusive(true);
    startupLocatiopnGroup->addAction(addCheckableActionToQMenuAndActionHash(startupLocationMenu, MenuOption::HomeLocation, 0, 
        false));
    startupLocatiopnGroup->addAction(addCheckableActionToQMenuAndActionHash(startupLocationMenu, MenuOption::LastLocation, 0, 
        true));

    // Navigate > Walk Through Walls: your avatar not colliding with the world (noclip; Caity, 10-25: it belongs where the
    // settings are, with a key). The same switch as MyAvatar.collisionsEnabled, the Avatar panel's "Collide with the
    // world" and Settings > General's: the item follows it whichever way it changes
    navigateMenu->addSeparator();
    {
        auto myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
        QAction* walkThroughWalls = addCheckableActionToQMenuAndActionHash(navigateMenu, MenuOption::WalkThroughWalls,
            [](bool checked) {
                DependencyManager::get<AvatarManager>()->getMyAvatar()->setCollisionsEnabled(!checked);
            },
            Qt::CTRL | Qt::SHIFT | Qt::Key_N, myAvatar && !myAvatar->getCollisionsEnabled());
        if (myAvatar) {
            connect(myAvatar.get(), &MyAvatar::collisionsEnabledChanged, walkThroughWalls, [walkThroughWalls](bool enabled) {
                walkThroughWalls->setChecked(!enabled);
            });
        }
    }

    // Settings menu ----------------------------------
    MenuWrapper* settingsMenu = addMenu("Settings");

    // Settings > General...
    action = addActionToQMenuAndActionHash(settingsMenu, MenuOption::Preferences, Qt::CTRL | Qt::Key_G, nullptr, nullptr);
    connect(action, &QAction::triggered, [] {
        if (!qApp->getLoginDialogPoppedUp()) {
            qApp->showDialog(QString("hifi/dialogs/GeneralPreferencesDialog.qml"),
                QString("hifi/tablet/TabletGeneralPreferences.qml"), "GeneralPreferencesDialog");
        }
    });

    // Settings > Controls...
    action = addActionToQMenuAndActionHash(settingsMenu, "Controls...");
    connect(action, &QAction::triggered, [] {
            auto tablet = DependencyManager::get<TabletScriptingInterface>()->getTablet("com.highfidelity.interface.tablet.system");
            auto hmd = DependencyManager::get<HMDScriptingInterface>();
            tablet->pushOntoStack("hifi/tablet/ControllerSettings.qml");

            if (!hmd->getShouldShowTablet()) {
                hmd->toggleShouldShowTablet();
            }
    });

    // Settings > Audio...
    action = addActionToQMenuAndActionHash(settingsMenu, "Audio...");
    connect(action, &QAction::triggered, [] {
        static const QUrl tabletUrl("hifi/audio/Audio.qml");
        auto tablet = DependencyManager::get<TabletScriptingInterface>()->getTablet("com.highfidelity.interface.tablet.system");
        auto hmd = DependencyManager::get<HMDScriptingInterface>();
        tablet->pushOntoStack(tabletUrl);

        if (!hmd->getShouldShowTablet()) {
            hmd->toggleShouldShowTablet();
        }
    });

    // Settings > Developer Menu
    addCheckableActionToQMenuAndActionHash(settingsMenu, "Developer Menu", 0, false, this, SLOT(toggleDeveloperMenus()));
    

    //// Settings > Use shaders (procedural materials)
    //action = addCheckableActionToQMenuAndActionHash(settingsMenu, MenuOption::CustomShaders, 0, false);
    //connect(action, &QAction::triggered, [action] {

    //    TextureCache::setCustomShadersEnabled(action->isChecked());

    //    // Refresh entire scene if custom shaders were disabled in settings at startup
    //    if (TextureCache::wasLaunchedWithShadersDisabled()) {
    //        TextureCache::setWasLaunchedWithShadersDisabled(false);
    //        
    //    }
    //    else { // do a quick refresh of the shaders
    //        ShaderCache::instance().refreshAll(); // only refresh the shader cache
    //    }
    //});

    // Settings > Custom Shaders on Models
    action = addCheckableActionToQMenuAndActionHash(
        settingsMenu, MenuOption::MaterialProceduralShaders, 0,
        MeshPartPayload::DEFAULT_ENABLE_MATERIAL_PROCEDURAL_SHADERS
    );
    connect(action, &QAction::triggered, [action] {       
        MeshPartPayload::enableMaterialProceduralShaders = action->isChecked();
    });

    // Settings > Enable Flow / Dynamic Bone
    action = addCheckableActionToQMenuAndActionHash(settingsMenu, MenuOption::EnableFlow, 0, true);
    connect(action, &QAction::triggered, [action] {
        qDebug() << "Please reload to see changes to dynamic bone flow: " << action->isChecked();
        auto myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
        myAvatar->setFlowEnabled(action->isChecked());
        
    });


    // Settings > Disable Web Entities
    action = addCheckableActionToQMenuAndActionHash(settingsMenu, MenuOption::DisableWebEntities, 0, false);
    connect(action, &QAction::triggered, [action] {
        auto entityTreeRenderer = DependencyManager::get<EntityTreeRenderer>();
        entityTreeRenderer->setDisableWebEntities(action->isChecked());
    });

    // Settings > Use Avatar Placeholders
    action = addCheckableActionToQMenuAndActionHash(settingsMenu, MenuOption::UseAvatarPlaceholders, 0, false);
    connect(action, &QAction::triggered, [action] {
        auto avatarManager = DependencyManager::get<AvatarManager>();
        avatarManager->setUseAvatarPlaceholders(action->isChecked());
    });



    // Settings > Ask to Reset Settings
    addCheckableActionToQMenuAndActionHash(settingsMenu, MenuOption::AskToResetSettings, 0, false);


    // Developer menu ----------------------------------
    MenuWrapper* developerMenu = addMenu("Developer", "Developer");
    

    // Developer > Darkride Options >>>
    MenuWrapper* tivoliOptionsMenu = developerMenu->addMenu("Darkride Options");

    addCheckableActionToQMenuAndActionHash(tivoliOptionsMenu, MenuOption::StopRendering);

    action = addCheckableActionToQMenuAndActionHash(tivoliOptionsMenu, MenuOption::LoadCompleteEntityTree, 0,
                                                    qApp->getLoadCompleteEntityTreeSetting());
 

    //// Developer > Darkride Options > Test Zone Culling (test)
    //addCheckableActionToQMenuAndActionHash(tivoliOptionsMenu, MenuOption::TestZoneCulling, 0, false);


    // Edit > Redraw Tree 
    addActionToQMenuAndActionHash(tivoliOptionsMenu, MenuOption::Redraw, Qt::CTRL | Qt::SHIFT | Qt::Key_T, qApp, SLOT(requeryOctree()));

    action = addCheckableActionToQMenuAndActionHash(tivoliOptionsMenu, MenuOption::BypassPrioritySorting, 0,
        qApp->getForcedBypassPrioritySorting());
    connect(action, &QAction::triggered, [action] { 
        qApp->setForcedBypassPrioritySorting(action->isChecked());
     });


    action = addCheckableActionToQMenuAndActionHash(tivoliOptionsMenu, MenuOption::BypassScriptThrottling, 0,
        qApp->getBypassScriptThrottling());
    connect(action, &QAction::triggered, [action] { 
        qApp->setBypassScriptThrottling(action->isChecked());
     });


    // Developer > Scripting >>>
    MenuWrapper* scriptingOptionsMenu = developerMenu->addMenu("Scripting");
    
    // Developer > Scripting > Console...
    addActionToQMenuAndActionHash(scriptingOptionsMenu, MenuOption::Console, Qt::CTRL | Qt::ALT | Qt::Key_J,
                                  DependencyManager::get<StandAloneJSConsole>().data(),
                                  SLOT(toggleConsole()),
                                  QAction::NoRole,
                                  UNSPECIFIED_POSITION);

     // Developer > Scripting > API Debugger
    action = addActionToQMenuAndActionHash(scriptingOptionsMenu, "API Debugger");
    connect(action, &QAction::triggered, [] {
        QUrl defaultScriptsLoc = PathUtils::defaultScriptsLocation();
        defaultScriptsLoc.setPath(defaultScriptsLoc.path() + "developer/utilities/tools/currentAPI.js");
        DependencyManager::get<ScriptEngines>()->loadScript(defaultScriptsLoc.toString());
    });
    
    // Developer > Scripting > Entity Script Server Log
    auto essLogAction = addActionToQMenuAndActionHash(scriptingOptionsMenu, MenuOption::EntityScriptServerLog, 0,
                                                      qApp, SLOT(toggleEntityScriptServerLogDialog()));
    {
        auto nodeList = DependencyManager::get<NodeList>();
        QObject::connect(nodeList.data(), &NodeList::canRezChanged, essLogAction, [essLogAction] {
            auto nodeList = DependencyManager::get<NodeList>();
            essLogAction->setEnabled(nodeList->getThisNodeCanRez());
        });
        essLogAction->setEnabled(nodeList->getThisNodeCanRez());
    }

    // Developer > Scripting > Script Log (HMD friendly)...
    addActionToQMenuAndActionHash(scriptingOptionsMenu, "Script Log (HMD friendly)...", Qt::NoButton,
                                           qApp, SLOT(showScriptLogs()));

    // Developer > Scripting > Verbose Logging
    addCheckableActionToQMenuAndActionHash(scriptingOptionsMenu, MenuOption::VerboseLogging, 0, false,
                                           qApp, SLOT(updateVerboseLogging()));

    // Developer > Scripting > Run JavaScript on QtScript (the old engine): the way back from the WebAssembly engine, which
    // .js client scripts load on otherwise; it takes effect for scripts loaded after the change. Without the runtime
    // file every JavaScript script runs on QtScript, so the item shows ticked and can't be changed; it has its own name,
    // since a menu item's name is its saved key and a saved tick would turn a later client with the file to QtScript
    if (WasmScriptEngine::hasJavaScriptGuest()) {
        addCheckableActionToQMenuAndActionHash(scriptingOptionsMenu, MenuOption::RunJavaScriptOnQtScript,
                                               [](bool checked) { WasmScriptEngine::setRunJavaScriptOnWebAssembly(!checked); },
                                               0, !WasmScriptEngine::runJavaScriptOnWebAssembly());
    } else {
        action = addCheckableActionToQMenuAndActionHash(scriptingOptionsMenu, MenuOption::RunJavaScriptOnQtScriptNoRuntime,
                                                        0, true);
        action->setEnabled(false);
    }

    // Developer > UI >>>
    MenuWrapper* uiOptionsMenu = developerMenu->addMenu("UI");
    action = addCheckableActionToQMenuAndActionHash(uiOptionsMenu, MenuOption::DesktopTabletToToolbar, 0,
                                                    qApp->getDesktopTabletBecomesToolbarSetting());
    
    // Developer > UI > Show Overlays
    addCheckableActionToQMenuAndActionHash(uiOptionsMenu, MenuOption::Overlays, 0, true);
    
    // Developer > UI > Desktop Tablet Becomes Toolbar
    connect(action, &QAction::triggered, [action] {
        qApp->setDesktopTabletBecomesToolbarSetting(action->isChecked());
    });
    
     // Developer > UI > HMD Tablet Becomes Toolbar
    action = addCheckableActionToQMenuAndActionHash(uiOptionsMenu, MenuOption::HMDTabletToToolbar, 0,
                                                    qApp->getHmdTabletBecomesToolbarSetting());
    connect(action, &QAction::triggered, [action] {
        qApp->setHmdTabletBecomesToolbarSetting(action->isChecked());
    });

    // Developer > Render >>>
    MenuWrapper* renderOptionsMenu = developerMenu->addMenu("Render");

    // addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::AntiAliasing, 0, RenderScriptingInterface::getInstance()->getAntialiasingEnabled(),
    // RenderScriptingInterface::getInstance(), SLOT(setAntialiasingEnabled(bool)));

    addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::Shadows, 0, RenderScriptingInterface::getInstance()->getShadowsEnabled(),
        RenderScriptingInterface::getInstance(), SLOT(setShadowsEnabled(bool)));

    // Developer > Render > GPU Instancing (INSTANCING.md 3.6): copies of a model drawn in one draw call; off to compare
    addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::GpuInstancing, 0, true);

    addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::AmbientOcclusion, 0, RenderScriptingInterface::getInstance()->getAmbientOcclusionEnabled(),
        RenderScriptingInterface::getInstance(), SLOT(setAmbientOcclusionEnabled(bool)));

    // Developer > Render > Forward Rendering: the forward path (Forward+: the point and spot lights with their shadows
    // and cookies, and the sun's cascades, shaded in one multisampled pass), the default since 2026-10-16; off,
    // deferred, for what it alone has (ambient occlusion, temporal antialiasing, scattering). The item shows the
    // method in use, whoever set it (a preset, a script, the setting)
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::RenderForward, 0,
        RenderScriptingInterface::getInstance()->getRenderMethod() == RenderScriptingInterface::RenderMethod::FORWARD);
    connect(action, &QAction::triggered, [action] {
        RenderScriptingInterface::getInstance()->setRenderMethod(action->isChecked() ?
            RenderScriptingInterface::RenderMethod::FORWARD : RenderScriptingInterface::RenderMethod::DEFERRED);
    });
    connect(RenderScriptingInterface::getInstance(), &RenderScriptingInterface::settingsChanged, action, [action] {
        // the method changed by another road (a preset, a script): the item follows
        action->setChecked(RenderScriptingInterface::getInstance()->getRenderMethod() == RenderScriptingInterface::RenderMethod::FORWARD);
    });

    // Developer > Render > Vsync: the swap waits for the display's refresh. Off by default: the frames are paced at the
    // display's rate without a wait and the compositor shows the newest at each refresh, which is even; on macOS the
    // GL swap's wait was measured at two refreshes with the odd frame queued at once, a ragged 72 Hz on a 144 Hz
    // panel (PHYSICS.md "The driver's stutter"). The setting is Render.vsyncEnabled; TIVOLI_VSYNC in the environment
    // forces it.
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::Vsync, 0,
        RenderScriptingInterface::getInstance()->getVsyncEnabled());
    connect(action, &QAction::triggered, [action] {
        RenderScriptingInterface::getInstance()->setVsyncEnabled(action->isChecked());
    });
    connect(RenderScriptingInterface::getInstance(), &RenderScriptingInterface::settingsChanged, action, [action] {
        action->setChecked(RenderScriptingInterface::getInstance()->getVsyncEnabled());
    });

    // Developer > Render > MSAA (Forward): the forward path's multisampling, 1, 2, 4 or 8 samples a pixel (the
    // graphics presets set it: Potato 1, Low 2, Medium and High 4); the setting behind it is Render.msaaSamples
    {
        MenuWrapper* msaaMenu = renderOptionsMenu->addMenu(MenuOption::RenderMsaaMenu);
        QActionGroup* msaaGroup = new QActionGroup(msaaMenu);
        msaaGroup->setExclusive(true);
        auto renderInterface = RenderScriptingInterface::getInstance();
        const std::vector<std::pair<QString, int>> choices = {
            { MenuOption::RenderMsaaOff, 1 }, { MenuOption::RenderMsaa2, 2 }, { MenuOption::RenderMsaa4, 4 }, { MenuOption::RenderMsaa8, 8 }
        };
        const int maxSamples = gpu::Context::getMaxSamples();   // known: the GL backend is up before the menu
        for (const auto& choice : choices) {
            const int samples = choice.second;
            auto msaaAction = msaaGroup->addAction(addCheckableActionToQMenuAndActionHash(msaaMenu, choice.first, 0,
                renderInterface->getMsaaSamples() == samples));
            msaaAction->setEnabled(maxSamples <= 0 || samples <= maxSamples);   // what this GPU cannot do is greyed
            connect(msaaAction, &QAction::triggered, [samples] { RenderScriptingInterface::getInstance()->setMsaaSamples(samples); });
            connect(renderInterface, &RenderScriptingInterface::settingsChanged, msaaAction, [msaaAction, samples] {
                msaaAction->setChecked(RenderScriptingInterface::getInstance()->getMsaaSamples() == samples);
            });
        }
    }

    // Developer > Render > Cache Light Shadows: off, a spot or point light's shadow maps are drawn every frame, for
    // comparing
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::LocalShadowCaching, 0, true);
    connect(action, &QAction::triggered, [action] {
        auto renderConfig = qApp->getRenderEngine()->getConfiguration();
        for (const auto& view : { "RenderMainView.LocalShadowSetup", "RenderSecondView.LocalShadowSetup" }) {
            auto config = renderConfig->getConfig<RenderLocalShadowSetup>(view);
            if (config) {
                config->setCaching(action->isChecked());
            }
        }
    });
    // Developer > Render > Cache Static Shadow Casters: off, a light's map is drawn whole each time (no map of the
    // casters that haven't changed for a while, copied under the moving ones), for comparing
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::LocalShadowStaticCaching, 0, true);
    connect(action, &QAction::triggered, [action] {
        auto renderConfig = qApp->getRenderEngine()->getConfiguration();
        for (const auto& view : { "RenderMainView.LocalShadowSetup", "RenderSecondView.LocalShadowSetup" }) {
            auto config = renderConfig->getConfig<RenderLocalShadowSetup>(view);
            if (config) {
                config->setStaticCaching(action->isChecked());
            }
        }
    });
    // Developer > Render > Mirrors: mirror shapes show their reflection (Mirrors.h); off, they show their colour
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::Mirrors, 0, true);
    connect(action, &QAction::triggered, [action] { Mirrors::instance().setEnabled(action->isChecked()); });
    // Developer > Render > Half-Resolution Mirrors: a mirror's picture at half its size on screen, a quarter of the cost
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::MirrorsHalfResolution, 0, false);
    connect(action, &QAction::triggered, [action] { Mirrors::instance().setResolutionScale(action->isChecked() ? 0.5f : 1.0f); });

    // Developer > Render > Impostor Cards: entities with an impostor card draw it at a distance (IMPOSTORS.md); off,
    // every entity draws its model. Not saved.
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::ImpostorCards, 0, true);
    connect(action, &QAction::triggered, [action] {
        DependencyManager::get<LODManager>()->setImpostorsEnabled(action->isChecked());
    });
    // Developer > Render > Automatic LODs: meshes draw their simplified versions where those look the same
    // (AUTOMATIC-LODS.md); off, every mesh draws in full. Not saved.
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::AutomaticLods, 0, true);
    connect(action, &QAction::triggered, [action] {
        DependencyManager::get<LODManager>()->setMeshLodsEnabled(action->isChecked());
    });
    // Developer > Render > Show Impostors and LODs: every card tinted blue, and what the view draws of the thing at its
    // centre, card or model and its LOD level, written at the top of the view (scripts/system/impostorLodView.js reads
    // LODManager.showImpostorsAndLods). Not saved.
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::ShowImpostorsAndLods, 0, false);
    connect(action, &QAction::triggered, [action] {
        DependencyManager::get<LODManager>()->setShowImpostorsAndLods(action->isChecked());
    });
    // Developer > Render > Show Models Still Loading: a model whose file hasn't loaded yet, or failed to, shows as a
    // magenta wire box where it will be, as High Fidelity always drew it; off, nothing shows there until it's ready
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::ShowModelsStillLoading, 0, false);
    connect(action, &QAction::triggered, [action] {
        render::entities::ModelEntityRenderer::setShowModelsStillLoading(action->isChecked());
    });
    // Developer > Render > Show NaN Pixels: a fragment whose shading came out NaN (not a number) is painted magenta, so
    // the fault can be found; off, it is black, which players barely notice
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::ShowNaNPixels, 0, false);
    connect(action, &QAction::triggered, [action] {
        ToneMapAndResample::setShowNaNPixels(action->isChecked());
    });
    // Developer > Render > Upscaling (FSR) in VR: a resolution scale under 1x upscales with FSR in a headset too, for
    // trying it there; off (the default), VR keeps the plain stretch. Follows the item's state however it's set.
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::UpscaleFSRInVR, 0, false);
    connect(action, &QAction::toggled, [](bool checked) {
        ToneMapAndResample::setUpscaleFSRInVR(checked);
    });

    addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::WorldAxes);
    // addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::DefaultSkybox, Qt::Key_Asterisk, true);

    // Developer > Render > Throttle FPS If Not Focus
    // addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::ThrottleFPSIfNotFocus, 0, true);

    // Developer > Render > OpenVR threaded submit
    addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::OpenVrThreadedSubmit, 0, true);

#ifdef Q_OS_WIN
    // Developer > Render > Enable Sparse Textures
    {
        auto action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::SparseTextureManagement, 0, gpu::Texture::getEnableSparseTextures());
        connect(action, &QAction::triggered, [&](bool checked) {
            qDebug() << "[TEXTURE TRANSFER SUPPORT] --- Enable Dynamic Texture Management menu option:" << checked;
            gpu::Texture::setEnableSparseTextures(checked);
        });
    }

#else
    qDebug() << "[TEXTURE TRANSFER SUPPORT] Incremental Texture Transfer and Dynamic Texture Management not supported on this platform.";
#endif


    {
        auto action = addActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::RenderClearKtxCache);
        connect(action, &QAction::triggered, []{
            Setting::Handle<int>(KTXCache::SETTING_VERSION_NAME, KTXCache::INVALID_VERSION).set(KTXCache::INVALID_VERSION);
        });
    }

    // Developer > Render > LOD Tools
    addActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::LodTools, 0,
                                  qApp, SLOT(loadLODToolsDialog()));

    // HACK enable texture decimation
    {
        auto action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, "Decimate Textures");
        connect(action, &QAction::triggered, [&](bool checked) {
            DEV_DECIMATE_TEXTURES = checked;
        });
    }

    addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::ComputeBlendshapes, 0, true,
        DependencyManager::get<ModelBlender>().data(), SLOT(setComputeBlendshapes(bool)));

    {
        auto drawStatusConfig = qApp->getRenderEngine()->getConfiguration()->getConfig<render::DrawStatus>("RenderMainView.DrawStatus");
        addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::HighlightTransitions, 0, false,
            drawStatusConfig, SLOT(setShowFade(bool)));
    }


    // Settings > Developer > Render > ForceUnlit
    action = addCheckableActionToQMenuAndActionHash(renderOptionsMenu, MenuOption::ForceEverythingUnlit, 0, false);
    connect(action, &QAction::triggered, [action] {
        TextureCache::setEverythingUnlit(action->isChecked());

    });
    // Developer > Assets >>>
    // Menu item is not currently needed but code should be kept in case it proves useful again at some stage.
//#define WANT_ASSET_MIGRATION
#ifdef WANT_ASSET_MIGRATION
    MenuWrapper* assetDeveloperMenu = developerMenu->addMenu("Assets");
    auto& atpMigrator = ATPAssetMigrator::getInstance();
    atpMigrator.setDialogParent(this);

    addActionToQMenuAndActionHash(assetDeveloperMenu, MenuOption::AssetMigration,
        0, &atpMigrator,
        SLOT(loadEntityServerFile()));
#endif

    // Developer > Avatar >>>
    MenuWrapper* avatarDebugMenu = developerMenu->addMenu("Avatar");

    action = addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::AvatarReceiveStats, 0, false);
    connect(action, &QAction::triggered, [this]{ Avatar::setShowReceiveStats(isOptionChecked(MenuOption::AvatarReceiveStats)); });
    action = addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::ShowBoundingCollisionShapes, 0, false);
    connect(action, &QAction::triggered, [this]{ Avatar::setShowCollisionShapes(isOptionChecked(MenuOption::ShowBoundingCollisionShapes)); });
    action = addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::ShowMyLookAtVectors, 0, false);
    connect(action, &QAction::triggered, [this]{ Avatar::setShowMyLookAtVectors(isOptionChecked(MenuOption::ShowMyLookAtVectors)); });
    action = addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::ShowMyLookAtTarget, 0, false);
    connect(action, &QAction::triggered, [this]{ Avatar::setShowMyLookAtTarget(isOptionChecked(MenuOption::ShowMyLookAtTarget)); });
    action = addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::ShowOtherLookAtVectors, 0, false);
    connect(action, &QAction::triggered, [this]{ Avatar::setShowOtherLookAtVectors(isOptionChecked(MenuOption::ShowOtherLookAtVectors)); });
    action = addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::ShowOtherLookAtTarget, 0, false);
    connect(action, &QAction::triggered, [this]{ Avatar::setShowOtherLookAtTarget(isOptionChecked(MenuOption::ShowOtherLookAtTarget)); });

    auto avatarManager = DependencyManager::get<AvatarManager>();
    auto avatar = avatarManager->getMyAvatar();

    action = addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::EnableLookAtSnapping, 0, true);
    connect(action, &QAction::triggered, [this, avatar]{
            avatar->setProperty("lookAtSnappingEnabled", isOptionChecked(MenuOption::EnableLookAtSnapping));
        });

    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::FixGaze, 0, false);
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::ToggleHipsFollowing, 0, false,
        avatar.get(), SLOT(setToggleHips(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::AnimDebugDrawBaseOfSupport, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawBaseOfSupport(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::AnimDebugDrawDefaultPose, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawDefaultPose(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::AnimDebugDrawAnimPose, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawAnimPose(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::AnimDebugDrawPosition, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawPosition(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::AnimDebugDrawOtherSkeletons, 0, false,
        avatarManager.data(), SLOT(setEnableDebugDrawOtherSkeletons(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::MeshVisible, 0, true,
        avatar.get(), SLOT(setEnableMeshVisible(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::DisableEyelidAdjustment, 0, false);
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::TurnWithHead, 0, false);
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::EnableInverseKinematics, 0, true,
        avatar.get(), SLOT(setEnableInverseKinematics(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::RenderSensorToWorldMatrix, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawSensorToWorldMatrix(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::RenderIKTargets, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawIKTargets(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::RenderIKConstraints, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawIKConstraints(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::RenderIKChains, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawIKChains(bool)));
    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::RenderDetailedCollision, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawDetailedCollision(bool)));

    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::ActionMotorControl, 0, true,
        avatar.get(), SLOT(updateMotionBehaviorFromMenu()),
        UNSPECIFIED_POSITION, "Developer");

    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::ScriptedMotorControl, 0, true,
        avatar.get(), SLOT(updateMotionBehaviorFromMenu()),
        UNSPECIFIED_POSITION, "Developer");

    addCheckableActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::ShowTrackedObjects, 0, false, qApp, SLOT(setShowTrackedObjects(bool)));

    addActionToQMenuAndActionHash(avatarDebugMenu, MenuOption::PackageModel, 0, qApp, SLOT(packageModel()));

    // Developer > Hands >>>
    MenuWrapper* handOptionsMenu = developerMenu->addMenu("Hands");
    addCheckableActionToQMenuAndActionHash(handOptionsMenu, MenuOption::DisplayHandTargets, 0, false,
        avatar.get(), SLOT(setEnableDebugDrawHandControllers(bool)));

    // Developer > Entities >>>
    MenuWrapper* entitiesOptionsMenu = developerMenu->addMenu("Entities");

    addActionToQMenuAndActionHash(entitiesOptionsMenu, MenuOption::OctreeStats, 0,
        qApp, SLOT(loadEntityStatisticsDialog()));

    addCheckableActionToQMenuAndActionHash(entitiesOptionsMenu, MenuOption::ShowRealtimeEntityStats);

    // Developer > Network >>>
    MenuWrapper* networkMenu = developerMenu->addMenu("Network");
    action = addActionToQMenuAndActionHash(networkMenu, MenuOption::Networking);
    connect(action, &QAction::triggered, [] {
        qApp->showDialog(QString("hifi/dialogs/NetworkingPreferencesDialog.qml"),
            QString("hifi/tablet/TabletNetworkingPreferences.qml"), "NetworkingPreferencesDialog");
    });
    addActionToQMenuAndActionHash(networkMenu, MenuOption::ReloadContent, 0, qApp, SLOT(reloadResourceCaches()));

	action = addActionToQMenuAndActionHash(networkMenu, MenuOption::ClearDiskCaches);
    connect(action, &QAction::triggered, [] {
        // The following caches are cleared immediately
        DependencyManager::get<AssetClient>()->clearCache();
#ifndef Q_OS_ANDROID
        FileTypeProfile::clearCache();
        TivoliWebEngineProfile::clearCache();
#endif

        // Clear the KTX cache on the next restart. It can't be cleared immediately because its files might be in use.
        Setting::Handle<int>(KTXCache::SETTING_VERSION_NAME, KTXCache::INVALID_VERSION).set(KTXCache::INVALID_VERSION);
    });

    addActionToQMenuAndActionHash(networkMenu, MenuOption::ShowDSConnectTable, 0,
        qApp, SLOT(loadDomainConnectionDialog()));

    #if (PR_BUILD || DEV_BUILD)
    addCheckableActionToQMenuAndActionHash(networkMenu, MenuOption::SendWrongProtocolVersion, 0, false,
                qApp, SLOT(sendWrongProtocolVersionsSignature(bool)));

    {
        auto nodeList = DependencyManager::get<NodeList>();
        addCheckableActionToQMenuAndActionHash(networkMenu, MenuOption::SendWrongDSConnectVersion, 0, false,
            nodeList.data(), SLOT(toggleSendNewerDSConnectVersion(bool)));
    }
    #endif

    // Developer > Timing >>>
    MenuWrapper* timingMenu = developerMenu->addMenu("Timing");
    MenuWrapper* perfTimerMenu = timingMenu->addMenu("Performance Timer");
    addCheckableActionToQMenuAndActionHash(perfTimerMenu, MenuOption::DisplayDebugTimingDetails);
    addCheckableActionToQMenuAndActionHash(perfTimerMenu, MenuOption::OnlyDisplayTopTen, 0, true);
    addCheckableActionToQMenuAndActionHash(perfTimerMenu, MenuOption::ExpandUpdateTiming, 0, false);
    addCheckableActionToQMenuAndActionHash(perfTimerMenu, MenuOption::ExpandSimulationTiming, 0, false);
    addCheckableActionToQMenuAndActionHash(perfTimerMenu, MenuOption::ExpandPhysicsTiming, 0, false);
    addCheckableActionToQMenuAndActionHash(perfTimerMenu, MenuOption::ExpandMyAvatarTiming, 0, false);
    addCheckableActionToQMenuAndActionHash(perfTimerMenu, MenuOption::ExpandMyAvatarSimulateTiming, 0, false);
    addCheckableActionToQMenuAndActionHash(perfTimerMenu, MenuOption::ExpandOtherAvatarTiming, 0, false);
    addCheckableActionToQMenuAndActionHash(perfTimerMenu, MenuOption::ExpandPaintGLTiming, 0, false);

    addCheckableActionToQMenuAndActionHash(timingMenu, MenuOption::FrameTimer);
    addActionToQMenuAndActionHash(timingMenu, MenuOption::RunTimingTests, 0, qApp, SLOT(runTests()));
    addCheckableActionToQMenuAndActionHash(timingMenu, MenuOption::PipelineWarnings);
    addCheckableActionToQMenuAndActionHash(timingMenu, MenuOption::LogExtraTimings);
    addCheckableActionToQMenuAndActionHash(timingMenu, MenuOption::SuppressShortTimings
        // Qt::Key_S, isShifted && isControlOrCommand && !isOption
    );


    // Developer > Audio >>>
    MenuWrapper* audioDebugMenu = developerMenu->addMenu("Audio");

    action = addActionToQMenuAndActionHash(audioDebugMenu, "Stats...");
    connect(action, &QAction::triggered, [] {
        QUrl defaultScriptsLoc = PathUtils::defaultScriptsLocation();
        defaultScriptsLoc.setPath(defaultScriptsLoc.path() + "developer/utilities/audio/stats.js");
        DependencyManager::get<ScriptEngines>()->loadScript(defaultScriptsLoc.toString());
    });

    action = addActionToQMenuAndActionHash(audioDebugMenu, "Buffers...");
    connect(action, &QAction::triggered, [] {
        qApp->showDialog(QString("hifi/dialogs/AudioBuffers.qml"),
            QString("hifi/tablet/TabletAudioBuffers.qml"), "AudioBuffersDialog");
    });

    addActionToQMenuAndActionHash(audioDebugMenu, MenuOption::MuteEnvironment, 0,
        DependencyManager::get<AudioClient>().data(), SLOT(sendMuteEnvironmentPacket()));

    action = addActionToQMenuAndActionHash(audioDebugMenu, MenuOption::AudioScope);
    connect(action, &QAction::triggered, [] {
        QUrl defaultScriptsLoc = PathUtils::defaultScriptsLocation();
        defaultScriptsLoc.setPath(defaultScriptsLoc.path() + "developer/utilities/audio/audioScope.js");
        DependencyManager::get<ScriptEngines>()->loadScript(defaultScriptsLoc.toString());
    });

    // Developer > Physics >>>
    MenuWrapper* physicsOptionsMenu = developerMenu->addMenu("Physics");
    {
        auto drawStatusConfig = qApp->getRenderEngine()->getConfiguration()->getConfig<render::DrawStatus>("RenderMainView.DrawStatus");
        addCheckableActionToQMenuAndActionHash(physicsOptionsMenu, MenuOption::PhysicsShowOwned,
            0, false, drawStatusConfig, SLOT(setShowNetwork(bool)));
    }

    addCheckableActionToQMenuAndActionHash(physicsOptionsMenu, MenuOption::PhysicsShowBulletWireframe, 0, false, qApp, SLOT(setShowBulletWireframe(bool)));
    addCheckableActionToQMenuAndActionHash(physicsOptionsMenu, MenuOption::PhysicsShowBulletAABBs, 0, false, qApp, SLOT(setShowBulletAABBs(bool)));
    addCheckableActionToQMenuAndActionHash(physicsOptionsMenu, MenuOption::PhysicsShowBulletContactPoints, 0, false, qApp, SLOT(setShowBulletContactPoints(bool)));
    addCheckableActionToQMenuAndActionHash(physicsOptionsMenu, MenuOption::PhysicsShowBulletConstraints, 0, false, qApp, SLOT(setShowBulletConstraints(bool)));
    addCheckableActionToQMenuAndActionHash(physicsOptionsMenu, MenuOption::PhysicsShowBulletConstraintLimits, 0, false, qApp, SLOT(setShowBulletConstraintLimits(bool)));

    // Developer > Picking >>>
    MenuWrapper* pickingOptionsMenu = developerMenu->addMenu("Picking");
    addCheckableActionToQMenuAndActionHash(pickingOptionsMenu, MenuOption::ForceCoarsePicking, 0, false,
        DependencyManager::get<PickManager>().data(), SLOT(setForceCoarsePicking(bool)));

    // Developer > Crash >>>
    bool result = false;
    const QString HIFI_SHOW_DEVELOPER_CRASH_MENU("HIFI_SHOW_DEVELOPER_CRASH_MENU");
    result = QProcessEnvironment::systemEnvironment().contains(HIFI_SHOW_DEVELOPER_CRASH_MENU);
    if (result) {
        MenuWrapper* crashMenu = developerMenu->addMenu("Crash");
    
        // Developer > Crash > Display Crash Options
        addCheckableActionToQMenuAndActionHash(crashMenu, MenuOption::DisplayCrashOptions, 0, true);

        addActionToQMenuAndActionHash(crashMenu, MenuOption::DeadlockInterface, 0, qApp, SLOT(deadlockApplication()));
        addActionToQMenuAndActionHash(crashMenu, MenuOption::UnresponsiveInterface, 0, qApp, SLOT(unresponsiveApplication()));

        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashPureVirtualFunction);
        connect(action, &QAction::triggered, qApp, []() { crash::pureVirtualCall(); });
        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashPureVirtualFunctionThreaded);
        connect(action, &QAction::triggered, qApp, []() { std::thread(crash::pureVirtualCall).join(); });

        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashDoubleFree);
        connect(action, &QAction::triggered, qApp, []() { crash::doubleFree(); });
        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashDoubleFreeThreaded);
        connect(action, &QAction::triggered, qApp, []() { std::thread(crash::doubleFree).join(); });

        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashAbort);
        connect(action, &QAction::triggered, qApp, []() { crash::doAbort(); });
        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashAbortThreaded);
        connect(action, &QAction::triggered, qApp, []() { std::thread(crash::doAbort).join(); });

        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashNullDereference);
        connect(action, &QAction::triggered, qApp, []() { crash::nullDeref(); });
        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashNullDereferenceThreaded);
        connect(action, &QAction::triggered, qApp, []() { std::thread(crash::nullDeref).join(); });

        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashOutOfBoundsVectorAccess);
        connect(action, &QAction::triggered, qApp, []() { crash::outOfBoundsVectorCrash(); });
        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashOutOfBoundsVectorAccessThreaded);
        connect(action, &QAction::triggered, qApp, []() { std::thread(crash::outOfBoundsVectorCrash).join(); });

        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashNewFault);
        connect(action, &QAction::triggered, qApp, []() { crash::newFault(); });
        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashNewFaultThreaded);
        connect(action, &QAction::triggered, qApp, []() { std::thread(crash::newFault).join(); });

        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashThrownException);
        connect(action, &QAction::triggered, qApp, []() { crash::throwException(); });
        action = addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashThrownExceptionThreaded);
        connect(action, &QAction::triggered, qApp, []() { std::thread(crash::throwException).join(); });

        addActionToQMenuAndActionHash(crashMenu, MenuOption::CrashOnShutdown, 0, qApp, SLOT(crashOnShutdown()));
    }
    

    // Developer > Show Statistics
    addCheckableActionToQMenuAndActionHash(developerMenu, MenuOption::Stats, Qt::Key_F3, true);

    // Developer > Show Animation Statistics
    addCheckableActionToQMenuAndActionHash(developerMenu, MenuOption::AnimStats);

    // Developer > Log
    addActionToQMenuAndActionHash(developerMenu, MenuOption::Log, Qt::CTRL | Qt::SHIFT | Qt::Key_L,
                                  qApp, SLOT(toggleLogDialog()));

#if 0 ///  -------------- REMOVED FOR NOW --------------
    addDisabledActionAndSeparator(navigateMenu, "History");
    QAction* backAction = addActionToQMenuAndActionHash(navigateMenu, MenuOption::Back, 0, addressManager.data(), SLOT(goBack()));
    QAction* forwardAction = addActionToQMenuAndActionHash(navigateMenu, MenuOption::Forward, 0, addressManager.data(), SLOT(goForward()));

    // connect to the AddressManager signal to enable and disable the back and forward menu items
    connect(addressManager.data(), &AddressManager::goBackPossible, backAction, &QAction::setEnabled);
    connect(addressManager.data(), &AddressManager::goForwardPossible, forwardAction, &QAction::setEnabled);

    // set the two actions to start disabled since the stacks are clear on startup
    backAction->setDisabled(true);
    forwardAction->setDisabled(true);

    MenuWrapper* toolsMenu = addMenu("Tools");
    addActionToQMenuAndActionHash(toolsMenu,
                                  MenuOption::ToolWindow,
                                  Qt::CTRL | Qt::ALT | Qt::Key_T,
                                  dialogsManager.data(),
                                  SLOT(toggleToolWindow()),
                                  QAction::NoRole, UNSPECIFIED_POSITION, "Advanced");


    addCheckableActionToQMenuAndActionHash(avatarMenu, MenuOption::NamesAboveHeads, 0, true,
                NULL, NULL, UNSPECIFIED_POSITION, "Advanced");
#endif

    // Help/Application menu ----------------------------------
    MenuWrapper * helpMenu = addMenu("Help");

    action = addActionToQMenuAndActionHash(helpMenu, "About Darkride");
    connect(action, &QAction::triggered, [] {
        qApp->showDialog(QString("hifi/dialogs/AboutDialog.qml"),
            QString("hifi/dialogs/TabletAboutDialog.qml"), "AboutDialog");
    });
    helpMenu->addSeparator();

    // Help > Online Documentation, JavaScript API Reference, Changelog and Report a Bug stay greyed out until
    // Darkride has those pages (Tivoli's are gone)
    action = addActionToQMenuAndActionHash(helpMenu, "Online Documentation");
    action->setEnabled(false);

    // Help > Discord Server
    action = addActionToQMenuAndActionHash(helpMenu, "Discord Server");
    connect(action, &QAction::triggered, qApp, [] {
        QDesktopServices::openUrl(QUrl("https://discord.gg/fQqG7dT9wY"));
    });

    action = addActionToQMenuAndActionHash(helpMenu, "JavaScript API Reference");
    action->setEnabled(false);

    addActionToQMenuAndActionHash(helpMenu, "Controls Reference", 0, qApp, SLOT(showHelp()));

    helpMenu->addSeparator();

    action = addActionToQMenuAndActionHash(helpMenu, "Changelog");
    action->setEnabled(false);

    action = addActionToQMenuAndActionHash(helpMenu, "Report a Bug");
    action->setEnabled(false);
}

void Menu::addMenuItem(const MenuItemProperties& properties) {
    MenuWrapper* menuObj = getMenu(properties.menuName);
    if (menuObj) {
        QShortcut* shortcut = NULL;
        if (!properties.shortcutKeySequence.isEmpty()) {
            shortcut = new QShortcut(properties.shortcutKeySequence, this);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        }

        // check for positioning requests
        int requestedPosition = properties.position;
        if (requestedPosition == UNSPECIFIED_POSITION && !properties.beforeItem.isEmpty()) {
            requestedPosition = findPositionOfMenuItem(menuObj, properties.beforeItem);
            // double check that the requested location wasn't a separator label
            requestedPosition = positionBeforeSeparatorIfNeeded(menuObj, requestedPosition);
        }
        if (requestedPosition == UNSPECIFIED_POSITION && !properties.afterItem.isEmpty()) {
            int afterPosition = findPositionOfMenuItem(menuObj, properties.afterItem);
            if (afterPosition != UNSPECIFIED_POSITION) {
                requestedPosition = afterPosition + 1;
            }
        }

        QAction* menuItemAction = NULL;
        if (properties.isSeparator) {
            addDisabledActionAndSeparator(menuObj, properties.menuItemName, requestedPosition, properties.grouping);
        } else if (properties.isCheckable) {
            menuItemAction = addCheckableActionToQMenuAndActionHash(menuObj, properties.menuItemName,
                                                                    properties.shortcutKeySequence, properties.isChecked,
                                                                    MenuScriptingInterface::getInstance(), SLOT(menuItemTriggered()),
                                                                    requestedPosition, properties.grouping);
        } else {
            menuItemAction = addActionToQMenuAndActionHash(menuObj, properties.menuItemName, properties.shortcutKeySequence,
                                                           MenuScriptingInterface::getInstance(), SLOT(menuItemTriggered()),
                                                           QAction::NoRole, requestedPosition, properties.grouping);
        }
        if (shortcut && menuItemAction) {
            connect(shortcut, SIGNAL(activated()), menuItemAction, SLOT(trigger()));
        }
        QMenuBar::repaint();
    }
}
