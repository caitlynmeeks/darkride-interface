//
//  Created by Bradley Austin Davis on 2016/12/12
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once
#ifndef hifi_TestScriptingInterface_h
#define hifi_TestScriptingInterface_h

#include <functional>
#include <QtCore/QObject>
#include <QtCore/QUuid>
#include <QtCore/QVariant>

#include <glm/glm.hpp>

/**jsdoc
 * Here's how you could use the Test API:
 * 
 * <ul>
 *     <li>Write a script using the below and launch interface with: <code>interface --testScript path/to/script.js</code></li>
 *     <li>Save the recorded events with {@link Test.stopTracing} to a <code>.json</code> or <code>.json.gz</code> file</li>
 *     <li>Open Chrome, visit <a href="chrome://tracing">chrome://tracing</a> and load your file</li>
 * </ul>
 * 
 * @namespace Test
 *
 * @hifi-interface
 */

class TestScriptingInterface : public QObject {
    Q_OBJECT

public:
    void setTestResultsLocation(const QString path) { _testResultsLocation = path; }
    const QString& getTestResultsLocation() { return _testResultsLocation;  };

public slots:
    static TestScriptingInterface* getInstance();

    /**jsdoc
     * Exits the application
     * @function Test.quit
     */
    void quit();

    /**jsdoc
    * Waits for all texture transfers to be complete
    * @function Test.waitForTextureIdle
    */
    void waitForTextureIdle();

    /**jsdoc
    * Waits for all pending downloads to be complete
    * @function Test.waitForDownloadIdle
    */
    void waitForDownloadIdle();

    /**jsdoc
    * Waits for all file parsing operations to be complete
    * @function Test.waitForProcessingIdle
    */
    void waitForProcessingIdle();

    /**jsdoc
    * Waits for all pending downloads, parsing and texture transfers to be complete
    * @function Test.waitIdle
    */
    void waitIdle();

    /**jsdoc
    * Waits for establishment of connection to server
    * @function Test.waitForConnection
    * @param {number} [maxWaitMs=10000] - Number of milliseconds to wait
    */
    bool waitForConnection(qint64 maxWaitMs = 10000);

    /**jsdoc
    * Waits a specific number of milliseconds
    * @function Test.wait
    * @param {number} milliseconds - Number of milliseconds to wait
    */
    void wait(int milliseconds);

    /**jsdoc
    * Waits for all pending downloads, parsing and texture transfers to be complete
    * @function Test.loadTestScene
    * @param {string} sceneFile - URL of scene to load
    */
    bool loadTestScene(QString sceneFile);

    /**jsdoc
    * Clears all caches
    * @function Test.clear
    */
    void clear();

    /**jsdoc
    * Start recording Chrome compatible tracing events<br />
    * <code>logrules</code> can be used to specify a set of logging category rules to limit what gets captured
    * @function Test.startTracing
    * @param {string} [logrules=""] - See implementation for explanation
    */
    bool startTracing(QString logrules = "");

    /**jsdoc
    * Stop recording Chrome compatible tracing events and serialize recorded events to a file<br />
    * Using a filename with <code>.gz</code> extension will automatically compress the output file
    * @function Test.stopTracing
    * @param {string} filename - Name of file to save to ending with <code>.json</code> or <code>.json.gz</code>
    * @returns {bool} True if successful.   
    */
    bool stopTracing(QString filename);

    /**jsdoc
    * Starts a specific trace event
    * @function Test.startTraceEvent
    * @param {string} name - Name of event
    */
    void startTraceEvent(QString name);

    /**jsdoc
    * Stop a specific name event
    * @function Test.endTraceEvent
    * @param {string} name - Name of event
    */
    void endTraceEvent(QString name);

    /**jsdoc
     * Write detailed timing stats of next physics stepSimulation() to filename
     * @function Test.savePhysicsSimulationStats
     * @param {string} filename - Name of file to save to
     */
    void savePhysicsSimulationStats(QString filename);

    /**jsdoc
    * Clear all caches (menu command Reload Content)
    * @function Test.clearCaches
    */
    void clearCaches();

    /**jsdoc
    * Save a JSON object to a file in the test results location
    * @function Test.saveObject
    * @param {string} name - Name of the object
    * @param {string} filename - Name of file to save to
    */
    void saveObject(QVariant v, const QString& filename);

    /**jsdoc
    * Maximizes the window
    * @function Test.showMaximized
    */
    void showMaximized();

    /**jsdoc
    * Values higher than 0 will create replicas of other-avatars when entering a domain for testing purpouses
    * @function Test.setOtherAvatarsReplicaCount
    * @param {number} count - Number of replicas we want to create
    */
    Q_INVOKABLE void setOtherAvatarsReplicaCount(int count);

    /**jsdoc
    * Return the number of replicas that are being created of other-avatars when entering a domain
    * @function Test.getOtherAvatarsReplicaCount
    * @returns {number} Current number of replicas of other-avatars.
    */
    Q_INVOKABLE int getOtherAvatarsReplicaCount();

    /**jsdoc
     * Set number of cycles texture size is required to be stable
     * @function Test.setMinimumGPUTextureMemStabilityCount
     * @param {number} count - Number of cycles to wait
     */
    Q_INVOKABLE void setMinimumGPUTextureMemStabilityCount(int count);

    /**jsdoc
     * Check whether all textures have been loaded
     * @function Test.isTextureLoadingComplete
     * @returns {boolean} <code>true</code> texture memory usage is not increasing
     */
    Q_INVOKABLE bool isTextureLoadingComplete();

    /**jsdoc
     * The rate of the game loop (simulation and script updates), in frames per second, as the stats show it.
     * @function Test.getGameLoopRate
     * @returns {number}
     */
    Q_INVOKABLE float getGameLoopRate();

    /**jsdoc
     * The rate at which frames are presented, in frames per second.
     * @function Test.getRenderLoopRate
     * @returns {number}
     */
    Q_INVOKABLE float getRenderLoopRate();

    /*@jsdoc
     * Runs the avatar's spring bones in lockstep: once on, they advance only by the time handed to them with
     * {@link Test.advanceSpringBones}, whatever the frames take, so a recording can be replayed frame by frame.
     * @function Test.setSpringBoneLockstep
     * @param {boolean} on - Lockstep on or off (off returns them to real time).
     */
    Q_INVOKABLE void setSpringBoneLockstep(bool on);

    /*@jsdoc
     * Hands the spring bones time to advance by at their next update, in lockstep.
     * @function Test.advanceSpringBones
     * @param {number} seconds - The time to advance.
     */
    Q_INVOKABLE void advanceSpringBones(float seconds);

    /*@jsdoc
     * The same, with the avatar's own movement over those seconds given (a replay whose avatar is placed by its hips
     * says what the recorded root did) rather than read from the avatar's position.
     * @function Test.advanceSpringBones
     * @param {number} seconds - The time to advance.
     * @param {Vec3} avatarMove - The avatar's own movement over that time.
     */
    Q_INVOKABLE void advanceSpringBones(float seconds, const glm::vec3& avatarMove);

    /*@jsdoc
     * Gets how many steps the avatar's spring bones have taken since the client started.
     * @function Test.getSpringBoneSteps
     * @returns {number} The step count.
     */
    Q_INVOKABLE int getSpringBoneSteps();

    /*@jsdoc
     * Gets the time handed to {@link Test.advanceSpringBones} that the spring bones have not consumed yet.
     * @function Test.getSpringBonePendingTime
     * @returns {number} Seconds.
     */
    Q_INVOKABLE float getSpringBonePendingTime();

    /*@jsdoc
     * Gets how many updates of the spring bones have consumed time handed by {@link Test.advanceSpringBones}, each
     * counted once its poses are written, so a test can read poses back only then.
     * @function Test.getSpringBoneConsumed
     * @returns {number} The count.
     */
    Q_INVOKABLE int getSpringBoneConsumed();

    /*@jsdoc
     * Gets the blend between the last two steps the spring bones last presented: 0.5 at a step's own frame, 1 a frame
     * of 60 Hz later.
     * @function Test.getSpringBoneAlpha
     * @returns {number} The blend.
     */
    Q_INVOKABLE float getSpringBoneAlpha();

    /*@jsdoc
     * Gets how many updates the avatar's spring bones have run in all.
     * @function Test.getSpringBoneUpdates
     * @returns {number} The count.
     */
    Q_INVOKABLE int getSpringBoneUpdates();

    /**jsdoc
     * Turns build mode on and selects these entities in the editor, so a scripted test can picture the Inspector.
     * @function Test.selectInEditor
     * @param {Uuid[]} ids - The entities to select.
     */
    Q_INVOKABLE void selectInEditor(const QStringList& ids);

    /**jsdoc
     * Frames the editor's selection in the viewport, as the F key does in build mode.
     * @function Test.frameEditorSelection
     */
    Q_INVOKABLE void frameEditorSelection();

    /**jsdoc
     * What the Inspector's Script or Server Script row says the script is doing now (its status line), for the
     * entities selected in the editor.
     * @function Test.editorScriptStatus
     * @param {string} key - <code>"script"</code> or <code>"serverScripts"</code>.
     * @returns {string} The status line, <code>""</code> when the row shows none.
     */
    Q_INVOKABLE QString editorScriptStatus(const QString& key);

    /**jsdoc
     * Presses Reload on the Inspector's Script or Server Script row, as a click would.
     * @function Test.reloadEditorScript
     * @param {string} key - <code>"script"</code> or <code>"serverScripts"</code>.
     * @returns {boolean} Whether the row and its button were there to press.
     */
    Q_INVOKABLE bool reloadEditorScript(const QString& key);

    /**jsdoc
     * An Inspector field of the editor's selection, as the panel shows it.
     * @function Test.inspectorField
     * @param {string} key - The Inspector's key for the property (e.g. "locked").
     * @returns {object} <code>{ found, shown, enabled, inHeader, value, mixed, tip, lockNote }</code>: whether the
     *     field is there and shown, whether it can be changed, whether it's in the header (Unity's: Visible, the name,
     *     Locked, Ignore Picking), its value, whether the selection's values differ, its tooltip, and the note the
     *     header shows while everything selected is locked (<code>""</code> when none).
     */
    Q_INVOKABLE QVariantMap inspectorField(const QString& key);

    /**jsdoc
     * One of the Inspector header's toggle icons for the editor's selection (the Hierarchy rows' four), as the panel
     * shows it, and a click on it as the mouse's.
     * @function Test.inspectorToggle
     * @param {string} name - "Hide", "NoPick", "Lock" or "Scripts".
     * @param {boolean} [click=false] - Whether to click it.
     * @returns {object} <code>{ found, shown, look, usable, tip }</code>: whether it's there and shown, "on", "off" or
     *     "mixed" (before the click's effect), whether a click would change something, and its tooltip.
     */
    Q_INVOKABLE QVariantMap inspectorToggle(const QString& name, bool click = false);

    /**jsdoc
     * Saves a picture of the Inspector's whole content, whatever is scrolled into view, as the panel draws it.
     * @function Test.saveInspectorPicture
     * @param {string} path - Where to save it (PNG).
     * @returns {boolean} Whether it was saved.
     */
    Q_INVOKABLE bool saveInspectorPicture(const QString& path);

    /**jsdoc
     * Saves a picture of the client's window as its widgets draw it (the editor's panels; the viewport itself may come
     * out blank, being drawn by the GPU: Window.takeSnapshot pictures that).
     * @function Test.saveWindowPicture
     * @param {string} path - Where to save it (PNG).
     * @returns {boolean} Whether it was saved.
     */
    Q_INVOKABLE bool saveWindowPicture(const QString& path);

    /**jsdoc
     * Where the window's dock panels are (ui/WindowLayout.h): chat's ("chat") and Build mode's ("hierarchy",
     * "inspector", "assets"), each <code>{ open, floating, area, x, y, width, height }</code> (area "left", "right",
     * "top", "bottom" or "none"; x and y on the screen), the side of the bar of app buttons ("bar"), and the width of
     * the edges between the panels and the 3D view ("separator", px).
     * @function Test.windowLayout
     * @returns {object} The panels, by name; a panel not made yet is missing.
     */
    Q_INVOKABLE QVariantMap windowLayout();

    /**jsdoc
     * Does what a dock panel's title bar button does (ui/DockTitleBar.h): "dock", "float" or "close"; or
     * "tab:<object name>", tabbed behind that docked panel, as a drag onto its title does; or "drag:<x>,<y>", a drag
     * by its title that many points, in Qt's events only (the real mouse untouched).
     * @function Test.dockAction
     * @param {string} objectName - The panel's object name, such as "ChatDock".
     * @param {string} action - "dock", "float", "close", "tab:<object name>" or "drag:<x>,<y>".
     * @returns {boolean} Whether the panel has such a title bar.
     */
    Q_INVOKABLE bool dockAction(const QString& objectName, const QString& action);

    /**jsdoc
     * Runs a line in the scripting console's panel as if it were typed there (ui/StandAloneJSConsole.h).
     * @function Test.consoleRun
     * @param {string} command - The line.
     * @returns {boolean} Whether the console's panel exists.
     */
    Q_INVOKABLE bool consoleRun(const QString& command);

    /**jsdoc
     * Does what View > Layouts does (ui/WindowLayout.h), without its popups: "save" (under the name), "apply", "forget",
     * "reset" (Reset to Default), or "names".
     * @function Test.windowLayoutAction
     * @param {string} action - "save", "apply", "forget", "reset" or "names".
     * @param {string} [name=""] - The layout's name.
     * @returns {string[]} The current mode's saved layouts' names, after the action.
     */
    Q_INVOKABLE QStringList windowLayoutAction(const QString& action, const QString& name = QString());

    /**jsdoc
     * Finds a button on the docked bar (ui/SystemToolbar.h) by its text, and clicks it as the mouse would.
     * @function Test.barButton
     * @param {string} text - The button's text, such as "People".
     * @param {boolean} [click=false] - Whether to click it.
     * @returns {object} <code>{ active, badge, x, y, width, height, barWidth, barHeight }</code>: whether it shows
     *     active (before the click's effect), the count on its red badge (0 for none), its cell in the bar and the bar's
     *     size; empty when there's no such button.
     */
    Q_INVOKABLE QVariantMap barButton(const QString& text, bool click = false);

    /**jsdoc
     * The words one of Darkride's QML panels shows now (ui/QmlDock.h): every text in it that's shown, in order.
     * @function Test.panelTexts
     * @param {string} name - The panel, such as "People".
     * @returns {string[]} The texts; empty when the panel hasn't been made or is closed.
     */
    Q_INVOKABLE QStringList panelTexts(const QString& name);

    /**jsdoc
     * Saves a picture of one dock panel as its widgets draw it, floating or docked, its title bar too.
     * @function Test.saveDockPicture
     * @param {string} objectName - The panel's object name, such as "ChatDock".
     * @param {string} path - Where to save it (PNG).
     * @returns {boolean} Whether it was saved.
     */
    Q_INVOKABLE bool saveDockPicture(const QString& objectName, const QString& path);

    /**jsdoc
     * Opens an entity's script in the Code panel, as its Edit button in the Inspector does (Qt 6 builds only).
     * @function Test.openInCode
     * @param {Uuid} id - The entity.
     * @param {string} which - <code>"script"</code> or <code>"serverScripts"</code>.
     * @returns {boolean} Whether it opened.
     */
    Q_INVOKABLE bool openInCode(const QUuid& id, const QString& which);

    /**jsdoc
     * The Code panel's document as the panel shows it: <code>hasDocument, pageReady, fileName, kind, where, canSave,
     * readOnlyReason, problem, saving, savedText, note, status, statusState, errorLine, errorText, claudeWriting,
     * showingChange, nearby</code> (Code near you's row count), <code>rows</code> (its rows), <code>path, usedBy, sharedBy, isShader, language,
     * openKey, loading, drafts</code> (how many files have an unsaved change kept).
     * @function Test.codePanel
     * @returns {object} The panel's state.
     */
    Q_INVOKABLE QVariantMap codePanel();

    /**jsdoc
     * Runs JavaScript in the Code panel's page, as a test probe (the page's own content security policy applies to
     * what it does); its result comes later from Test.codePageAnswer.
     * @function Test.codePageRun
     * @param {string} source - The JavaScript.
     * @returns {number} The run's number; 0 when the page isn't there.
     */
    Q_INVOKABLE int codePageRun(const QString& source);

    /**jsdoc
     * A Test.codePageRun's result, as JSON text, once the page has answered.
     * @function Test.codePageAnswer
     * @param {number} run - The run's number.
     * @returns {string|null} The result as JSON, or <code>null</code> until it has come.
     */
    Q_INVOKABLE QVariant codePageAnswer(int run);

    /**jsdoc
     * Presses the Code panel's Save, as the person's click does.
     * @function Test.codeSave
     */
    Q_INVOKABLE void codeSave();

    /**jsdoc
     * Opens the Code panel on Code near you alone, unfolded, with no script open (a timing run's case).
     * @function Test.codeShowNearby
     */
    Q_INVOKABLE void codeShowNearby();

    /**jsdoc
     * Clicks a row of the Code panel's list, as the person does: the open file's unsaved typing is kept first.
     * @function Test.codeOpenRow
     * @param {Uuid} id - The row's entity (the nearest thing using the file).
     * @param {string} which - <code>"script"</code>, <code>"serverScripts"</code>, <code>"fragmentShader"</code> or
     *     <code>"vertexShader"</code>.
     */
    Q_INVOKABLE void codeOpenRow(const QUuid& id, const QString& which);

    /**jsdoc
     * Unfolds or folds the Code panel's list, as its sidebar button does.
     * @function Test.codeShowList
     * @param {boolean} shown - Whether the list shows.
     */
    Q_INVOKABLE void codeShowList(bool shown);

    /**jsdoc
     * Makes the Code panel act as if in another world (<code>""</code>: the real one again), as arriving there does:
     * the open document is cleared, and unsaved changes are kept per world.
     * @function Test.codeSetWorld
     * @param {string} name - A name for the stand-in world, or <code>""</code>.
     */
    Q_INVOKABLE void codeSetWorld(const QString& name);

    /**jsdoc
     * The Make dock as its QML sees it (Qt 6 builds; make/MakeDock.h): <code>available, dockMade, open, floating, area,
     * state, reason, route, hasKey, claudeFound, blenderFound, blenderSealed, blenderWhy, keyStoreName, working,
     * timeline, made</code> (row counts), <code>timelineRows, madeRows, watching</code> (its window is watched for real
     * input), <code>provider</code> (its engine has the "make" picture provider), <code>windowActive</code> (the dock's
     * Qt Quick window), <code>dockWindowActive</code> (the dock widget's window), <code>folder</code> (the Make folder).
     * @function Test.makeDock
     * @returns {object} The dock's state.
     */
    Q_INVOKABLE QVariantMap makeDock();

    /**jsdoc
     * Calls one of the Make dock's real-input calls from a test, as its QML would but with no person's input: it must
     * be refused. <code>"setRoute"</code> (to the route it isn't on) or <code>"answer"</code> (a question that was never
     * asked); <code>"events:setRoute"</code> first gives the dock's window a mouse release and a key press made by the
     * test (not by the window system). Only calls whose doing would change nothing outside the client's own settings.
     * @function Test.makeDockTry
     * @param {string} call - The call.
     * @returns {boolean} The call's answer: <code>true</code> means it was NOT refused.
     */
    Q_INVOKABLE bool makeDockTry(const QString& call);

    /**jsdoc
     * Gives the Make dock a made card as Make's link does when Claude's tools make something (MakeLink::made):
     * <code>{ kind: "script", id, side, name, file }</code> or <code>{ kind: "thing", id, name }</code>.
     * @function Test.makeDockCard
     * @param {number} request - The request it belongs to.
     * @param {object} card - The card.
     */
    Q_INVOKABLE void makeDockCard(int request, const QVariantMap& card);

    /**jsdoc
     * Does what a script card's Open does in the Make dock: the card's script in the Code panel.
     * @function Test.makeDockOpenCard
     * @param {string} cardId - The card's item id (Test.makeDock's madeRows).
     */
    Q_INVOKABLE void makeDockOpenCard(const QString& cardId);

    /**jsdoc
     * The file the Make dock's picture provider would serve for <code>image://make/&lt;what&gt;/&lt;id&gt;</code>, or
     * <code>""</code> when it would serve none (MakeDock::servedFile).
     * @function Test.makeDockServed
     * @param {string} what - <code>"sheet"</code>, <code>"thumb"</code> or <code>"picture"</code>.
     * @param {string} id - The picture's or model's id.
     * @returns {string} The file's path, or <code>""</code>.
     */
    Q_INVOKABLE QString makeDockServed(const QString& what, const QString& id);

    /**jsdoc
     * Connects the Make dock on the Terminal route without the person's click, for a kit that then speaks to Make as
     * Claude Code would (scripting's mcp-kit.py). From then on every question Claude asks is answered no. The key route
     * is never connected this way. <code>"chat"</code> connects with the chat line for this run only (the person's own
     * choice isn't changed), whether or not the connect screen offers chat yet.
     * @function Test.makeDockConnect
     * @param {string} route - <code>"terminal"</code> or <code>"chat"</code>; anything else is refused.
     * @returns {boolean} Whether Make is listening.
     */
    Q_INVOKABLE bool makeDockConnect(const QString& route);

    /**jsdoc
     * An item of the Make dock's view, found by its objectName: whether it's shown, its place in the dock's window,
     * its text and selected text (a text item's).
     * @function Test.makeDockItem
     * @param {string} name - The item's objectName (e.g. <code>"makeStartLine"</code>, <code>"makeCopy"</code>).
     * @returns {object} <code>{ found, visible, x, y, width, height, text, selectedText }</code>.
     */
    Q_INVOKABLE QVariantMap makeDockItem(const QString& name);

    /**jsdoc
     * A press, a drag and a release of the left button in the Make dock's window, over an item found by its objectName,
     * from and to points given as fractions of the item's width and height (a click when they're the same). Sent as
     * Qt events, not the system's: MakeDock's real-input check refuses any call they lead to, so a kit sees whether
     * the click reached its target (the refusal's log line) without anything happening.
     * @function Test.makeDockMouse
     * @param {string} name - The item's objectName.
     * @param {number} fromX - From, 0 to 1 across the item.
     * @param {number} fromY - From, 0 to 1 down the item.
     * @param {number} toX - To, 0 to 1 across the item.
     * @param {number} toY - To, 0 to 1 down the item.
     * @returns {boolean} Whether the item was found and shown, and the events were sent.
     */
    Q_INVOKABLE bool makeDockMouse(const QString& name, double fromX, double fromY, double toX, double toY);

    /**jsdoc
     * Shows the Make badge (the view's bottom right) in one state without a connected Make, whatever the Make panel,
     * for a timing run or a picture; <code>""</code> gives it back to Make.
     * @function Test.makeBadge
     * @param {string} state - <code>"working"</code>, <code>"answer"</code>, <code>"done"</code>,
     *     <code>"failed"</code>, <code>"off"</code> (hidden) or <code>""</code>.
     * @returns {string} The badge's state now, or <code>"unavailable"</code> without the Make dock.
     */
    Q_INVOKABLE QString makeBadge(const QString& state);

    /**jsdoc
     * The Make badge as the view's QML has it, for a kit's check that it shows and spins:
     * <code>{ found, visible, words, lead, badgeState }</code> (lead: the spinner's step, 0 to 11, one step a second
     * while it spins).
     * @function Test.makeBadgeItem
     * @returns {object} The badge item's values, or <code>{ found: false }</code>.
     */
    Q_INVOKABLE QVariantMap makeBadgeItem();

    /**jsdoc
     * Copy as the Make dock's Copy does (MakeDock.copyLine), without its real-input check: writes the system's clipboard,
     * which is the person's, so only in a run that has a clipboard slot.
     * @function Test.makeDockCopy
     * @param {string} which - <code>"start"</code> (the start line) or <code>"setup"</code>.
     * @returns {string} The text put on the clipboard, or <code>""</code>.
     */
    Q_INVOKABLE QString makeDockCopy(const QString& which);

    /**jsdoc
     * Saves a picture of the Code panel's menu, drawn as it would show but without showing it.
     * @function Test.saveCodeMenuPicture
     * @param {string} which - <code>"more"</code> (the &bull;&bull;&bull; menu).
     * @param {string} path - Where to save it (PNG).
     * @returns {boolean} Whether it was saved.
     */
    Q_INVOKABLE bool saveCodeMenuPicture(const QString& which, const QString& path);

    /**jsdoc
     * Shows a count on the bar's Chat button's badge (ui/ChatButton.h), as chat's unread count would, until chat's
     * next count.
     * @function Test.setChatBadge
     * @param {number} count - 0 shows none.
     * @returns {boolean} Whether the bar has a Chat button.
     */
    Q_INVOKABLE bool setChatBadge(int count);

    /**jsdoc
     * This world in chat (chat/WorldChat.h): its lines and unseen count, whether the chat panel is open, and the
     * widget that has the keyboard (its object name, and its text when it is chat's message box).
     * @function Test.worldChat
     * @returns {object} <code>{items, unread, chatOpen, focus, focusText}</code>
     */
    Q_INVOKABLE QVariantMap worldChat();

    /**jsdoc
     * Keys, as if typed: to the 3D view (as Enter or "/" pressed there), or to the widget that has the keyboard (chat's
     * message box). "\n" is Return, "\u001b" Escape.
     * @function Test.typeKeys
     * @param {string} text - The keys.
     * @param {boolean} [toView=false] - To the 3D view.
     */
    Q_INVOKABLE void typeKeys(const QString& text, bool toView = false);

    /**jsdoc
     * Answers the next of Darkride's alerts (ui/Popup.h) to open within 15 s, as a click would: after it has drawn,
     * saves its picture, types into its field, and presses the named button. Call it before what opens the alert
     * (an alert waits for its answer, so a call that opens one returns only once it is answered).
     * @function Test.answerNextAlert
     * @param {string} button - The label of the button to press.
     * @param {string} [fieldText=""] - What to type into the alert's field first, if it has one.
     * @param {string} [picturePath=""] - Where to save the alert's picture (PNG), if anywhere.
     */
    Q_INVOKABLE void answerNextAlert(const QString& button, const QString& fieldText = QString(),
                                     const QString& picturePath = QString());

    /**jsdoc
     * Answers the next text prompt (Window.prompt's) with the given text, as its OK button would, once it is open: for
     * a script that then asks, and waits.
     * @function Test.answerNextPrompt
     * @param {string} text - The answer.
     */
    Q_INVOKABLE void answerNextPrompt(const QString& text);

    /**jsdoc
     * Shows one of Darkride's alerts (ui/Popup.h) with Darkride's logo, and waits for its answer: a look at the alert's
     * design without the action that asks it. Answer it with Test.answerNextAlert, called first.
     * @function Test.showAlert
     * @param {string} title - The bold line.
     * @param {string} text - The short line under it.
     * @param {string} details - What the Details fold-out holds ("" for none).
     * @param {object[]} buttons - Each <code>{ label, role }</code>, role "default", "cancel", "plain" or
     *     "destructive".
     * @param {string} [fieldPlaceholder] - Given, the alert has a field with this placeholder.
     * @returns {number} The pressed button's place in the list, -1 for none.
     */
    Q_INVOKABLE int showAlert(const QString& title, const QString& text, const QString& details,
                              const QVariantList& buttons, const QString& fieldPlaceholder = QString());

    /**jsdoc
     * Chooses a control point of the Track selected on its own in the editor (-1 for none), as a click on its diamond
     * does: bright in the viewport, the Move gizmo on it, the Inspector's Points list on its row.
     * @function Test.chooseTrackPoint
     * @param {number} index - The point's index, from 0.
     */
    Q_INVOKABLE void chooseTrackPoint(int index);

    /**jsdoc
     * @function Test.chosenTrackPoint
     * @returns {number} The chosen control point's index, -1 for none.
     */
    Q_INVOKABLE int chosenTrackPoint();

    /**jsdoc
     * Inserts a control point on the selected Track's curve at an arc length, as Shift+click on the curve does.
     * @function Test.insertTrackPoint
     * @param {number} arcLength - Metres along the curve from its first point.
     */
    Q_INVOKABLE void insertTrackPoint(float arcLength);

    /**jsdoc
     * Removes the selected Track's chosen control point, as Delete does.
     * @function Test.deleteTrackPoint
     */
    Q_INVOKABLE void deleteTrackPoint();

    /**jsdoc
     * Undoes the editor's last step, as Cmd+Z does in build mode.
     * @function Test.editorUndo
     */
    Q_INVOKABLE void editorUndo();

    /**jsdoc
     * Puts an installed skin on (ui/Skins.h), or Darkride's own look with "", as the Skins panel does, so a scripted
     * test can picture build mode's panels under a skin and back.
     * @function Test.setSkin
     * @param {string} id - The installed skin's id (its folder's name under the app data's skins/), or "".
     * @returns {string} "" when it is on, else what is wrong with it.
     */
    Q_INVOKABLE QString setSkin(const QString& id);

    /**jsdoc
     * The colour an entity's row in build mode's Hierarchy is drawn in: red for one that couldn't load, grey for one
     * listed but not loaded yet (both from the skin while one is on), "" for the panel's own colour.
     * @function Test.hierarchyRowColor
     * @param {Uuid} id - The entity.
     * @returns {string} "#rrggbb", or "".
     */
    Q_INVOKABLE QString hierarchyRowColor(const QUuid& id);

    /**jsdoc
     * Imports an entities file into the world as Build > Import Entities… does once the file is chosen: placed under
     * the cursor (or in front of the camera), selected, one undo step; build mode comes on for it.
     * @function Test.editorImportEntities
     * @param {string} url - The file's URL (file:///…, https://…, atp:…).
     * @returns {boolean} Whether any entities were made.
     */
    Q_INVOKABLE bool editorImportEntities(const QString& url);

    /**jsdoc
     * Exports the editor's selection and its descendants as Build > Export Selected Entities… does once the path is
     * chosen.
     * @function Test.editorExportSelection
     * @param {string} path - The file to write (.json).
     * @returns {number} How many entities were written, -1 for none.
     */
    Q_INVOKABLE int editorExportSelection(const QString& path);

    /**jsdoc
     * What build mode's Hierarchy says the entity could not load (its row red, these lines in its tooltip): its model,
     * image or textures that failed, with the reasons; empty when nothing has. Build mode checks once a second.
     * @function Test.editorLoadProblem
     * @param {Uuid} id - The entity.
     * @returns {string} The failures, one per line.
     */
    Q_INVOKABLE QString editorLoadProblem(const QUuid& id);

    /**jsdoc
     * Brings the sign-in screen up as signing out does: out of the world, the avatar hidden, the screen over the window.
     * Scripts go on running. For a test client, which skips the screen at start (--tokens) and isn't signed out.
     * @function Test.showSignInScreen
     */
    Q_INVOKABLE void showSignInScreen();

    /**jsdoc
     * Types into a text field of the desktop's QML panels (a SearchBar's "searchField"), as if typed: its text set.
     * @function Test.setPanelText
     * @param {string} objectName - The field's objectName; the first found in the desktop's panels.
     * @param {string} text - What it holds after.
     * @returns {boolean} Whether a field of that name was found.
     */
    Q_INVOKABLE bool setPanelText(const QString& objectName, const QString& text);

    /**jsdoc
     * Presses Return in a text field of the desktop's QML panels: its accepted signal.
     * @function Test.acceptPanelText
     * @param {string} objectName - The field's objectName.
     * @returns {boolean} Whether a field of that name was found.
     */
    Q_INVOKABLE bool acceptPanelText(const QString& objectName);

    /**jsdoc
     * Does what an item of build mode's Create menu does: makes the entity (or a ready-made family of them, a vehicle),
     * placed at the middle of the view, selected, one undo step.
     * @function Test.editorCreate
     * @param {string} key - The item's key in the catalogue (editor/EditorCreate.cpp): <code>"cube"</code>,
     *     <code>"vehicle-car"</code>, …
     */
    Q_INVOKABLE void editorCreate(const QString& key);

    /**jsdoc
     * Sets the Terrain tool's Random tick (a new angle for each stroke) as a click on it does. Build mode must be on.
     * @function Test.terrainRandomAngle
     * @param {boolean} on - Ticked or not.
     * @returns {object} <code>{ found, angleEnabled }</code>: whether the tick and the Angle field are there, and
     *     whether the Angle field can be changed after.
     */
    Q_INVOKABLE QVariantMap terrainRandomAngle(bool on);

    /**jsdoc
     * A stroke of the Terrain tool straight down onto the entity's middle, pressed and let go at once.
     * @function Test.terrainStroke
     * @param {Uuid} id - The terrain.
     * @returns {number} The angle (degrees) the stroke turned the brush's picture by, -1 when no terrain was there.
     */
    Q_INVOKABLE float terrainStroke(const QUuid& id);

    /**jsdoc
     * The angle (degrees) the Terrain tool's next stroke will turn the brush's picture by, which its outline shows.
     * @function Test.terrainNextAngle
     * @returns {number} The angle.
     */
    Q_INVOKABLE float terrainNextAngle();

    /**jsdoc
     * Uploads files from this computer into the Assets panel's folder, as its Upload… button does after its file
     * dialog. Build mode must be on.
     * @function Test.editorUploadFiles
     * @param {string[]} paths - The files' paths on this computer.
     */
    Q_INVOKABLE void editorUploadFiles(const QStringList& paths);

    /**jsdoc
     * What Build mode's status bar says now ("" for nothing).
     * @function Test.editorStatus
     * @returns {string} The status bar's text.
     */
    Q_INVOKABLE QString editorStatus();

    /**jsdoc
     * Imports an avatar file as File > Import Avatar… does after its file dialog. An .fst goes straight to the question
     * whether to wear and keep it, which waits for its answer: call Test.answerNextAlert first.
     * @function Test.importAvatar
     * @param {string} path - The file's path on this computer.
     */
    Q_INVOKABLE void importAvatar(const QString& path);

    /**jsdoc
     * Turns build mode on or off, as Build > Build Mode does.
     * @function Test.setBuildMode
     * @param {boolean} on - Whether build mode is on.
     */
    Q_INVOKABLE void setBuildMode(bool on);

    /**jsdoc
     * The build-mode Hierarchy's rows: how many in all and how many loaded, whether the entity server's list of every
     * entity has arrived, the list request's state, and the editor's selection.
     * @function Test.getHierarchyCounts
     * @returns {object} <code>{ entities, loaded, listed, state, selected }</code>; <code>state</code> is "none",
     *     "waiting", "answered" or "unanswered".
     */
    Q_INVOKABLE QVariantMap getHierarchyCounts();

    /**jsdoc
     * Does what a click on the entity's row in the build-mode Hierarchy does: selects it, or, for a row that is not
     * loaded, flies the build camera to it and selects it when it arrives.
     * @function Test.clickHierarchyRow
     * @param {Uuid} id - The entity.
     */
    Q_INVOKABLE void clickHierarchyRow(const QUuid& id);

    /**jsdoc
     * Does what a click on a build-mode Hierarchy row's eye, hand, lock or script mark does: hides it in this
     * builder's view only, makes it unpickable by their clicks in the 3D view, locks it for everyone, or turns its
     * scripts off for everyone (or the reverse), with everything under it, or alone as an Alt-click does.
     * @function Test.hierarchyToggle
     * @param {Uuid} id - The row's entity.
     * @param {string} which - <code>"hide"</code>, <code>"pick"</code>, <code>"lock"</code> or
     *     <code>"scripts"</code>.
     * @param {boolean} [onlyThis=false] - As an Alt-click: the row alone.
     * @returns {boolean} Whether the row was there and the toggle could change.
     */
    Q_INVOKABLE bool hierarchyToggle(const QUuid& id, const QString& which, bool onlyThis = false);

    /**jsdoc
     * A build-mode Hierarchy row's toggles and what they amount to.
     * @function Test.hierarchyToggles
     * @param {Uuid} id - The row's entity.
     * @returns {object} <code>{ hidden, noPick, locked, scriptsOff, drawn, hiddenCount, note, scriptsOffReminder
     *     }</code>: hidden in this view, unpickable, locked, its scripts off, whether the entity is drawn here, how
     *     many of this world's rows are hidden here, the note under the Hierarchy, and the reminder shown when build
     *     mode last closed with scripts off ("" for none).
     */
    Q_INVOKABLE QVariantMap hierarchyToggles(const QUuid& id);

    /**jsdoc
     * A stroke along a build-mode Hierarchy toggle's column, as the mouse makes it: a press on the first row's toggle,
     * a move over each row, a release on the last. Every row passed takes the first row's new state, one undo step.
     * @function Test.hierarchyStroke
     * @param {string} which - "hide", "pick", "lock" or "scripts".
     * @param {Uuid[]} ids - The rows, in the order the stroke passes them (all in sight).
     * @param {boolean} [onlyThis=false] - As with Alt held: the rows alone, not what's under them.
     * @returns {boolean} <code>false</code> when a row isn't in sight.
     */
    Q_INVOKABLE bool hierarchyStroke(const QString& which, const QVariantList& ids, bool onlyThis = false);

    /**jsdoc
     * Does what a click on the Hierarchy note's Show All does.
     * @function Test.hierarchyShowAll
     */
    Q_INVOKABLE void hierarchyShowAll();

    /**jsdoc
     * What a click in build mode's viewport at the entity's middle would select.
     * @function Test.editorPickAt
     * @param {Uuid} id - The entity.
     * @returns {Uuid} The entity picked there, null for none.
     */
    Q_INVOKABLE QUuid editorPickAt(const QUuid& id);

    /**jsdoc
     * What a marquee over build mode's whole viewport would select.
     * @function Test.editorMarqueeIds
     * @returns {Uuid[]} The entities.
     */
    Q_INVOKABLE QVariantList editorMarqueeIds();

    /**jsdoc
     * Edits a property of the editor's selection as its Inspector field does when a list's + button or a field commits:
     * the undo point taken, the value sent, one step recorded.
     * @function Test.commitInspectorEdit
     * @param {string} key - The Inspector's key for the property (e.g. "trackPoints").
     * @param {string|number|boolean} value - The value as the field holds it.
     */
    Q_INVOKABLE void commitInspectorEdit(const QString& key, const QVariant& value);

    /**jsdoc
     * Starts tracing an entity as it is drawn: one sample per rendered frame, taken on the render thread once the frame's
     * scene is in place, of the frame's camera position and the centre of the entity's drawn bound. A null id stops it.
     * @function Test.traceRenderedEntity
     * @param {Uuid} entityID - The entity.
     * @returns {boolean} Whether the entity has a render item to trace.
     */
    Q_INVOKABLE bool traceRenderedEntity(const QUuid& entityID);

    /**jsdoc
     * Logs every main-view draw of a model part for the next frames (INSTANCING-DUMP lines: the model file, mesh, part,
     * level of detail, indices and each instance's position), to diff a frame with GPU instancing on against one with
     * it off (tests-manual/instancing/diff-dump.py).
     * @function Test.dumpDraws
     * @param {number} frames - How many frames to log.
     */
    Q_INVOKABLE void dumpDraws(int frames);

    /**jsdoc
     * Takes the samples traced so far: each is [time in ms, camera x, y, z, item x, y, z, frame index].
     * @function Test.takeRenderTrace
     * @returns {Array<Array<number>>} The samples, oldest first.
     */
    Q_INVOKABLE QVariantList takeRenderTrace();

    /**jsdoc
     * Starts or stops recording the presents: one sample per present, taken on the present thread once the swap has
     * returned (with vsync on, at the display's refresh). Starting clears what was recorded.
     * @function Test.tracePresents
     * @param {boolean} on - Whether to record.
     */
    Q_INVOKABLE void tracePresents(bool on);

    /**jsdoc
     * Takes the presents recorded so far: each is [time in ms, the rendered frame's index (as in the render trace),
     * 1 if that frame was new or 0 if the last frame was shown again, the present's whole duration in ms, the swap's
     * own duration in ms (its wait for the display, with vsync), the GPU's averaged time per frame in ms].
     * @function Test.takePresentTrace
     * @returns {Array<Array<number>>} The samples, oldest first.
     */
    Q_INVOKABLE QVariantList takePresentTrace();

    /**jsdoc
     * Starts or stops the frame trace (FrameTrace.h; team/performance/INSTRUMENTS-PLAN.md item 1): a ring buffer of the
     * last frames (about 14 s), each a record of the named numbers the engine's instruments write while it is on (the
     * frame's own times, the physics step, the camera boom, the avatar's carry in a vehicle, the entity render-update
     * budget, the presents); off, each writer pays one check. Starting drops the records held so far. Thread-safe by
     * construction, so no hop to the game loop.
     * @function Test.frameTrace
     * @param {boolean} on - Whether to record.
     */
    Q_INVOKABLE void frameTrace(bool on);

    /**jsdoc
     * Writes the frame trace's records as a CSV: a header of "frame", "t_us" and one column per name written, then one
     * line per frame with blank cells where that frame has no value. The records stay.
     * @function Test.dumpFrameTrace
     * @param {string} path - The file to write.
     * @returns {number} The rows written, or -1 when the file could not be written.
     */
    Q_INVOKABLE int dumpFrameTrace(const QString& path);

    /**jsdoc
     * Takes the frame trace's records: the column names first, then one row per frame, oldest first, in that order (a
     * cell is a number, a string, or undefined where the frame has no value: test it with <code>cell == null</code>).
     * The records are cleared in the same step; the trace stays on or off as it was.
     * @function Test.takeFrameTrace
     * @returns {Array<Array<number|string>>} [columns, row, row, ...].
     */
    Q_INVOKABLE QVariantList takeFrameTrace();

    /**jsdoc
     * Starts or stops recording the display's refreshes (macOS: the display link's callbacks, each with the instant
     * of the refresh it announces). Starting clears what was recorded.
     * @function Test.traceRefreshes
     * @param {boolean} on - Whether to record.
     */
    Q_INVOKABLE void traceRefreshes(bool on);

    /**jsdoc
     * Takes the refreshes recorded so far: the time of each in ms, on the clock of the present trace.
     * @function Test.takeRefreshTrace
     * @returns {Array<number>} The refresh times, oldest first; empty where the display gives no signal.
     */
    Q_INVOKABLE QVariantList takeRefreshTrace();

    /**jsdoc
     * The refresh rate of the display the window is on, in Hz, as the platform reports it (0 when it does not).
     * @function Test.getDisplayRefreshRate
     * @returns {number} The refresh rate.
     */
    Q_INVOKABLE float getDisplayRefreshRate();

    /**jsdoc
     * Whether the swap waits for the display's refresh (Render.vsyncEnabled, or TIVOLI_VSYNC in the environment).
     * @function Test.isVsyncEnabled
     * @returns {boolean} Whether vsync is on.
     */
    Q_INVOKABLE bool isVsyncEnabled();

    /**jsdoc
     * Makes every present cost this many microseconds more, busy-waited, as a heavy scene's would: for measuring the frame
     * pacing when presents overrun their period. 0 is none. TIVOLI_PRESENT_LOAD_US in the environment sets it at start.
     * @function Test.setPresentLoad
     * @param {number} microseconds - The extra time each present takes.
     */
    Q_INVOKABLE void setPresentLoad(int microseconds);

    /**jsdoc
     * @function Test.getPresentPeriod
     * @returns {number} The period the presents are paced at, in seconds, as the physics reads it; 0 while they are not
     *     paced (vsync, an HMD, or a schedule that restarted after a present overran).
     */
    Q_INVOKABLE float getPresentPeriod();

    /**jsdoc
     * Holds the present loop off for a while, as a stall would: nothing is presented, so no new frames are made. The
     * deadlock watchdog's report names the loop as held for a test once a second has passed.
     * @function Test.holdPresents
     * @param {number} milliseconds - How long; 0 lets presents go at once.
     */
    Q_INVOKABLE void holdPresents(int milliseconds);

    /**jsdoc
     * Drags the editor's selection as the gizmo would, from a script: turns build mode on, and with begin captures the
     * selection as a press on the gizmo does; every call applies the move or the rotation (a roll about the selection's
     * own centre) as the pointer's motion does; final takes the undo step as the release does.
     * @function Test.editorDrag
     * @param {Vec3} delta - The move, in metres (ignored when the rotation is not zero).
     * @param {number} yawDegrees - A rotation about the vertical.
     * @param {number} rollDegrees - A rotation about the forward axis.
     * @param {boolean} begin - The first call of a drag.
     * @param {boolean} final - The last call of a drag.
     */
    Q_INVOKABLE void editorDrag(const glm::vec3& delta, float yawDegrees, float rollDegrees, bool begin, bool final);

    /**jsdoc
     * Scales the editor's selection as a drag on the gizmo's scale handles would, from a script: as
     * {@link Test.editorDrag}, begin captures the selection and final takes the undo step.
     * @function Test.editorScaleDrag
     * @param {Vec3} scale - The scale so far, along the gizmo's axes (1 is the size the drag began at).
     * @param {boolean} begin - The first call of a drag.
     * @param {boolean} final - The last call of a drag.
     */
    Q_INVOKABLE void editorScaleDrag(const glm::vec3& scale, bool begin, bool final);

private:
    bool waitForCondition(qint64 maxWaitMs, std::function<bool()> condition);
    QString _testResultsLocation;
};

#endif  // hifi_TestScriptingInterface_h
