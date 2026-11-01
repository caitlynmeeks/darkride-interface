//
//  Panels.h
//  interface/src/ui
//
//  The window's panels by name, for the bar's buttons (tivoli-scripts' ButtonManager.addQmlButton), View > Panels and
//  scripts ("Panels"): Darkride's QML panels in dock panels (ui/QmlDock.h: Settings, Avatar, People, Explore, Radio), chat's,
//  My Files, the scripting console, and Build mode's Hierarchy and Inspector. Each opens floating the first time, docks or tabs where the player drags it, and comes back where the
//  window's saved layout has it (ui/WindowLayout.h). Open is "not hidden": a panel tabbed behind another is still
//  open. Dockable panels, step 2 (Caity's "go step 2", 2026-10-26; team/editor-ui/DOCKING-STEP2-PLAN.md).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_Panels_h
#define hifi_Panels_h

#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QSize>

#include <functional>

#include <DependencyManager.h>

class QDockWidget;
class QQuickItem;
class QQuickWindow;
class QWidget;

/**jsdoc
 * The window's panels, by name: Darkride's QML panels and chat. On the desktop each can float, dock beside the 3D view
 * or be tabbed with another panel.
 * @namespace Panels
 *
 * @hifi-interface
 * @hifi-client-entity
 * @hifi-avatar
 */
class Panels : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

public:
    Panels();

    /**jsdoc
     * The panels' names, in View > Panels' order.
     * @function Panels.names
     * @returns {string[]} The names: "Settings", "Avatar", "People", "Chat", "Explore", "Radio", "My Files", "Console", and Build
     *     mode's "Entities" and "Inspector" (which open only while it's on).
     */
    Q_INVOKABLE QStringList names() const;

    /**jsdoc
     * Whether a panel is open: shown, or tabbed behind another panel.
     * @function Panels.isOpen
     * @param {string} name - The panel.
     * @returns {boolean} Whether it's open.
     */
    Q_INVOKABLE bool isOpen(const QString& name) const;

    /**jsdoc
     * Opens a panel in front: a floating one in front of the other windows, a tabbed one with its tab in front.
     * @function Panels.open
     * @param {string} name - The panel.
     */
    Q_INVOKABLE void open(const QString& name);

    /**jsdoc
     * Closes a panel.
     * @function Panels.close
     * @param {string} name - The panel.
     */
    Q_INVOKABLE void close(const QString& name);

    /**jsdoc
     * Closes a panel when it's open and in front, else opens it in front.
     * @function Panels.toggle
     * @param {string} name - The panel.
     */
    Q_INVOKABLE void toggle(const QString& name);

    /**jsdoc
     * Opens the People panel in front on someone's profile: the card wherever a name appears (Explore's authors, chat's
     * names).
     * @function Panels.showProfile
     * @param {string} username - Their account name; "" for your own.
     */
    Q_INVOKABLE void showProfile(const QString& username);

    /// Build mode's own panels (Hierarchy, Inspector): they open only while it's on
    static bool isBuildModeOnly(const QString& name);

    /// a panel floating at its first size in the middle of the main window, no bigger than the window less the bar's
    /// room, kept on the window's screen
    static void placeFloating(QDockWidget* panel, QWidget* window, QSize size);

    /// the panel's dock, made (hidden, floating at its first size) the first time it's asked for; null for a name it
    /// doesn't know, or for Build mode's before it has been on. Main thread.
    QDockWidget* dock(const QString& name);

    /// after the window's saved layout is restored (WindowLayout::restore): every panel made, so the layout puts the
    /// open ones back where they were. A hidden panel costs nothing: its QML is made the first time it shows.
    void restore();

    /// at quit, before the QML engine goes (ApplicationShutdown.cpp): the QML panels' pages, made on its engine
    void releaseQml();

    /// the bar's panels closed, floating at their first places (View > Layouts > Reset to Default)
    void resetDocks();

    // ---- the VR screen (team/editor-ui/VR-SCREEN-PLAN.md; vr-screen/PANEL-SURFACE-INTERFACE.md): three slots, "left"
    // (People), "middle" (the other panels, one in front, tabs along its top) and "right" (the Radio, while it's on),
    // each an item in a Qt Quick window of vr's; the panels' pages, made once, move in and out. Main thread ----

    /// a slot's host item (ScreenSlot.qml) says it's ready; refused unless its window is one of the screen's
    Q_INVOKABLE void screenSlotReady(const QString& slot, QQuickItem* host);
    /// a slot's host QML, by its exact address: the app's names (Panels among them) go to that surface's context
    /// (Application::setupQmlSurface). Not a trusted address for Web entities: vr makes the slots as the app's own
    static bool isScreenSlotUrl(const QString& url);
    /// vr's check that a window is one of its slots' surfaces; until it's set, every host is refused
    static void setScreenWindowCheck(std::function<bool(QQuickWindow*)> check);
    /// a slot's layout size in panel pixels; empty while it holds nothing (the right one while the radio is off)
    Q_INVOKABLE QSize screenSlotSize(const QString& slot) const;
    /// "down" or "parked" (hiding is releaseScreenSlots); "tablet shown" for scripts follows it
    void setScreenState(const QString& state);
    QString screenState() const { return _screenState; }
    /// every page back in its dock (hidden while the headset is on), the slots forgotten: before vr deletes them
    void releaseScreenSlots();
    /// test builds only (DARKRIDE_SCREEN_TEST_SLOTS): a test script's releaseScreenSlots, before it deletes its slots
    Q_INVOKABLE void testReleaseScreenSlots();

signals:
    /**jsdoc
     * Triggered when a panel opens or closes, however that happened (its close box, View > Panels, a script).
     * @function Panels.openChanged
     * @param {string} name - The panel.
     * @param {boolean} open - Whether it's open now.
     * @returns {Signal}
     */
    void openChanged(const QString& name, bool open);

    /// a VR screen slot's size changed (the right one folds while the radio is off), or the screen's state did
    void screenSlotSizeChanged(const QString& slot);
    void screenStateChanged(const QString& state);

private:
    enum class Change { Open, Close, Toggle };
    void change(const QString& name, Change how);
    void watch(const QString& name, QDockWidget* dock);
    void setOpen(const QString& name, bool open);
    void styleTabs();   // the tabs of panels docked together, in the skin's colours
    QString screenSlotFor(const QString& name) const;
    bool putOnScreen(const QString& name);   // the page into its slot, if the screen is up
    void showDockPages();   // the pages in docks shown, or hidden while the headset is on (nobody sees the docks then)
    void displayChanged();   // the headset on or off
    void showHiddenForScreen();   // the docks the screen hid, showing again

    bool _restored { false };   // none opens before the layout's restore (after sign-in)
    QHash<QString, QPointer<QDockWidget>> _docks;
    mutable QMutex _openMutex;   // isOpen is asked from scripts' threads
    QSet<QString> _open;
    QHash<QString, QPointer<QQuickItem>> _screenSlots;
    QString _screenState;
    QStringList _middleTabs;   // the middle slot's panels, the front one last
    QSet<QString> _hiddenForScreen;   // docks that were showing when their page moved to the screen
};

#endif // hifi_Panels_h
