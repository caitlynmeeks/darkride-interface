import { SignalManager } from "../lib/signal-manager";
import { ButtonManager } from "./button-manager";

export class Buttons {
	readonly tablet = Tablet.getTablet(
		"com.highfidelity.interface.tablet.system",
	);

	signalManager = new SignalManager();
	buttonManager = new ButtonManager();

	readonly exploreMappingName = "com.tivolicloud.explore";
	// a local "open" here opens Explore: the viewer's "Can't reach" alert sends it
	readonly exploreChannel = "com.tivolicloud.explore";
	// the sign-in screen asks for every panel to close here (ui/SignInGate.cpp), with "close"
	readonly panelsChannel = "Darkride-Panels";

	constructor() {
		// old order from hifi
		// audio, enter vr, snap, avatar, people, goto, create, emote

		// toggle vr
		// const toggleVRButton = this.addUselessButton("enter vr", "", () => {
		// 	console.log("clicked");
		// });

		// settings: Darkride's own QML panel (qml/darkride/SettingsPanel.qml), the first of the web pages moved to QML
		const settings = this.buttonManager.addQmlButton(
			"Settings",
			'data:image/svg;xml,<svg xmlns="http://www.w3.org/2000/svg" fill="#fff" viewBox="0 0 24 24"><path d="M19.14,12.94c0.04-0.3,0.06-0.61,0.06-0.94c0-0.32-0.02-0.64-0.07-0.94l2.03-1.58c0.18-0.14,0.23-0.41,0.12-0.61 l-1.92-3.32c-0.12-0.22-0.37-0.29-0.59-0.22l-2.39,0.96c-0.5-0.38-1.03-0.7-1.62-0.94L14.4,2.81c-0.04-0.24-0.24-0.41-0.48-0.41 h-3.84c-0.24,0-0.43,0.17-0.47,0.41L9.25,5.35C8.66,5.59,8.12,5.92,7.63,6.29L5.24,5.33c-0.22-0.08-0.47,0-0.59,0.22L2.74,8.87 C2.62,9.08,2.66,9.34,2.86,9.48l2.03,1.58C4.84,11.36,4.8,11.69,4.8,12s0.02,0.64,0.07,0.94l-2.03,1.58 c-0.18,0.14-0.23,0.41-0.12,0.61l1.92,3.32c0.12,0.22,0.37,0.29,0.59,0.22l2.39-0.96c0.5,0.38,1.03,0.7,1.62,0.94l0.36,2.54 c0.05,0.24,0.24,0.41,0.48,0.41h3.84c0.24,0,0.44-0.17,0.47-0.41l0.36-2.54c0.59-0.24,1.13-0.56,1.62-0.94l2.39,0.96 c0.22,0.08,0.47,0,0.59-0.22l1.92-3.32c0.12-0.22,0.07-0.47-0.12-0.61L19.14,12.94z M12,15.6c-1.98,0-3.6-1.62-3.6-3.6 s1.62-3.6,3.6-3.6s3.6,1.62,3.6,3.6S13.98,15.6,12,15.6z"/></svg>',
			"#e91e63", // pink 500
			2,
			Script.resourcesPath() + "qml/darkride/SettingsPanel.qml",
			"small",
		);

		// avatar: Darkride's Avatar panel (qml/darkride/AvatarPanel.qml): the avatar, its own menu, the nametag
		const avatar = this.buttonManager.addQmlButton(
			"Avatar",
			'data:image/svg;xml,<svg fill="#fff" viewBox="0 0 48 48" xmlns="http://www.w3.org/2000/svg"><path d="M41 12c-5.22 1.4-11.34 2-17 2s-11.78-.6-17-2l-1 4c3.72 1 8 1.66 12 2v26h4V32h4v12h4V18c4-.34 8.28-1 12-2l-1-4zm-17 0c2.2 0 4-1.8 4-4s-1.8-4-4-4-4 1.8-4 4 1.8 4 4 4z"/></svg>',
			"#3f51b5", // indigo 500
			6,
			Script.resourcesPath() + "qml/darkride/AvatarPanel.qml",
			"small",
		);

		// people: Darkride's People panel (qml/darkride/PeoplePanel.qml): who is here, and your friends; it takes the
		// place of High Fidelity's People app (pal.js), in the same place on the bar
		const people = this.buttonManager.addQmlButton(
			"People",
			"icons/tablet-icons/people.svg",
			"#2196f3", // blue 500
			7,
			Script.resourcesPath() + "qml/darkride/PeoplePanel.qml",
			"small",
		);
		// a badge on People for the friend requests waiting for you, the same red one as Chat's unread messages
		// (social's Friends counts them from sign-in; F5, Caity 10-27 "friends as recommended")
		if (typeof Friends != "undefined") {
			const showRequests = () =>
				people.button.editProperties({ badge: Math.max(0, Friends.requestCount) });
			this.signalManager.connect(Friends.requestCountChanged, showRequests);
			showRequests();
		}

		// explore: Darkride's Explore panel (qml/darkride/ExplorePanel.qml): worlds, their instances, hops
		const explore = this.buttonManager.addQmlButton(
			"Explore",
			Script.resolvePath("../assets/explore.png"),
			null,
			8,
			Script.resourcesPath() + "qml/darkride/ExplorePanel.qml",
			"big",
		);
		// radio: Darkride's Radio panel (qml/darkride/RadioPanel.qml): internet radio in a classic skin player (Webamp),
		// SomaFM's stations, any stream address, the player's own skins
		this.buttonManager.addQmlButton(
			"Radio",
			"icons/bar/radio.svg",
			"#ff9800", // orange 500
			9,
			Script.resourcesPath() + "qml/darkride/RadioPanel.qml",
			"small",
		);

		// CTRL+L behaviour
		const mapping = Controller.newMapping(this.exploreMappingName);
		mapping
			.from(Controller.Hardware.Keyboard.L)
			.when(Controller.Hardware.Keyboard.Control)
			.to((state: number) => {
				if (state == 1) {
					if (explore.button.getProperties().isActive) {
						explore.close();
					} else {
						explore.open();
					}
				}
			});
		Messages.subscribe(this.exploreChannel);
		Messages.subscribe(this.panelsChannel);
		this.signalManager.connect(
			Messages.messageReceived,
			(channel: string, message: string, senderID: Uuid, localOnly: boolean) => {
				if (
					channel == this.exploreChannel &&
					localOnly &&
					message == "open" &&
					!explore.button.getProperties().isActive
				) {
					explore.open();
				}
				if (channel == this.panelsChannel && localOnly && message == "close") {
					this.buttonManager.closeAll();
				}
			},
		);

		// (Tivoli's "Market" web page had its button here, long switched off: its handler is gone too; its page stays in
		// frontend/src/app/market until the clean-up)

		// (Tivoli's "Things" web page had its button here: obsolete, gone (Caity 10-28); its page stays in
		// frontend/src/app/things until the clean-up)

		Controller.enableMapping(this.exploreMappingName);
	}

	cleanup = () => {
		Controller.disableMapping(this.exploreMappingName);
		Messages.unsubscribe(this.exploreChannel);
		Messages.unsubscribe(this.panelsChannel);

		this.buttonManager.cleanup();
		this.signalManager.cleanup();
	};
}
