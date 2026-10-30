import { SignalManager } from "../lib/signal-manager";
import { WebEventHandler } from "./web-event-handler";

export interface PanelPair {
	window: OverlayWebWindow;
	entity: Uuid;
}

export interface ButtonData {
	button: TabletButtonProxy;
	panel: PanelPair;
	open: Function;
	close: Function;
}

export class ButtonManager {
	// for cleanup
	private buttons: ButtonData[] = [];
	private handlers: WebEventHandler[] = [];
	private panels: PanelPair[] = [];
	// the windows of the QML panels (addQmlButton), closed when another panel opens
	private qmlWindows: OverlayWindow[] = [];

	private readonly panelDistance = 0.75;
	private readonly panelRotation = 20;
	private readonly panelHeight = 0;
	private readonly panelScale = 0.7;
	private readonly panelDpi = 33;

	private readonly smallPanelModifier = 0.75;

	private readonly tablet = Tablet.getTablet(
		"com.highfidelity.interface.tablet.system",
	);

	private signalManager = new SignalManager();

	constructor() {}

	private getOptimalWindowSize(small = false) {
		const maxWidth = Window.innerWidth - 16;
		const maxHeight = Window.innerHeight - 96;
		const windowWidth = 1280 * (small ? this.smallPanelModifier : 1);
		const windowHeight = 720 * (small ? this.smallPanelModifier : 1);
		return {
			x: maxWidth < windowWidth ? maxWidth : windowWidth,
			y: maxHeight < windowHeight ? maxHeight : windowHeight,
		};
	}

	private getOptimalWindowPosition(small = false) {
		const size = this.getOptimalWindowSize(small);
		return {
			x: Window.innerWidth / 2 - size.x / 2,
			y: Window.innerHeight / 2 - size.y / 2,
		};
	}

	private inVr() {
		return HMD.active;
	}

	private lockControls(active: boolean) {
		// TODO: think of possible solutions for this
		// if (active) {
		// 	Controller.captureActionEvents(); // moving around
		// } else {
		// 	Controller.releaseActionEvents();
		// }
	}

	private tryCloseTablet(giveUpMs: number) {
		let connected = true;

		const closeTablet = () => {
			if (HMD.showTablet) {
				HMD.closeTablet();
				Script.update.disconnect(closeTablet);
				connected = false;
			}
		};
		Script.update.connect(closeTablet);

		Script.setTimeout(() => {
			if (connected) Script.update.disconnect(closeTablet);
		}, giveUpMs);
	}

	addWebButton(
		name: string,
		icon: string,
		backgroundColor: string,
		sortOrder: number,
		isPanel: "false" | "big" | "small" = "false",
		handlerClass: typeof WebEventHandler = null,
	) {
		const url =
			Script.resolvePath("../ui/index.html") +
			"#/" +
			name.toLowerCase() +
			"?metaverseUrl=" +
			AccountServices.metaverseServerURL;

		const buttonProperties = {
			text: name,
			icon,
			backgroundColor,
			iconPadding: 8,
			sortOrder,
			// activeIcon: icon.replace('fill="#fff"', 'fill="#000"'),
		};
		if (backgroundColor == null) {
			buttonProperties.backgroundColor = "transparent";
			buttonProperties.iconPadding = 0;
		}

		const button = this.tablet.addButton(buttonProperties);

		let visible = false;

		const panel =
			isPanel != "false"
				? {
						// for desktop
						window: new OverlayWebWindow({
							title: name,
							visible: false,
							source: "about:blank",
						}),
						// for vr
						entity: Entities.addEntity<Entities.EntityPropertiesWeb>(
							{
								type: "Web",
								sourceUrl: "about:blank",
								position: { x: 0, y: 0, z: 0 },
								dimensions: { x: 0, y: 0, z: 0 },
								maxFPS: 120,
								dpi: 0,
								visible: false,
								showKeyboardFocusHighlight: false,
								inputMode: "touch",
							},
							"local",
						),
				  }
				: null;

		// functions

		const verifyPanelUrl = () => {
			if (isPanel == "false") return;

			const windowUrl = panel.window.getURL();
			const entityUrl = Entities.getEntityProperties<Entities.EntityPropertiesWeb>(
				panel.entity,
			).sourceUrl;

			const inVr = this.inVr();
			const windowTargetUrl = inVr ? "about:blank" : url;
			const entityTargetUrl = inVr ? url : "about:blank";

			if (windowUrl != windowTargetUrl)
				panel.window.setURL(windowTargetUrl);

			if (entityUrl != entityTargetUrl)
				Entities.editEntity<Entities.EntityPropertiesWeb>(
					panel.entity,
					{ sourceUrl: entityTargetUrl },
				);
		};

		verifyPanelUrl();

		const open = () => {
			if (visible) return;

			visible = true;
			button.editProperties({
				isActive: true,
			});

			if (panel) {
				handler.emitEvent("refresh");

				if (this.inVr()) {
					const forwardQuat = Quat.cancelOutRollAndPitch(
						Camera.orientation,
						//MyAvatar.orientation,
					);
					const forwardYawRotation =
						(Quat.safeEulerAngles(forwardQuat).y / 180) * Math.PI;
					const forwardNormal = {
						x: Math.sin(forwardYawRotation),
						y: 0,
						z: Math.cos(forwardYawRotation),
					};

					const position = Vec3.sum(
						// move panel forward
						Vec3.sum(
							MyAvatar.position,
							Vec3.multiply(
								forwardNormal,
								-this.panelDistance * MyAvatar.scale,
							),
						),
						// move panel height
						{ x: 0, y: this.panelHeight * MyAvatar.scale, z: 0 },
					);

					const rotation = Quat.multiply(
						forwardQuat,
						Quat.fromPitchYawRollDegrees(-this.panelRotation, 0, 0),
					);

					const dimensions = Vec3.multiply(
						Vec3.multiply(
							{
								x: 1.6,
								y: 0.9,
								z: 0.01,
							},
							isPanel == "small" ? this.smallPanelModifier : 1,
						),
						this.panelScale * MyAvatar.scale,
					);

					const dpi = this.panelDpi * (1 / MyAvatar.scale);

					Entities.editEntity<Entities.EntityPropertiesWeb>(
						panel.entity,
						{
							position,
							rotation,
							dimensions,
							dpi,
							visible: true,
						},
					);

					this.lockControls(true);
					HMD.closeTablet();
				} else {
					// close all other window panels first
					for (const panel of this.panels) {
						panel.window.setVisible(false);
					}
					for (const qmlWindow of this.qmlWindows) {
						qmlWindow.setVisible(false);
					}

					panel.window.setSize(
						this.getOptimalWindowSize(isPanel == "small") as Vec2,
					);
					panel.window.setPosition(
						this.getOptimalWindowPosition(
							isPanel == "small",
						) as Vec2,
					);
					panel.window.setVisible(true);
				}

				// disabled because it resets the url everytime you open
				// verifyPanelUrl();
			} else {
				this.tablet.gotoWebScreen(url);
			}
		};

		const close = (force = false) => {
			if (!visible) return;

			visible = false;
			button.editProperties({
				isActive: false,
			});

			if (panel) {
				const inVr = this.inVr();

				if (inVr || force) {
					Entities.editEntity<Entities.EntityPropertiesWeb>(
						panel.entity,
						{
							visible: false,
						},
					);

					this.lockControls(false);
				}

				if (!inVr || force) {
					panel.window.setVisible(false);
				}
			} else {
				this.tablet.gotoHomeScreen();
				HMD.closeTablet();
			}
		};

		const setVisible = (visible: boolean) => {
			if (visible) {
				open();
			} else {
				close();
			}
		};

		// handler

		const buttonData: ButtonData = {
			button,
			panel,
			open,
			close,
		};

		const handler =
			handlerClass == null
				? new WebEventHandler(
						"com.tivolicloud.defaultScripts",
						buttonData,
						this.buttons,
				  )
				: new handlerClass(
						"com.tivolicloud.defaultScripts." + name.toLowerCase(),
						buttonData,
						this.buttons,
				  );

		this.handlers.push(handler); // for cleanup

		// signals

		this.signalManager.connect(button.clicked, () => {
			setVisible(!visible);
		});

		this.signalManager.connect(HMD.displayModeChanged, () => {
			verifyPanelUrl();
		});

		if (panel) {
			// when window changes visibility
			this.signalManager.connect(panel.window.visibleChanged, () => {
				setVisible(panel.window.isVisible());
			});

			// when window gets web event
			this.signalManager.connect(
				panel.window.webEventReceived,
				handler.webEventReceived,
			);

			// when vr menu button is pressed whilst open
			this.signalManager.connect(
				Controller.inputEvent,
				(action: number, value: number) => {
					if (action != Controller.Standard.LeftSecondaryThumb)
						return;

					if (button.getProperties().isActive) {
						close();

						// TODO: not the best solution
						this.tryCloseTablet(200);
					}
				},
			);

			// when panel get web event
			this.signalManager.connect(
				Entities.webEventReceived,
				(entityID: Uuid, message: string) => {
					if (entityID != panel.entity) return;
					handler.webEventReceived(message);
				},
			);

			// when user switches between vr and desktop
			this.signalManager.connect(HMD.displayModeChanged, () => {
				close(true);
			});

			// will be used for emitting and cleanup
			this.panels.push(panel);
		} else {
			this.signalManager.connect(
				this.tablet.screenChanged,
				(type: string, newCurrentUrl: string) => {
					setVisible(newCurrentUrl == url);
				},
			);

			// the tablet's events come from whatever page it shows: only this button's own page is heard
			this.signalManager.connect(
				this.tablet.webEventReceived,
				(message: string) => {
					if (visible) handler.webEventReceived(message);
				},
			);
		}

		this.buttons.push(buttonData);
		return buttonData;
	}

	// A button whose panel is Darkride's own QML (ARCHITECTURE.md §5.2.1), not a web page: on the desktop the panel's
	// dock (Panels, interface/src/ui/Panels.h), which floats, docks beside the 3D view or tabs with another panel, or a
	// window placed and sized as the web panels are for a panel without one; in a headset the tablet's screen. The page
	// reads and sets the engine's values itself through its context properties, so there is no event handler between
	// them.
	addQmlButton(
		name: string,
		icon: string,
		backgroundColor: string,
		sortOrder: number,
		qmlUrl: string,
		isPanel: "big" | "small" = "small",
	) {
		const button = this.tablet.addButton({
			text: name,
			icon,
			backgroundColor: backgroundColor == null ? "transparent" : backgroundColor,
			iconPadding: backgroundColor == null ? 0 : 8,
			sortOrder,
		});
		// a dock is the engine's: it stays open beside other panels, and the button follows it however it opens or
		// closes (its close box, View > Panels)
		const docked =
			typeof Panels != "undefined" && Panels.names().indexOf(name) >= 0;
		// the desktop window is made on the first open, so the page does no work until it is wanted
		let window: OverlayWindow = null;
		const makeWindow = () => {
			window = new OverlayWindow({
				title: "", // the page carries its own large title; the frame keeps only its close box
				source: qmlUrl,
				width: 560,
				height: 640,
				visible: false,
			});
			this.qmlWindows.push(window);
			// the window's own close box: a hide counts once the window has been seen open since open() (a new
			// window's first signals say "hidden" while it is still coming up; taken as a close, they hid it mid
			// fade-in, which left it on screen marked hidden, and its close box did nothing)
			this.signalManager.connect(window.visibleChanged, () => {
				if (window.isVisible()) {
					seenOpen = true;
				} else if (seenOpen && visible && !this.inVr()) {
					close();
				}
			});
		};

		let visible = false;
		let seenOpen = false;

		const open = () => {
			if (visible) return;
			visible = true;
			seenOpen = false;
			button.editProperties({ isActive: true });

			if (this.inVr()) {
				this.tablet.loadQMLSource(qmlUrl);
				HMD.openTablet();
			} else if (docked) {
				Panels.open(name);
			} else {
				for (const panel of this.panels) {
					panel.window.setVisible(false);
				}
				for (const qmlWindow of this.qmlWindows) {
					if (qmlWindow != window) qmlWindow.setVisible(false);
				}
				if (window == null) makeWindow();
				// as tall as a big panel; a column wide (a settings page), or wider for a big one (Explore's cards)
				const size = this.getOptimalWindowSize(isPanel == "small");
				const width = Math.min(size.x, isPanel == "big" ? 760 : 560);
				window.setSize(width, this.getOptimalWindowSize(false).y);
				window.setPosition(
					Window.innerWidth / 2 - width / 2,
					Window.innerHeight / 2 - this.getOptimalWindowSize(false).y / 2,
				);
				window.setVisible(true);
				window.raise();
			}
		};

		const close = (force = false) => {
			if (!visible) return;
			visible = false;
			seenOpen = false;
			button.editProperties({ isActive: false });

			const inVr = this.inVr();
			if ((inVr || force) && this.tablet.isPathLoaded(qmlUrl)) {
				this.tablet.gotoHomeScreen();
			}
			if ((!inVr || force) && docked) {
				Panels.close(name);
			} else if ((!inVr || force) && window != null) {
				window.setVisible(false);
			}
		};

		const buttonData: ButtonData = { button, panel: null, open, close };

		if (docked) {
			// open from the saved layout before this script started, or opened and closed by the player
			const follow = (open: boolean) => {
				if (this.inVr() || open == visible) return;
				visible = open;
				button.editProperties({ isActive: open });
			};
			follow(Panels.isOpen(name));
			this.signalManager.connect(
				Panels.openChanged,
				(panelName: string, open: boolean) => {
					if (panelName == name) follow(open);
				},
			);
		}

		this.signalManager.connect(button.clicked, () => {
			// a dock open behind another panel's tab comes in front; in front, it closes
			if (docked && !this.inVr()) Panels.toggle(name);
			else if (visible) close();
			else open();
		});
		// the tablet showing something else
		this.signalManager.connect(
			this.tablet.screenChanged,
			(type: string, newCurrentUrl: string) => {
				if (this.inVr() && visible && newCurrentUrl != qmlUrl) close();
			},
		);
		// switching between the desktop and a headset: a dock stays on the desktop as the player left it
		this.signalManager.connect(HMD.displayModeChanged, () => {
			if (!docked) {
				close(true);
				return;
			}
			if (this.tablet.isPathLoaded(qmlUrl)) this.tablet.gotoHomeScreen();
			visible = !this.inVr() && Panels.isOpen(name);
			seenOpen = false;
			button.editProperties({ isActive: visible });
		});

		this.buttons.push(buttonData);
		return buttonData;
	}

	addUselessButton(name: string, icon: string, onButtonClicked = () => {}) {
		const button = this.tablet.addButton({
			text: name,
			icon,
			activeIcon: icon.replace('fill="#fff"', 'fill="#000"'),
		});

		this.signalManager.connect(button.clicked, onButtonClicked);

		this.buttons.push({
			button,
			panel: null,
			open: () => {},
			close: () => {},
		});
		return button;
	}

	// every panel closed: the sign-in screen is coming up (ui/SignInGate.cpp) and must not sit under one
	closeAll() {
		for (const button of this.buttons) {
			button.close();
		}
	}

	cleanup() {
		for (let button of this.buttons) {
			this.tablet.removeButton(button.button);
		}

		for (let panel of this.panels) {
			panel.window.close();
			Entities.deleteEntity(panel.entity);
		}

		for (let qmlWindow of this.qmlWindows) {
			qmlWindow.close();
		}

		for (let handler of this.handlers) {
			handler.cleanup();
		}

		this.signalManager.cleanup();
	}
}
