import { usersCache } from "./users-cache";

export function initNametags(showSelf) {
	var nametags = {};
	console.log("nametags: starting, showSelf " + showSelf);

	// An AI resident's mark turns while it thinks (2026-10-10, Caitlyn: "the claude text animation ... back to star
	// diamond when she's done"): its body says busy on AI_CHANNEL, the world stamps the sender, and the picture's
	// glyph cycles through the frames the metaverse draws (glyph=1..5), then back to the mark (0).
	var AI_CHANNEL = "tivoli.ai-resident";
	var GLYPH_CYCLE = [1, 2, 3, 4, 5, 4, 3, 2];
	var GLYPH_MS = 130;
	var BUSY_TIMEOUT_MS = 15000;

	// the picture is drawn by the metaverse we are on; for an AI resident it carries the mark,
	// the resident's word and its host (AI-FIRST-CLIENT.md §10.2)
	function nametagURL(nametag, glyph) {
		return AccountServices.metaverseServerURL+"/api/user/"+
			encodeURIComponent(nametag.username)+
			"/nametag?"+ encodeURIComponent(glyph > 0 ? nametag.stamp + "." + (nametag.frameTry || 0) : nametag.stamp) +
			"&glyph="+ encodeURIComponent(glyph) +
			(nametag.admin?"&admin":"")+
			(nametag.connection=="friend"?"&friend":"");
	}

	function isAIResident(nametag) {
		var user = usersCache.get(nametag.id);
		return !!(user && user.ai);
	}

	function prefetchGlyphs(nametag) {
		if (nametag.prefetched == nametag.stamp) return;
		nametag.prefetched = nametag.stamp;
		try {
			nametag.glyphResources = [];
			for (var g = 1; g <= 5; g++) nametag.glyphResources.push(TextureCache.prefetch(nametagURL(nametag, g)));
		} catch (e) {
			console.error("nametags: prefetch failed: " + e);
		}
	}

	// the frames turn only once all have arrived (a resource's state: 2 loaded, 3 finished), never showing a blank
	function glyphsReady(nametag) {
		var r = nametag.glyphResources;
		if (!r || r.length < 5) return false;
		for (var i = 0; i < r.length; i++) {
			if (!r[i] || (r[i].state !== 2 && r[i].state !== 3)) return false;
		}
		return true;
	}

	function updateNametagEntity(nametag) {
		if (nametag.entity==undefined) return;

		// no name (not signed in, signing in or out, or a newcomer's details not here yet): there is no picture to
		// ask for, so the tag stays hidden; a name arriving is a change, and this runs again then
		if (!nametag.username) {
			Entities.editEntity(nametag.entity, { alpha: 0, imageURL: "" });
			return;
		}

		nametag.stamp = Date.now(); // a new picture when the details change; its glyph frames share the stamp
		Entities.editEntity(nametag.entity, {
			alpha: 1,
			imageURL: nametagURL(nametag, 0)
		});
		try {
			if (isAIResident(nametag)) prefetchGlyphs(nametag);
		} catch (e) {
			console.error("nametags: AI resident check failed: " + e);
		}
	}

	function addNametag(id, details) {
		/* details = {
			username: String,
			avatarURL: String,
			connection: String,
			admin: Boolean,
		} */

		if (nametags[id]!=undefined) return;
		var nametag = details;
		nametag.id = id;

		var scale = 1.5;
		nametag.entity = Entities.addEntity({
			name: "com.tivolicloud.nametags",
			parentID: id,
			localPosition: {y:1},
			dimensions: {x:scale, y:scale/8},
			grab: {
				grabbable: false,
				grabFollowsController: false,
				equippable: false,
			},
			type: "Image",
			emissive: true,
			keepAspectRatio: false,
			billboardMode: "full",
			imageURL: "",
			alpha: 0,
			isVisibleInSecondaryCamera: false,
			collisionless: true,
			collisionMask: 0,
			ignorePickIntersection: true
		}, "local");

		nametags[id] = nametag;
		updateNametagEntity(nametag);
		console.log("nametags: added " + nametag.username + " " + nametag.entity);
	}

	function updateNametag(nametag, details) {
		var updated = false;

		function update(key) {
			if (details[key]==undefined) return;
			if (nametag[key]!=details[key]) {
				nametag[key] = details[key];
				updated = true;
			}
		}

		update("username");
		update("avatarURL");
		update("connection");
		update("admin");

		if (updated) updateNametagEntity(nametag);
	}

	function deleteNametag(id) {
		Entities.deleteEntity(nametags[id].entity);
		delete nametags[id];
	}

	// the account the world checked when this avatar connected (usersCache), else none: the tag stays hidden. Never the
	// name it shows, which anyone can set
	function nameOf(id) {
		var known = usersCache.get(id);
		return known ? known.username : "";
	}

	function updateUserLocal(id) {
		var localUser = AvatarList.getAvatar(id);
		var nametag = nametags[id];

		if (localUser.sessionUUID == null) {
			if (nametag==undefined) return; // doesnt exist
			// must have left
			deleteNametag(id);
			return;
		}

		if (nametag==undefined) {
			// add nametag
			addNametag(id, {
				username: nameOf(id),
				avatarURL: AccountServices.metaverseServerURL+"/api/user/-/image",
				connection: null,
				admin: false,
			});
		} else {
			// update nametag
			updateNametag(nametag, {
				username: nameOf(id)
			});
		}

		// check if admin
		Users.requestUsernameFromID(id);
	}

	function updateUsersLocal() {
		//console.log("Updating users locally");

		AvatarList.getAvatarIdentifiers().forEach(function(id) {
			if (id==null) return;
			updateUserLocal(id);
		});

		if (showSelf) {
			updateUserLocal(MyAvatar.sessionUUID);
		}
	}

	function applyUsersAPI(users) {
		Object.keys(users).forEach(function(id) {
			var nametag = nametags[id];
			if (nametag==undefined) return;
			var apiUser = users[id];

			updateNametag(nametag, {
				username: apiUser.username,
				avatarURL: apiUser.thumbnail,
				connection: apiUser.connection,
			});
		});
	}

	function updateUsersAPI() {
		//console.log("Updating users with API");
		usersCache.refreshSoon();
	}

	var usersListener = usersCache.onUpdate(applyUsersAPI);

	var gracefullyUpdateReady = true;
	var gracefullyUpdateToBeUpdated = false;

	function gracefullyUpdateUsersAPI() {
		if (gracefullyUpdateReady==false) {
			gracefullyUpdateToBeUpdated = true;
		} else {
			updateUsersAPI();
			gracefullyUpdateReady = false;

			Script.setTimeout(function() {
				gracefullyUpdateReady = true;

				if (gracefullyUpdateToBeUpdated) {
					updateUsersAPI();
					gracefullyUpdateToBeUpdated = false;
				}
			}, 1000*5);
		}
	}

	updateUsersLocal();
	gracefullyUpdateUsersAPI();

	function avatarAddedEvent(id) {
		Script.setTimeout(function() {
			updateUserLocal(id);
			gracefullyUpdateUsersAPI();
		}, 1000);
	}

	function avatarRemovedEvent(id) {
		updateUserLocal(id);
		gracefullyUpdateUsersAPI();
	}

	function hostChanged() {
		Object.keys(nametags).forEach(function(id) {
			deleteNametag(id);
		});
	}

	function usernameFromIDReply(id, username, machineFingerprint, isAdmin) {
		var nametag = nametags[id];
		if (nametag==undefined) return;
		updateNametag(nametag, {
			admin: isAdmin
		});
	}

	// function avatarSessionChangedEvent(id, oldId) {
	// 	updateUsersLocal();
	// 	gracefullyUpdateUsersAPI();
	// }

	var updateIntervalLocal = Script.setInterval(function() {
		updateUsersLocal();
	}, 1000*10);

	var updateIntervalAPI = Script.setInterval(function() {
		gracefullyUpdateUsersAPI();
	}, 1000*20);

	AvatarList.avatarAddedEvent.connect(avatarAddedEvent);
	AvatarList.avatarRemovedEvent.connect(avatarRemovedEvent);
	// AvatarList.avatarSessionChangedEvent.connect(avatarSessionChangedEvent);
	location.hostChanged.connect(hostChanged);
	Users.usernameFromIDReply.connect(usernameFromIDReply);

	function onAIResidentMessage(channel, message, senderID) {
		if (channel != AI_CHANNEL) return;
		var nametag = nametags[senderID];
		if (nametag==undefined || nametag.entity==undefined || !nametag.username || !isAIResident(nametag)) return;
		var data;
		try { data = JSON.parse(message); } catch (e) { return; }
		if (data && data.busy) {
			if (!(nametag.busyUntil > Date.now())) prefetchGlyphs(nametag);
			nametag.busyUntil = Date.now() + BUSY_TIMEOUT_MS;
		} else if (nametag.busyUntil) {
			nametag.busyUntil = 0;
			Entities.editEntity(nametag.entity, { imageURL: nametagURL(nametag, 0) });
		}
	}
	Messages.subscribe(AI_CHANNEL);
	Messages.messageReceived.connect(onAIResidentMessage);

	var glyphInterval = Script.setInterval(function() {
		var now = Date.now();
		Object.keys(nametags).forEach(function(id) {
			var nametag = nametags[id];
			if (!nametag.busyUntil || nametag.entity==undefined || !nametag.username) return;
			if (now > nametag.busyUntil) {                // no word for a while: back to the mark
				nametag.busyUntil = 0;
				Entities.editEntity(nametag.entity, { imageURL: nametagURL(nametag, 0) });
				return;
			}
			if (!glyphsReady(nametag)) {                 // the mark stays until every frame is here;
				var failed = (nametag.glyphResources || []).some(function(r) { return r && r.state === 4; });
				if (failed && nametag.retriedStamp != nametag.stamp) {
					nametag.retriedStamp = nametag.stamp;    // frames that failed (a jammed server) are asked for
					nametag.frameTry = (nametag.frameTry || 0) + 1;   // once more under new addresses; the
					nametag.prefetched = null;               // picture showing keeps its own, so it never blanks
					prefetchGlyphs(nametag);
				}
				return;
			}
			nametag.glyphStep = ((nametag.glyphStep || 0) + 1) % GLYPH_CYCLE.length;
			Entities.editEntity(nametag.entity, { imageURL: nametagURL(nametag, GLYPH_CYCLE[nametag.glyphStep]) });
		});
	}, GLYPH_MS);

	var cleanup = function() {
		Script.clearInterval(updateIntervalLocal);
		Script.clearInterval(updateIntervalAPI);
		Script.clearInterval(glyphInterval);
		Messages.messageReceived.disconnect(onAIResidentMessage);
		Messages.unsubscribe(AI_CHANNEL);

		AvatarList.avatarAddedEvent.disconnect(avatarAddedEvent);
		AvatarList.avatarRemovedEvent.disconnect(avatarRemovedEvent);
		// AvatarList.avatarSessionChangedEvent.disconnect(avatarSessionChangedEvent);
		location.hostChanged.disconnect(hostChanged);
		Users.usernameFromIDReply.disconnect(usernameFromIDReply);
		usersCache.offUpdate(usersListener);

		Object.keys(nametags).forEach(function(id) {
			Entities.deleteEntity(nametags[id].entity);
		});
	}

	return { cleanup };
}