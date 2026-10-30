//
// snapshot.js
//
// Created by David Kelly on 1 August 2016
// Copyright 2016 High Fidelity, Inc
//
// Distributed under the Apache License, Version 2.0
// See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
/* globals Tablet, Script, HMD, Settings, DialogsManager, Menu, Reticle,
    OverlayWebWindow, Desktop, Account, MyAvatar, Snapshot */
/* eslint indent: ["error", 4, { "outerIIFEBody": 0 }] */

(function () { // BEGIN LOCAL_SCOPE
Script.include("/~/system/libraries/accountUtils.js");
var AppUi = Script.require('appUi');

var SNAPSHOT_DELAY = 500; // 500ms
var FINISH_SOUND_DELAY = 350;
var resetOverlays;
var resetNametags;
var reticleVisible;

var snapshotOptions = {};
var imageData = [];
var storyIDsToMaybeDelete = [];
var shareAfterLogin = false;
var snapshotToShareAfterLogin = [];
var METAVERSE_BASE = Account.metaverseServerURL;
var isLoggedIn;
var mostRecentGifSnapshotFilename = "";
var mostRecentStillSnapshotFilename = "";
// Where each snap went after it was taken (Snapshot.snapshotStored), by the path it was taken at: its URL in My Files,
// or its path on this computer and why it stayed there.
var storedSnaps = {};

// It's totally unnecessary to return to C++ to perform many of these requests, such as DELETEing an old story,
// POSTING a new one, PUTTING a new audience, or GETTING story data. It's far more efficient to do all of that within JS
var request;

try {
    // Due to an issue where if the user spams 'script reload', this call could cause an exception
    // preventing our scriptEnding to not properly be initialized resulting in the tablet button
    // duplicating itself where you end up with a bunch of SNAP buttons on your toolbar
    request = Script.require('request').request;
} catch(err) {
    print('Failed to resolve request api, error: ' + err);
}

function removeFromStoryIDsToMaybeDelete(story_id) {
    story_id = parseInt(story_id);
    if (storyIDsToMaybeDelete.indexOf(story_id) > -1) {
        storyIDsToMaybeDelete.splice(storyIDsToMaybeDelete.indexOf(story_id), 1);
    }
    print('storyIDsToMaybeDelete[] now:', JSON.stringify(storyIDsToMaybeDelete));
}

// the Settings panel's General tab, with its Snapshots section, as Settings > General... opens it (the old General
// Settings window until 10-28)
function openSnapSettings() {
    Menu.triggerOption("General...");
}

function fileExtensionMatches(filePath, extension) {
    return filePath.split('.').pop().toLowerCase() === extension;
}

function getFilenameFromPath(str) {
    return str.split('\\').pop().split('/').pop();
}

function onMessage(message) {
    // Receives message from the html dialog via the qwebchannel EventBridge. This is complicated by the following:
    // 1. Although we can send POJOs, we cannot receive a toplevel object. (Arrays of POJOs are fine, though.)
    // 2. Although we currently use a single image, we would like to take snapshot, a selfie, a 360 etc. all at the
    //    same time, show the user all of them, and have the user deselect any that they do not want to share.
    //    So we'll ultimately be receiving a set of objects, perhaps with different post processing for each.

    if (message.type !== "snapshot") {
        return;
    }

    switch (message.action) {
        case 'ready': // DOM is ready and page has loaded
            ui.sendMessage({
                type: "snapshot",
                action: "captureSettings",
                setting: Settings.getValue("alsoTakeAnimatedSnapshot", true)
            });
            ui.sendMessage({
                type: "snapshot",
                action: "nametagsSettings",
                setting: Settings.getValue("shapshotNametagsEnabled", true)
            });
            // signed in, snaps go to My Files: no folder on this computer is needed
            if (Snapshot.getSnapshotsLocation() !== "" || Account.isLoggedIn()) {
                isDomainOpen(Settings.getValue("previousSnapshotDomainID"), function (canShare) {
                    ui.sendMessage({
                        type: "snapshot",
                        action: "showPreviousImages",
                        options: snapshotOptions,
                        image_data: imageData,
                        canShare: canShare
                    });
                });
            } else {
                ui.sendMessage({
                    type: "snapshot",
                    action: "showSetupInstructions"
                });
                Settings.setValue("previousStillSnapPath", "");
                Settings.setValue("previousStillSnapStoryID", "");
                Settings.setValue("previousStillSnapBlastingDisabled", false);
                Settings.setValue("previousStillSnapHifiSharingDisabled", false);
                Settings.setValue("previousStillSnapUrl", "");
                Settings.setValue("previousAnimatedSnapPath", "");
                Settings.setValue("previousAnimatedSnapStoryID", "");
                Settings.setValue("previousAnimatedSnapBlastingDisabled", false);
                Settings.setValue("previousAnimatedSnapHifiSharingDisabled", false);
            }
            updatePrintPermissions();
            break;
        case 'login':
            openLoginWindow();
            break;
        case 'chooseSnapshotLocation':
            Window.browseDirChanged.connect(snapshotDirChanged);
            Window.browseDirAsync("Choose Snapshots Directory", "", "");
            break;
        case 'openSettings':
            openSnapSettings();
            break;
        case 'captureStillAndGif':
            print("Changing Snapshot Capture Settings to Capture Still + GIF");
            Settings.setValue("alsoTakeAnimatedSnapshot", true);
            break;
        case 'captureStillOnly':
            print("Changing Snapshot Capture Settings to Capture Still Only");
            Settings.setValue("alsoTakeAnimatedSnapshot", false);
            break;
        case 'nametagsEnabled':
            Settings.setValue("shapshotNametagsEnabled", true);
            break;
        case 'nametagsDisabled':
            Settings.setValue("shapshotNametagsEnabled", false);
            break;
        case 'takeSnapshot':
            takeSnapshot();
            break;
        case 'shareSnapshotForUrl':
            isDomainOpen(Settings.getValue("previousSnapshotDomainID"), function (canShare) {
                var isGif = fileExtensionMatches(message.data, "gif");      
                isLoggedIn = Account.isLoggedIn();                        
                if (!isGif) {
                    isUploadingPrintableStill = canShare && Account.isLoggedIn();
                }
                if (canShare) {                  
                    if (isLoggedIn) {
                        print('Sharing snapshot with audience "for_url":', message.data);
                        Window.shareSnapshot(message.data, Settings.getValue("previousSnapshotHref"));                      
                        if (isGif) {
                            mostRecentGifSnapshotFilename = getFilenameFromPath(message.data);
                        } else {
                            mostRecentStillSnapshotFilename = getFilenameFromPath(message.data);
                        }
                    } else {
                        shareAfterLogin = true;
                        snapshotToShareAfterLogin.push({ path: message.data, href: Settings.getValue("previousSnapshotHref") });
                    }
                }
                updatePrintPermissions();
            });
            break;
        case 'blastToConnections':
            isLoggedIn = Account.isLoggedIn();
            if (isLoggedIn) {
                if (message.isGif) {
                    Settings.setValue("previousAnimatedSnapBlastingDisabled", true);
                } else {
                    Settings.setValue("previousStillSnapBlastingDisabled", true);
                }

                print('Uploading new story for announcement!');

                request({
                    uri: METAVERSE_BASE + '/api/v1/user_stories/' + message.story_id,
                    method: 'GET'
                }, function (error, response) {
                    if (error || (response.status !== 'success')) {
                        print("ERROR getting details about existing snapshot story:", error || response.status);
                        return;
                    } else {
                        var requestBody = {
                            user_story: {
                                audience: "for_connections",
                                action: "announcement",
                                path: response.user_story.path,
                                place_name: response.user_story.place_name,
                                thumbnail_url: response.user_story.thumbnail_url,
                                // For historical reasons, the server doesn't take nested JSON objects.
                                // Thus, I'm required to STRINGIFY what should be a nested object.
                                details: JSON.stringify({
                                    shareable_url: response.user_story.details.shareable_url,
                                    image_url: response.user_story.details.image_url
                                })
                            }
                        }
                        request({
                            uri: METAVERSE_BASE + '/api/v1/user_stories',
                            method: 'POST',
                            json: true,
                            body: requestBody
                        }, function (error, response) {
                            if (error || (response.status !== 'success')) {
                                print("ERROR uploading announcement story: ", error || response.status);
                                if (message.isGif) {
                                    Settings.setValue("previousAnimatedSnapBlastingDisabled", false);
                                } else {
                                    Settings.setValue("previousStillSnapBlastingDisabled", false);
                                }
                                return;
                            } else {
                                print("SUCCESS uploading announcement story! Story ID:", response.user_story.id);
                                removeFromStoryIDsToMaybeDelete(message.story_id); // Don't delete original "for_url" story
                            }
                        });
                    }
                });
            }
            break;
        case 'requestPrintButtonUpdate':
            updatePrintPermissions();
            break;
        case 'printToPolaroid':
            if (Entities.canRez() || Entities.canRezTmp()) {
               printToPolaroid(Settings.getValue("previousStillSnapUrl"));
               removeFromStoryIDsToMaybeDelete(Settings.getValue("previousStillSnapStoryID"));
            }
            break;
        case 'alertSnapshotLoadFailed':
            snapshotFailedToLoad = true;
            break;
        case 'shareSnapshotWithEveryone':
            isLoggedIn = Account.isLoggedIn();
            if (isLoggedIn) {
                if (message.isGif) {
                    Settings.setValue("previousAnimatedSnapHifiSharingDisabled", true);
                } else {
                    Settings.setValue("previousStillSnapHifiSharingDisabled", true);
                }
                print('Modifying audience of story ID', message.story_id, "to 'for_feed'");
                var requestBody = {
                    audience: "for_feed"
                }

                if (message.isAnnouncement) {
                    requestBody.action = "announcement";
                    print('...Also announcing!');
                }
                request({
                    uri: METAVERSE_BASE + '/api/v1/user_stories/' + message.story_id,
                    method: 'PUT',
                    json: true,
                    body: requestBody
                }, function (error, response) {
                    if (error || (response.status !== 'success')) {
                        print("ERROR changing audience: ", error || response.status);
                        if (message.isGif) {
                            Settings.setValue("previousAnimatedSnapHifiSharingDisabled", false);
                        } else {
                            Settings.setValue("previousStillSnapHifiSharingDisabled", false);
                        }
                        return;
                    } else {
                        print("SUCCESS changing audience" + (message.isAnnouncement ? " and posting announcement!" : "!"));
                        removeFromStoryIDsToMaybeDelete(message.story_id);
                    }
                });
            }
            break;
        case 'removeFromStoryIDsToMaybeDelete':
            removeFromStoryIDsToMaybeDelete(message.story_id);
            break;
        default:
            print('Unknown message action received by snapshot.js!');
            break;
    }
}

var POLAROID_PRINT_SOUND = SoundCache.getSound(Script.resourcesPath() + "sounds/snapshot/sound-print-photo.wav");
var POLAROID_MODEL_URL = 'http://hifi-content.s3.amazonaws.com/alan/dev/Test/snapshot.fbx';
var POLAROID_RATE_LIMIT_MS = 1000;
var polaroidPrintingIsRateLimited = false;

// force call the gotoPreviousApp on script thead to load snapshot html page.
var tablet = Tablet.getTablet("com.highfidelity.interface.tablet.system");
tablet.fromQml.connect(function(message) {
    if (message === 'returnToPreviousApp') {
	tablet.returnToPreviousApp();
    }
});

function printToPolaroid(image_url) {
    // Rate-limit printing
    if (polaroidPrintingIsRateLimited) {
        return;
    }
    polaroidPrintingIsRateLimited = true;
    Script.setTimeout(function () {
        polaroidPrintingIsRateLimited = false;
    }, POLAROID_RATE_LIMIT_MS);

    var polaroid_url = image_url;                  

    var model_pos = Vec3.sum(MyAvatar.position, Vec3.multiply(1.25, Quat.getForward(MyAvatar.orientation)));
    model_pos.y += 0.39; // Print a bit closer to the head
    
    var model_q1 = MyAvatar.orientation;
    var model_q2 = Quat.angleAxis(90, Quat.getRight(model_q1));
    var model_rot = Quat.multiply(model_q2, model_q1);
    
    var properties = {                                
        "type": 'Model',
        "shapeType": 'box',

        "name": "Snapshot by " + MyAvatar.sessionDisplayName,
        "description": "Printed from SNAP app",                               
        "modelURL": POLAROID_MODEL_URL,

        "dimensions": { "x": 0.5667, "y": 0.042, "z": 0.4176 },
        "position": model_pos,
        "rotation": model_rot,

        "textures": JSON.stringify( { "tex.picture": polaroid_url } ),

        "density": 200,
        "restitution": 0.15,                            
        "gravity": { "x": 0, "y": -2.0, "z": 0 },
        "damping": 0.45,

        "dynamic": true, 
        "collisionsWillMove": true,

        "grab": { "grabbable": true }
    };
    
    var polaroid = Entities.addEntity(properties);
    Audio.playSound(POLAROID_PRINT_SOUND, {
        position: model_pos,
        localOnly: false,
        volume: 0.1
    });
}

function fillImageDataFromPrevious() {
    isLoggedIn = Account.isLoggedIn();
    var previousStillSnapPath = Settings.getValue("previousStillSnapPath");
    var previousStillSnapStoryID = Settings.getValue("previousStillSnapStoryID");
    var previousStillSnapBlastingDisabled = Settings.getValue("previousStillSnapBlastingDisabled");
    var previousStillSnapHifiSharingDisabled = Settings.getValue("previousStillSnapHifiSharingDisabled");
    var previousAnimatedSnapPath = Settings.getValue("previousAnimatedSnapPath");
    var previousAnimatedSnapStoryID = Settings.getValue("previousAnimatedSnapStoryID");
    var previousAnimatedSnapBlastingDisabled = Settings.getValue("previousAnimatedSnapBlastingDisabled");
    var previousAnimatedSnapHifiSharingDisabled = Settings.getValue("previousAnimatedSnapHifiSharingDisabled");

    snapshotOptions = {
        containsGif: previousAnimatedSnapPath !== "",
        processingGif: false,
        shouldUpload: false,
        canBlast: snapshotDomainID === Settings.getValue("previousSnapshotDomainID") &&
            snapshotDomainID === location.domainID,
        isLoggedIn: isLoggedIn
    };
    imageData = [];
    if (previousStillSnapPath !== "") {
        imageData.push({
            localPath: previousStillSnapPath,
            takenPath: previousStillSnapPath,
            caption: snapCaption(previousStillSnapPath),
            story_id: previousStillSnapStoryID,
            blastButtonDisabled: previousStillSnapBlastingDisabled,
            hifiButtonDisabled: previousStillSnapHifiSharingDisabled,
            errorPath: Script.resourcesPath() + 'snapshot/img/no-image.jpg'
        });
    }
    if (previousAnimatedSnapPath !== "") {
        imageData.push({
            localPath: previousAnimatedSnapPath,
            takenPath: previousAnimatedSnapPath,
            caption: snapCaption(previousAnimatedSnapPath),
            story_id: previousAnimatedSnapStoryID,
            blastButtonDisabled: previousAnimatedSnapBlastingDisabled,
            hifiButtonDisabled: previousAnimatedSnapHifiSharingDisabled,
            errorPath: Script.resourcesPath() + 'snapshot/img/no-image.jpg'
        });
    }
}

function snapshotUploaded(isError, reply) {
    if (!isError) {
        var replyJson = JSON.parse(reply),
            storyID = replyJson.user_story.id,
            imageURL = replyJson.user_story.details.image_url,
            isGif = fileExtensionMatches(imageURL, "gif"),
            ignoreGifSnapshotData = false,
            ignoreStillSnapshotData = false;
        storyIDsToMaybeDelete.push(parseInt(storyID));
        print('storyIDsToMaybeDelete[] now:', JSON.stringify(storyIDsToMaybeDelete));
        if (isGif) {
            if (mostRecentGifSnapshotFilename !== replyJson.user_story.details.original_image_file_name) {
                ignoreGifSnapshotData = true;
            }
        } else {
            if (mostRecentStillSnapshotFilename !== replyJson.user_story.details.original_image_file_name) {
                ignoreStillSnapshotData = true;
            }
        }
        if ((isGif && !ignoreGifSnapshotData) || (!isGif && !ignoreStillSnapshotData)) {
            print('SUCCESS: Snapshot uploaded! Story with audience:for_url created! ID:', storyID);
            ui.sendMessage({
                type: "snapshot",
                action: "snapshotUploadComplete",
                story_id: storyID,
                image_url: imageURL,
            });
            if (isGif) {
                Settings.setValue("previousAnimatedSnapStoryID", storyID);
            } else {
                Settings.setValue("previousStillSnapStoryID", storyID);
                Settings.setValue("previousStillSnapUrl", imageURL);
            }
        } else {
            print('Ignoring snapshotUploaded() callback for stale ' + (isGif ? 'GIF' : 'Still' ) + ' snapshot. Stale story ID:', storyID);
        }
    }
    isUploadingPrintableStill = false;
}
// Where a snap is now: in My Files, or on this computer (where it was taken, or where it went when its upload failed).
function storedLocation(takenPath) {
    var stored = storedSnaps[takenPath];
    return stored ? stored.location : takenPath;
}

function snapCaption(path) {
    var stored = storedSnaps[path];
    if (!stored) {
        if (/^https?:/.test(path)) {
            return "Saved to My Files, in Snapshots";
        }
        // a snap in the folder snaps wait in for their upload (UPLOADS_DIRECTORY in Snapshot.cpp) is on its way
        return path.indexOf("Snapshot uploads") !== -1 ? "Saving to My Files\u2026" : "";
    }
    if (stored.inUserFiles) {
        return "Saved to My Files, in Snapshots";
    }
    if (stored.uploadError) {
        return "Saved on this computer, in " + stored.location.substring(0, stored.location.lastIndexOf("/")) +
            ". The upload to My Files failed: " + stored.uploadError;
    }
    return "Saved on this computer; sign in to keep Snaps in My Files";
}

function snapImageData(takenPath) {
    return { localPath: storedLocation(takenPath), takenPath: takenPath, caption: snapCaption(takenPath), href: href };
}

function onSnapshotStored(takenPath, location, inUserFiles, uploadError) {
    storedSnaps[takenPath] = { location: location, inUserFiles: inUserFiles, uploadError: uploadError };
    // Snap reopens with its last snaps from where they are now
    if (Settings.getValue("previousStillSnapPath") === takenPath) {
        Settings.setValue("previousStillSnapPath", location);
    }
    if (Settings.getValue("previousAnimatedSnapPath") === takenPath) {
        Settings.setValue("previousAnimatedSnapPath", location);
    }
    ui.sendMessage({
        type: "snapshot",
        action: "snapshotStored",
        takenPath: takenPath,
        location: location,
        caption: snapCaption(takenPath)
    });
    cameraStored(takenPath);
}

var href, snapshotDomainID;
var captureTarget = "tablet"; // who asked for the Snap being taken: "tablet" (the tablet page) or "camera"
function takeSnapshot(target) {
    captureTarget = target || "tablet";
    ui.sendMessage({
        type: "snapshot",
        action: "clearPreviousImages"
    });
    Settings.setValue("previousStillSnapPath", "");
    Settings.setValue("previousStillSnapStoryID", "");
    Settings.setValue("previousStillSnapBlastingDisabled", false);
    Settings.setValue("previousStillSnapHifiSharingDisabled", false);
    Settings.setValue("previousStillSnapUrl", "");
    Settings.setValue("previousAnimatedSnapPath", "");
    Settings.setValue("previousAnimatedSnapStoryID", "");
    Settings.setValue("previousAnimatedSnapBlastingDisabled", false);
    Settings.setValue("previousAnimatedSnapHifiSharingDisabled", false);
    
    // Since we are taking a snapshot, we should make the print button appear to be loading/processing
    snapshotFailedToLoad = false;
    isUploadingPrintableStill = true;
    updatePrintPermissions();

    // We will record snapshots based on the starting location. That could change, e.g., when recording a .gif.
    // Even the domainID could change (e.g., if the user falls into a teleporter while recording).
    href = location.href;
    Settings.setValue("previousSnapshotHref", href);
    snapshotDomainID = location.domainID;
    Settings.setValue("previousSnapshotDomainID", snapshotDomainID);

    maybeDeleteSnapshotStories();

    // update button states
    resetOverlays = Menu.isOptionChecked("Show Overlays"); // For completeness. Certainly true if the button is visible to be clicked.
    reticleVisible = Reticle.visible;
    Reticle.visible = false;
    if (!HMD.active) {
        Reticle.allowMouseCapture = false;
    }
    resetNametags = Render.getNametagsEnabled();
    if (Settings.getValue("shapshotNametagsEnabled", true)) {
        resetNametags = false;
    }

    var includeAnimated = Settings.getValue("alsoTakeAnimatedSnapshot", true);
    if (includeAnimated) {
        Window.processingGifStarted.connect(processingGifStarted);
    } else {
        Window.stillSnapshotTaken.connect(stillSnapshotTaken);
    }

    // hide overlays if they are on
    if (resetOverlays) {
        Menu.setIsOptionChecked("Show Overlays", false);
    }
    if (resetNametags) {
        Render.setNametagsEnabled(false);
    }

    var snapActivateSound = SoundCache.getSound(Script.resourcesPath() + "sounds/snapshot/snap.wav");

    if (captureTarget === "camera") {
        // the camera's windows are leaving the screen: a few frames for that, then the Snap
        var renderRate = Rates.render > 0 ? Rates.render : 30;
        Script.setTimeout(function () {
            Audio.playSound(snapActivateSound, {
                position: { x: MyAvatar.position.x, y: MyAvatar.position.y, z: MyAvatar.position.z },
                localOnly: true,
                volume: 0.6
            });
            Window.takeSnapshot(false, includeAnimated, CAMERA_ASPECT_RATIO);
        }, Math.max(CAMERA_CAPTURE_DELAY, 3000 / renderRate));
        return;
    }

    // take snapshot (with no notification)
    Script.setTimeout(function () {
        Audio.playSound(snapActivateSound, {
            position: { x: MyAvatar.position.x, y: MyAvatar.position.y, z: MyAvatar.position.z },
            localOnly: true,
            volume: 0.6
        });
        HMD.closeTablet();
        var DOUBLE_RENDER_TIME_TO_MS = 2000; // If rendering is bogged down, allow double the render time to close the tablet.
        Script.setTimeout(function () {
            Window.takeSnapshot(false, includeAnimated, 1.91);
        }, Math.max(SNAPSHOT_DELAY, DOUBLE_RENDER_TIME_TO_MS / Rates.render ));
    }, FINISH_SOUND_DELAY);
}
    
// High Fidelity's sharing asked its metaverse whether a world's "user stories" were open; Darkride's metaverse has no
// user stories, so the answer is always no and nothing is asked (the request failed on every VR snapshot; Caity 10-29)
function isDomainOpen(id, callback) {
    callback(false);
}

function stillSnapshotTaken(pathStillSnapshot, notify) {
    isLoggedIn = Account.isLoggedIn();
    // show hud
    Reticle.visible = reticleVisible;
    Reticle.allowMouseCapture = true;
    // show overlays if they were on
    if (resetOverlays) {
        Menu.setIsOptionChecked("Show Overlays", true);
    }
    if (resetNametags) {
        Render.setNametagsEnabled(true);
    }
    Window.stillSnapshotTaken.disconnect(stillSnapshotTaken);

    // A Snapshot Review dialog might be left open indefinitely after taking the picture,
    // during which time the user may have moved. So stash that info in the dialog so that
    // it records the correct href. (We can also stash in .jpegs, but not .gifs.)
    // last element in data array tells dialog whether we can share or not
    Settings.setValue("previousStillSnapPath", storedLocation(pathStillSnapshot));

    if (captureTarget === "camera") {
        cameraTaken(pathStillSnapshot, false);
        return;
    }

    HMD.openTablet();

    isDomainOpen(snapshotDomainID, function (canShare) {
        snapshotOptions = {
            containsGif: false,
            processingGif: false,
            canShare: canShare,
            isLoggedIn: isLoggedIn
        };
        imageData = [snapImageData(pathStillSnapshot)];
        ui.sendMessage({
            type: "snapshot",
            action: "addImages",
            options: snapshotOptions,
            image_data: imageData
        });
    });
}

function snapshotDirChanged(snapshotPath) {
    Window.browseDirChanged.disconnect(snapshotDirChanged);
    if (snapshotPath !== "") { // not cancelled
        Snapshot.setSnapshotsLocation(snapshotPath);
        ui.sendMessage({
            type: "snapshot",
            action: "snapshotLocationChosen"
        });
    }
}

function processingGifStarted(pathStillSnapshot) {
    Window.processingGifStarted.disconnect(processingGifStarted);
    Window.processingGifCompleted.connect(processingGifCompleted);
    isLoggedIn = Account.isLoggedIn();
    // show hud
    Reticle.visible = reticleVisible;
    Reticle.allowMouseCapture = true;
    // show overlays if they were on
    if (resetOverlays) {
        Menu.setIsOptionChecked("Show Overlays", true);
    }
    if (resetNametags) {
        Render.setNametagsEnabled(true);
    }
    Settings.setValue("previousStillSnapPath", storedLocation(pathStillSnapshot));

    if (captureTarget === "camera") {
        cameraTaken(pathStillSnapshot, true);
        return;
    }

    HMD.openTablet();
    
    isDomainOpen(snapshotDomainID, function (canShare) {
        snapshotOptions = {
            containsGif: true,
            processingGif: true,
            loadingGifPath: Script.resourcesPath() + 'icons/loadingDark.gif',
            canShare: canShare,
            isLoggedIn: isLoggedIn
        };
        imageData = [snapImageData(pathStillSnapshot)];
        ui.sendMessage({
            type: "snapshot",
            action: "addImages",
            options: snapshotOptions,
            image_data: imageData
        });
    });
}

function processingGifCompleted(pathAnimatedSnapshot) {
    isLoggedIn = Account.isLoggedIn();
    Window.processingGifCompleted.disconnect(processingGifCompleted);

    Settings.setValue("previousAnimatedSnapPath", storedLocation(pathAnimatedSnapshot));

    if (captureTarget === "camera") {
        cameraGifTaken(pathAnimatedSnapshot);
        return;
    }

    isDomainOpen(snapshotDomainID, function (canShare) {
        snapshotOptions = {
            containsGif: true,
            processingGif: false,
            canShare: canShare,
            isLoggedIn: isLoggedIn,
            canBlast: location.domainID === Settings.getValue("previousSnapshotDomainID"),
        };
        imageData = [snapImageData(pathAnimatedSnapshot)];
        ui.sendMessage({
            type: "snapshot",
            action: "addImages",
            options: snapshotOptions,
            image_data: imageData
        });
    });
}
function maybeDeleteSnapshotStories() {
    storyIDsToMaybeDelete.forEach(function (element, idx, array) {
        request({
            uri: METAVERSE_BASE + '/api/v1/user_stories/' + element,
            method: 'DELETE'
        }, function (error, response) {
            if (error || (response.status !== 'success')) {
                print("ERROR deleting snapshot story: ", error || response.status);
                return;
            } else {
                print("SUCCESS deleting snapshot story with ID", element);
            }
        })
    });
    storyIDsToMaybeDelete = [];
}
function onUsernameChanged() {
    fillImageDataFromPrevious();
    if (Snapshot.getSnapshotsLocation() === "" && !Account.isLoggedIn()) {
        // signed out with no folder chosen: a snap would have nowhere to go
        ui.sendMessage({
            type: "snapshot",
            action: "showSetupInstructions"
        });
    } else {
        isDomainOpen(Settings.getValue("previousSnapshotDomainID"), function (canShare) {
            ui.sendMessage({
                type: "snapshot",
                action: "showPreviousImages",
                options: snapshotOptions,
                image_data: imageData,
                canShare: canShare
            });
        });
    }
    if (isLoggedIn) {
        if (shareAfterLogin) {
            isDomainOpen(Settings.getValue("previousSnapshotDomainID"), function (canShare) {
                if (canShare) {
                    snapshotToShareAfterLogin.forEach(function (element) {
                        print('Uploading snapshot after login:', element.path);
                        Window.shareSnapshot(element.path, element.href);
                        var isGif = fileExtensionMatches(element.path, "gif");
                        if (isGif) {
                            mostRecentGifSnapshotFilename = getFilenameFromPath(element.path);
                        } else {
                            mostRecentStillSnapshotFilename = getFilenameFromPath(element.path);
                        }
                    });
                }
                isUploadingPrintableStill = canShare;
                updatePrintPermissions();
            });

            shareAfterLogin = false;
            snapshotToShareAfterLogin = [];
        }
    }
}

function snapshotLocationSet(location) {
    if (location !== "") {
        ui.sendMessage({
            type: "snapshot",
            action: "snapshotLocationChosen"
        });
    }
}

function updatePrintPermissions() {
    processRezPermissionChange(Entities.canRez() || Entities.canRezTmp());
}

var snapshotFailedToLoad = false;
var isUploadingPrintableStill = false;
function processRezPermissionChange(canRez) {
    var action = "";
    
    if (canRez && !snapshotFailedToLoad) {  
        if (Settings.getValue("previousStillSnapUrl")) {
            action = 'setPrintButtonEnabled';
        } else if (isUploadingPrintableStill) {
            action = 'setPrintButtonLoading';
        } else {
            action = 'setPrintButtonDisabled';
        }
    } else {
        action = 'setPrintButtonDisabled';
    }
    
    ui.sendMessage({
        type: "snapshot",
        action : action
    });
}

// --- the camera on desktop -----------------------------------------------------------------------------------------
// On desktop, Snap's button opens a camera over the world (html/SnapCamera.html): a click-through frame on the area a
// Snap keeps, a switch bar and a shutter bar, each an overlay window showing its part of the page. In VR the button
// opens the tablet page as before.

var CAMERA_PAGE = Script.resolvePath("html/SnapCamera.html");
// messages to Snap from the visitor's own scripts (the tests'): "open", "close", "shutter", "review"
var CAMERA_CHANNEL = "Snap.camera";
var CAMERA_ASPECT_RATIO = 1.91; // what a Snap keeps of the window: its centre at this width/height
var CAMERA_BAR = { width: 460, height: 56 };
var CAMERA_DOCK = { width: 340, height: 108 };
var CAMERA_EDGE = 12;
// With Show Overlays off the desktop fades its windows out over 300 ms (Desktop.qml): the Snap waits for that, and
// the camera's own windows leave the screen a moment before it starts, so the fade never has them.
var CAMERA_CAPTURE_DELAY = 450;
var CAMERA_HIDE_DELAY = 60;
var SYSTEM_TOOLBAR = "com.highfidelity.interface.toolbar.system";
var CAMERA_TOAST_MS = 2600;
var CAMERA_WATCHDOG_MS = 15000; // a Snap (with a GIF, its frames) not taken by then didn't save
var CAMERA_TOAST_LONG_MS = 6000;
var TIMER_CHOICES = [0, 3, 10];
var TIMER_SETTING = "snapTimerSeconds";
var GIF_SETTING = "alsoTakeAnimatedSnapshot";
var NAMETAGS_SETTING = "shapshotNametagsEnabled";
var KEY_ESCAPE = 16777216; // Qt::Key_Escape

var camera = {
    windows: null,
    open: false,
    busy: false,
    counting: false,
    countdown: 0,
    countdownTimer: null,
    watchdog: null,
    toast: null,
    toastTimer: null,
    modeBeforeSelfie: null,
    toolbarWasVisible: false,
    reviewOpen: false,
    selected: 0,
    snaps: [] // this session's Snaps from the camera, newest first: { stillPath, gifPath, time, deleting, pendingDeletes }
};

function cameraCrop() {
    var width = Overlays.width();
    var height = Overlays.height();
    if (width / height > CAMERA_ASPECT_RATIO) {
        var cropWidth = Math.round(height * CAMERA_ASPECT_RATIO);
        return { x: Math.round((width - cropWidth) / 2), y: 0, w: cropWidth, h: height };
    }
    var cropHeight = Math.round(width / CAMERA_ASPECT_RATIO);
    return { x: 0, y: Math.round((height - cropHeight) / 2), w: width, h: cropHeight };
}

function timerSeconds() {
    var seconds = Number(Settings.getValue(TIMER_SETTING, 0));
    return TIMER_CHOICES.indexOf(seconds) !== -1 ? seconds : 0;
}

function cameraRecord(path) {
    return path ? storedSnaps[path] : undefined;
}

function folderOf(path) {
    var slash = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
    return slash > 0 ? path.substring(0, slash) : path;
}

function timeOfDay(date) {
    function twoDigits(number) {
        return (number < 10 ? "0" : "") + number;
    }
    return twoDigits(date.getHours()) + ":" + twoDigits(date.getMinutes());
}

function cameraItem(snap) {
    var still = cameraRecord(snap.stillPath);
    var gif = cameraRecord(snap.gifPath);
    var inUserFiles = !!(still && still.inUserFiles);
    var where;
    if (inUserFiles) {
        where = "In My Files › Snapshots. Anyone with the link can see it; it doesn't say where it was taken.";
    } else if (!still) {
        where = "Saving to My Files…";
    } else {
        where = "On this computer, in " + folderOf(still.location) + "." +
            (still.uploadError ? " The upload to My Files failed: " + still.uploadError : "");
    }
    return {
        src: still ? still.location : snap.stillPath,
        gifSrc: snap.gifPath ? (gif ? gif.location : snap.gifPath) : null,
        inUserFiles: inUserFiles,
        canDelete: !snap.deleting && (inUserFiles || !!(gif && gif.inUserFiles)),
        when: "Today, " + timeOfDay(snap.time),
        where: where
    };
}

function cameraLast() {
    var snap = camera.snaps[0];
    if (!snap) {
        return null;
    }
    var still = cameraRecord(snap.stillPath);
    return {
        src: still ? still.location : snap.stillPath,
        badge: !still ? "saving" : (still.inUserFiles ? "in-files" : "local")
    };
}

function cameraState() {
    var timer = timerSeconds();
    return {
        crop: cameraCrop(),
        width: Overlays.width(),
        height: Overlays.height(),
        gif: !!Settings.getValue(GIF_SETTING, true),
        nametags: !!Settings.getValue(NAMETAGS_SETTING, true),
        timer: timer,
        selfie: camera.modeBeforeSelfie !== null,
        busy: camera.busy,
        counting: camera.counting,
        countdown: camera.countdown,
        progress: camera.counting && timer > 0 ? (timer - camera.countdown + 1) / timer : 0,
        progressColor: "violet",
        toast: camera.toast,
        last: cameraLast(),
        review: { open: camera.reviewOpen, selected: camera.selected, items: camera.snaps.map(cameraItem) }
    };
}

function cameraSend(message) {
    if (!camera.windows) {
        return;
    }
    var text = JSON.stringify(message);
    camera.windows.frame.emitScriptEvent(text);
    camera.windows.bar.emitScriptEvent(text);
    camera.windows.dock.emitScriptEvent(text);
}

function cameraUpdate() {
    cameraSend({ type: "snapCamera", action: "state", state: cameraState() });
}

function cameraWindow(part, width, height, enabled) {
    var overlay = new OverlayWebWindow({
        title: "Snap",
        source: CAMERA_PAGE + "?part=" + part,
        width: width,
        height: height,
        visible: false,
        frameless: true,
        enabled: enabled
    });
    overlay.webEventReceived.connect(onCameraEvent);
    if (!enabled) {
        // Window.qml enables a window each time it shows (enabled = visible), on the fade's first frame, after any
        // setEnabled sent with setVisible; enabled, the frame took the mouse, and a press on the world raised it over
        // the bars, where its band below the crop (on a window taller than a Snap, a 16:10 laptop) covered the
        // shutter bar and took its clicks. Click-through again once shown, unless the review (in the frame) is open.
        overlay.visibleChanged.connect(function () {
            if (!camera.reviewOpen) {
                overlay.setEnabled(false);
            }
        });
    }
    return overlay;
}

function cameraPlaceWindows() {
    var width = Overlays.width();
    var height = Overlays.height();
    var windows = camera.windows;
    windows.frame.setPosition(0, 0);
    windows.frame.setSize(width, height);
    windows.bar.setPosition(Math.round((width - CAMERA_BAR.width) / 2), CAMERA_EDGE);
    windows.bar.setSize(CAMERA_BAR.width, CAMERA_BAR.height);
    windows.dock.setPosition(Math.round((width - CAMERA_DOCK.width) / 2), height - CAMERA_DOCK.height - CAMERA_EDGE);
    windows.dock.setSize(CAMERA_DOCK.width, CAMERA_DOCK.height);
}

function cameraShowWindows(visible) {
    var windows = camera.windows;
    if (!windows) {
        return;
    }
    windows.frame.setVisible(visible);
    windows.bar.setVisible(visible && !camera.reviewOpen);
    windows.dock.setVisible(visible && !camera.reviewOpen);
}

function openCamera() {
    if (camera.open || HMD.active) {
        return;
    }
    if (!camera.windows) {
        camera.windows = {
            frame: cameraWindow("frame", Overlays.width(), Overlays.height(), false),
            bar: cameraWindow("bar", CAMERA_BAR.width, CAMERA_BAR.height, true),
            dock: cameraWindow("dock", CAMERA_DOCK.width, CAMERA_DOCK.height, true)
        };
    }
    camera.open = true;
    // a camera has the screen to itself: the toolbar steps aside until it closes ("shown", which a Snap's
    // Show Overlays off and on leaves alone; "visible" comes back with it, see Window.qml)
    var toolbar = Toolbars.getToolbar(SYSTEM_TOOLBAR);
    camera.toolbarWasVisible = !!(toolbar && toolbar.readProperty("shown"));
    if (camera.toolbarWasVisible) {
        toolbar.writeProperty("shown", false);
    }
    cameraPlaceWindows();
    cameraShowWindows(true);
    ui.buttonActive(true);
    cameraUpdate();
}

function closeCamera() {
    if (!camera.open) {
        return;
    }
    cameraStopCountdown();
    if (camera.toastTimer) {
        Script.clearTimeout(camera.toastTimer);
        camera.toastTimer = null;
    }
    camera.toast = null;
    if (camera.modeBeforeSelfie !== null) {
        var mode = camera.modeBeforeSelfie;
        camera.modeBeforeSelfie = null;
        Camera.mode = mode;
    }
    camera.reviewOpen = false;
    camera.windows.frame.setEnabled(false);
    camera.open = false;
    cameraShowWindows(false);
    if (camera.toolbarWasVisible) {
        var toolbar = Toolbars.getToolbar(SYSTEM_TOOLBAR);
        if (toolbar) {
            toolbar.writeProperty("shown", true);
        }
        camera.toolbarWasVisible = false;
    }
    ui.buttonActive(false);
}

function cameraToast(toast, hideAfterMs) {
    if (camera.toastTimer) {
        Script.clearTimeout(camera.toastTimer);
        camera.toastTimer = null;
    }
    camera.toast = toast;
    if (toast && hideAfterMs > 0) {
        camera.toastTimer = Script.setTimeout(function () {
            camera.toastTimer = null;
            camera.toast = null;
            cameraUpdate();
        }, hideAfterMs);
    }
}

function cameraToastForStored(stored, isGif, gifFollows) {
    if (stored.inUserFiles) {
        cameraToast({
            kind: "ok",
            text: "Saved to My Files",
            detail: gifFollows ? "Snapshots · the GIF follows" : (isGif ? "Snapshots · the GIF too" : "Snapshots")
        }, gifFollows ? 0 : CAMERA_TOAST_MS);
    } else if (stored.uploadError) {
        cameraToast({
            kind: "local",
            text: "Saved on this computer",
            detail: "the upload to My Files failed: " + stored.uploadError
        }, CAMERA_TOAST_LONG_MS);
    } else {
        cameraToast({ kind: "local", text: "Saved on this computer", detail: "sign in to keep Snaps in My Files" },
            CAMERA_TOAST_LONG_MS);
    }
}

function cameraStartCountdown(seconds) {
    camera.counting = true;
    camera.countdown = seconds;
    cameraUpdate();
    camera.countdownTimer = Script.setInterval(function () {
        camera.countdown -= 1;
        if (camera.countdown <= 0) {
            cameraStopCountdown();
            cameraCapture();
        } else {
            cameraUpdate();
        }
    }, 1000);
}

function cameraStopCountdown() {
    if (camera.countdownTimer) {
        Script.clearInterval(camera.countdownTimer);
        camera.countdownTimer = null;
    }
    camera.counting = false;
    camera.countdown = 0;
}

function cameraCapture() {
    if (camera.busy || !camera.open) {
        return;
    }
    camera.busy = true;
    cameraToast(null, 0);
    cameraUpdate();
    cameraShowWindows(false);
    cameraWatch();
    Script.setTimeout(function () {
        takeSnapshot("camera");
    }, CAMERA_HIDE_DELAY);
}

function cameraWatch() {
    cameraStopWatching();
    camera.watchdog = Script.setTimeout(function () {
        camera.watchdog = null;
        cameraCaptureFailed();
    }, CAMERA_WATCHDOG_MS);
}

function cameraStopWatching() {
    if (camera.watchdog) {
        Script.clearTimeout(camera.watchdog);
        camera.watchdog = null;
    }
}

// No word from the Snap (it couldn't be saved): everything it hid comes back, and the camera says so.
function cameraCaptureFailed() {
    [[Window.stillSnapshotTaken, stillSnapshotTaken], [Window.processingGifStarted, processingGifStarted],
        [Window.processingGifCompleted, processingGifCompleted]].forEach(function (pair) {
        try {
            pair[0].disconnect(pair[1]);
        } catch (notConnected) {
            // it wasn't waiting on this one
        }
    });
    Reticle.visible = reticleVisible;
    Reticle.allowMouseCapture = true;
    if (resetOverlays) {
        Menu.setIsOptionChecked("Show Overlays", true);
    }
    if (resetNametags) {
        Render.setNametagsEnabled(true);
    }
    camera.busy = false;
    cameraToast({ kind: "local", text: "That Snap didn't save", detail: "try again" }, CAMERA_TOAST_LONG_MS);
    cameraShowWindows(camera.open);
    cameraUpdate();
}

function cameraShutter() {
    if (camera.counting) {
        cameraStopCountdown();
        cameraUpdate();
        return;
    }
    if (camera.busy) {
        return;
    }
    var seconds = timerSeconds();
    if (seconds > 0) {
        cameraStartCountdown(seconds);
    } else {
        cameraCapture();
    }
}

// The still is taken (with a GIF, its frames too): the camera comes back, showing it.
function cameraTaken(stillPath, gifFollows) {
    camera.snaps.unshift({ stillPath: stillPath, gifPath: null, time: new Date(), deleting: false, pendingDeletes: 0 });
    camera.selected = 0;
    camera.busy = gifFollows;
    if (gifFollows) {
        cameraWatch();
    } else {
        cameraStopWatching();
    }
    var stored = cameraRecord(stillPath);
    if (stored) {
        cameraToastForStored(stored, false, gifFollows);
    } else {
        cameraToast({ kind: "saving", text: "Saving to My Files…", detail: gifFollows ? "the GIF follows" : "" }, 0);
    }
    if (camera.open) {
        cameraShowWindows(true);
        cameraSend({ type: "snapCamera", action: "flash" });
    }
    cameraUpdate();
}

function cameraGifTaken(gifPath) {
    cameraStopWatching();
    var snap = camera.snaps[0];
    if (snap && !snap.gifPath) {
        snap.gifPath = gifPath;
    }
    camera.busy = false;
    var stored = cameraRecord(gifPath);
    if (stored) {
        cameraToastForStored(stored, true, false);
    }
    cameraUpdate();
}

// Where a camera Snap went (Snapshot.snapshotStored): the note, the thumbnail and the review follow it.
function cameraStored(takenPath) {
    var newest = camera.snaps[0];
    if (!newest) {
        return;
    }
    if (newest.stillPath === takenPath) {
        cameraToastForStored(storedSnaps[takenPath], false, camera.busy);
    } else if (newest.gifPath === takenPath) {
        cameraToastForStored(storedSnaps[takenPath], true, false);
    }
    cameraUpdate();
}

function cameraOpenReview() {
    if (!camera.snaps.length) {
        return;
    }
    camera.reviewOpen = true;
    camera.selected = 0;
    camera.windows.frame.setEnabled(true);
    cameraShowWindows(true);
    cameraUpdate();
}

function cameraCloseReview() {
    camera.reviewOpen = false;
    if (camera.windows) {
        camera.windows.frame.setEnabled(false);
    }
    cameraShowWindows(camera.open);
    cameraUpdate();
}

function cameraSelected() {
    return camera.snaps[camera.selected] || null;
}

function cameraCopyLink() {
    var snap = cameraSelected();
    var still = snap && cameraRecord(snap.stillPath);
    if (still && still.inUserFiles) {
        Window.copyToClipboard(still.location);
    }
}

function cameraDelete() {
    var snap = cameraSelected();
    if (!snap || snap.deleting) {
        return;
    }
    var urls = [];
    [snap.stillPath, snap.gifPath].forEach(function (path) {
        var record = cameraRecord(path);
        if (record && record.inUserFiles) {
            urls.push(record.location);
        }
    });
    if (!urls.length) {
        return;
    }
    snap.deleting = true;
    snap.pendingDeletes = urls.length;
    urls.forEach(function (url) {
        Snapshot.deleteFromUserFiles(url);
    });
    cameraUpdate();
}

function onSnapshotDeleted(url, error) {
    var snap = null;
    camera.snaps.forEach(function (each) {
        [each.stillPath, each.gifPath].forEach(function (path) {
            var record = cameraRecord(path);
            if (record && record.location === url) {
                snap = each;
            }
        });
    });
    if (!snap) {
        return;
    }
    if (error) {
        snap.deleting = false;
        cameraToast({ kind: "local", text: "Not deleted", detail: error }, CAMERA_TOAST_LONG_MS);
        cameraUpdate();
        return;
    }
    snap.pendingDeletes -= 1;
    if (snap.pendingDeletes > 0) {
        return;
    }
    // gone from My Files: Snap forgets it too
    [snap.stillPath, snap.gifPath].forEach(function (path) {
        var record = cameraRecord(path);
        if (!record) {
            return;
        }
        if (Settings.getValue("previousStillSnapPath") === record.location) {
            Settings.setValue("previousStillSnapPath", "");
        }
        if (Settings.getValue("previousAnimatedSnapPath") === record.location) {
            Settings.setValue("previousAnimatedSnapPath", "");
        }
    });
    camera.snaps.splice(camera.snaps.indexOf(snap), 1);
    camera.selected = Math.min(camera.selected, Math.max(0, camera.snaps.length - 1));
    if (!camera.snaps.length) {
        cameraCloseReview();
    }
    cameraToast({ kind: "ok", text: "Deleted from My Files", detail: "" }, CAMERA_TOAST_MS);
    cameraUpdate();
}

function cameraFlip() {
    if (camera.modeBeforeSelfie === null) {
        camera.modeBeforeSelfie = Camera.mode;
        Camera.mode = "selfie";
    } else {
        var mode = camera.modeBeforeSelfie;
        camera.modeBeforeSelfie = null;
        Camera.mode = mode;
    }
    cameraUpdate();
}

function onCameraModeUpdated(mode) {
    // turned away from the selfie camera some other way: the flip goes back to off
    if (mode !== "selfie" && camera.modeBeforeSelfie !== null) {
        camera.modeBeforeSelfie = null;
        cameraUpdate();
    }
}

function cameraEscape() {
    if (camera.reviewOpen) {
        cameraCloseReview();
    } else if (camera.counting) {
        cameraStopCountdown();
        cameraUpdate();
    } else {
        closeCamera();
    }
}

function onCameraEvent(text) {
    var message;
    try {
        message = JSON.parse(text);
    } catch (error) {
        return;
    }
    if (!message || message.type !== "snapCamera") {
        return;
    }
    switch (message.action) {
        case "ready":
            cameraUpdate();
            break;
        case "close":
            closeCamera();
            break;
        case "escape":
            cameraEscape();
            break;
        case "toggleGif":
            Settings.setValue(GIF_SETTING, !Settings.getValue(GIF_SETTING, true));
            cameraUpdate();
            break;
        case "toggleNametags":
            Settings.setValue(NAMETAGS_SETTING, !Settings.getValue(NAMETAGS_SETTING, true));
            cameraUpdate();
            break;
        case "cycleTimer":
            Settings.setValue(TIMER_SETTING, TIMER_CHOICES[(TIMER_CHOICES.indexOf(timerSeconds()) + 1) % TIMER_CHOICES.length]);
            cameraUpdate();
            break;
        case "settings":
            openSnapSettings();
            break;
        case "shutter":
            cameraShutter();
            break;
        case "flip":
            cameraFlip();
            break;
        case "openReview":
            cameraOpenReview();
            break;
        case "closeReview":
            cameraCloseReview();
            break;
        case "select":
            camera.selected = Math.max(0, Math.min(camera.snaps.length - 1, Number(message.index) || 0));
            cameraUpdate();
            break;
        case "copyLink":
            cameraCopyLink();
            break;
        case "showInMyFiles":
            Window.openUrl(Account.metaverseServerURL + "/user/files");
            break;
        case "delete":
            cameraDelete();
            break;
        default:
            break;
    }
}

function onCameraKey(event) {
    if (camera.open && event.key === KEY_ESCAPE && !event.isAutoRepeat) {
        cameraEscape();
    }
}

// (fromOwnScripts: false for a world's scripts on this computer; a viewer without it gives none)
function onCameraMessage(channel, message, senderID, localOnly, fromOwnScripts) {
    if (channel !== CAMERA_CHANNEL || !localOnly || fromOwnScripts === false) {
        return;
    }
    if (message === "open") {
        openCamera();
    } else if (message === "close") {
        closeCamera();
    } else if (message === "shutter" && camera.open) {
        cameraShutter();
    } else if (message === "review" && camera.open) {
        cameraOpenReview();
    }
}

function onCameraGeometryChanged() {
    if (camera.windows) {
        cameraPlaceWindows();
        cameraUpdate();
    }
}

function onCameraDisplayModeChanged(isHMDMode) {
    if (isHMDMode) {
        closeCamera();
    }
}

function startup() {
    ui = new AppUi({
        buttonName: "Snap",
        sortOrder: 5,
        iconOverride: "icons/tablet-icons/snap.svg",
        iconBackgroundColor: "#607d8b", // blue gray 500
        home: Script.resolvePath("html/SnapshotReview.html"),
        onOpened: fillImageDataFromPrevious,
        onMessage: onMessage
    });

    Entities.canRezChanged.connect(updatePrintPermissions);
    Entities.canRezTmpChanged.connect(updatePrintPermissions);
    GlobalServices.myUsernameChanged.connect(onUsernameChanged);
    Snapshot.snapshotLocationSet.connect(snapshotLocationSet);
    Snapshot.snapshotStored.connect(onSnapshotStored);
    Window.snapshotShared.connect(snapshotUploaded);

    // on desktop the button opens the camera; in VR, the tablet page as before
    var openTabletPage = ui.open;
    ui.open = function (optionalUrl, optionalInject) {
        if (HMD.active || optionalUrl) {
            openTabletPage(optionalUrl, optionalInject);
        } else if (camera.open) {
            closeCamera();
        } else {
            openCamera();
        }
    };
    Snapshot.snapshotDeleted.connect(onSnapshotDeleted);
    Camera.modeUpdated.connect(onCameraModeUpdated);
    Controller.keyPressEvent.connect(onCameraKey);
    Window.geometryChanged.connect(onCameraGeometryChanged);
    HMD.displayModeChanged.connect(onCameraDisplayModeChanged);
    Messages.subscribe(CAMERA_CHANNEL);
    Messages.messageReceived.connect(onCameraMessage);
}
startup();

function shutdown() {
    closeCamera();
    if (camera.windows) {
        camera.windows.frame.close();
        camera.windows.bar.close();
        camera.windows.dock.close();
        camera.windows = null;
    }
    Messages.messageReceived.disconnect(onCameraMessage);
    Messages.unsubscribe(CAMERA_CHANNEL);
    HMD.displayModeChanged.disconnect(onCameraDisplayModeChanged);
    Window.geometryChanged.disconnect(onCameraGeometryChanged);
    Controller.keyPressEvent.disconnect(onCameraKey);
    Camera.modeUpdated.disconnect(onCameraModeUpdated);
    Snapshot.snapshotDeleted.disconnect(onSnapshotDeleted);
    Window.snapshotShared.disconnect(snapshotUploaded);
    Snapshot.snapshotStored.disconnect(onSnapshotStored);
    Snapshot.snapshotLocationSet.disconnect(snapshotLocationSet);
    GlobalServices.myUsernameChanged.disconnect(onUsernameChanged);
    Entities.canRezChanged.disconnect(updatePrintPermissions);
    Entities.canRezTmpChanged.disconnect(updatePrintPermissions);
}
Script.scriptEnding.connect(shutdown);

}()); // END LOCAL_SCOPE
