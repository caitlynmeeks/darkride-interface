// included scripts
[
	"system/request-service.js", // some http request dependancy thing
	"system/worldLoading.js", // the loading screen: a hop to another world, and a world that loads whole first
	// "system/progress.js", // progress bar at the bottom, to be rewritten
	// "system/away.js",
	"system/hmd.js", // enter leave vr button
	"system/menu.js", // settings menu in vr
	// "system/bubble.js",

	"system/snapshot.js",
	// "system/makeUserConnection.js",
	// "system/tablet-goto.js",
	// "system/notifications.js",
	"system/emote.js",
	"system/vehicles.js", // sit in and drive vehicles (PHYSICS.md round 6)
	"system/avatarMenu.js", // an imported avatar's menu
	// "system/audioMuteOverlay.js", // the big text that appears when you're muted
	"system/keyboardShortcuts/keyboardShortcuts.js"
	// "system/interstitialPage.js"
].forEach(function (script) {
	Script.include("file:///~/" + script);
});

// loaded scripts
[
	"tivoli/tivoli.js",
	"system/inspect.js",
	"system/controllers/controllerScripts.js",
	"system/buildMode.js", // the bar's Create button: the native editor's build mode
	"system/impostorLodView.js" // Developer > Render > Show Impostors and LODs
].forEach(function (script) {
	var runningScripts = ScriptDiscoveryService.getRunning();

	for (var i = 0; i < runningScripts.length; i++) {
		if (runningScripts[i].name == script.split("/").pop()) return;
	}

	ScriptDiscoveryService.loadScript("file:///~/" + script);
});
