import { SignalManager } from "./lib/signal-manager";

// Who each avatar in this world is: session id ("{...}") → the account behind it, shared by the nametags, the chat and
// the Tab overview. The world server says which account each session signed in with (it checked that account when the
// session connected; Users.requestUsernameFromID), and the account's public profile on the metaverse gives its picture
// and, for an AI resident, its mark, its own word and its host (AI-FIRST-CLIENT.md §10.4); for a bot (scripted logic,
// not AI), its host. So an avatar is matched to
// its account by the world's check, never by the display name it shows, and nothing here asks the metaverse who is
// where.

export interface CachedUser {
	username: string;
	ai: boolean;
	bot: boolean;
	aiWord: string;
	host: string;
	thumbnail: string;
	connection: string;
}

export type CachedUsers = { [sessionId: string]: CachedUser };

// the line under a resident's name: "✦ language model · hosted by caitlyn", or for a bot "⚙ bot · hosted by madders"
// (U+2699 alone: with U+FE0F it is drawn as a colour emoji)
export function markLine(user: { bot?: boolean; aiWord?: string; host?: string }) {
	if (user.bot) return "\u2699 bot" + (user.host ? " · hosted by " + user.host : "");
	return (
		"✦ " +
		(user.aiWord || "AI resident") +
		(user.host ? " · hosted by " + user.host : "")
	);
}

const NULL_UUID = "{00000000-0000-0000-0000-000000000000}";

interface Profile {
	username: string;
	ai: boolean;
	bot: boolean;
	aiWord: string;
	host: string;
	image: string;
	fetchedAt: number;
}

class UsersCache {
	private bySession: CachedUsers = {};
	// what the world said each session signed in with ("" for a visitor without an account)
	private names: { [sessionId: string]: string } = {};
	private profiles: { [username: string]: Profile } = {};
	private fetching: { [username: string]: boolean } = {};
	private listeners: ((users: CachedUsers) => any)[] = [];
	private signals = new SignalManager();

	private started = false;
	private interval: object = null;
	private askTimeout: object = null;
	private lastAskAt = 0;

	private readonly minAskIntervalMs = 2 * 1000;
	private readonly pollIntervalMs = 20 * 1000;
	private readonly profileMaxAgeMs = 5 * 60 * 1000;

	get(sessionId: string): CachedUser | null {
		if (sessionId == null) return null;
		this.start();
		return this.bySession[sessionId] || null;
	}

	all(): CachedUsers {
		this.start();
		return this.bySession;
	}

	onUpdate(listener: (users: CachedUsers) => any) {
		this.start();
		this.listeners.push(listener);
		return listener;
	}

	offUpdate(listener: (users: CachedUsers) => any) {
		this.listeners = this.listeners.filter(l => l !== listener);
	}

	// ask the world again soon, at most once every few seconds (arrivals and departures call this)
	refreshSoon() {
		this.start();
		const due = this.lastAskAt + this.minAskIntervalMs - Date.now();
		if (due <= 0) {
			this.ask();
		} else if (this.askTimeout == null) {
			this.askTimeout = Script.setTimeout(() => {
				this.askTimeout = null;
				this.ask();
			}, due);
		}
	}

	private start() {
		if (this.started) return;
		this.started = true;

		this.interval = Script.setInterval(() => this.ask(), this.pollIntervalMs);
		this.signals.connect(AvatarList.avatarAddedEvent, () => this.refreshSoon());
		this.signals.connect(AvatarList.avatarRemovedEvent, () => this.refreshSoon());
		this.signals.connect(AddressManager.hostChanged, () => {
			this.bySession = {};
			this.names = {};
			this.refreshSoon();
		});
		// the nametags ask too (for the admin mark); every answer is used
		this.signals.connect(Users.usernameFromIDReply, (sessionId: string, username: string) =>
			this.onName(sessionId, username),
		);

		this.ask();
	}

	// ask the world which account each avatar here signed in with, and forget the ones who left
	private ask() {
		this.lastAskAt = Date.now();

		const here: { [sessionId: string]: boolean } = {};
		const ids = AvatarList.getAvatarIdentifiers().concat([MyAvatar.sessionUUID]);
		for (const id of ids) {
			if (id == null || id == "" || id == NULL_UUID) continue;
			here[id] = true;
			Users.requestUsernameFromID(id);
		}

		let changed = false;
		for (const id of Object.keys(this.names)) {
			if (here[id]) continue;
			delete this.names[id];
			if (this.bySession[id]) {
				delete this.bySession[id];
				changed = true;
			}
		}
		if (changed) this.notify();
	}

	private onName(sessionId: string, username: string) {
		this.names[sessionId] = username || "";
		if (!username) {
			if (this.bySession[sessionId]) {
				delete this.bySession[sessionId];
				this.notify();
			}
			return;
		}

		const profile = this.profiles[username];
		if (profile != null) this.wear(sessionId, profile);
		if (profile == null || Date.now() - profile.fetchedAt > this.profileMaxAgeMs) {
			this.fetchProfile(username);
		}
	}

	private wear(sessionId: string, profile: Profile) {
		const old = this.bySession[sessionId];
		if (
			old != null &&
			old.username == profile.username &&
			old.ai == profile.ai &&
			old.bot == profile.bot &&
			old.aiWord == profile.aiWord &&
			old.host == profile.host &&
			old.thumbnail == profile.image
		)
			return;

		this.bySession[sessionId] = {
			username: profile.username,
			ai: profile.ai,
			bot: profile.bot,
			aiWord: profile.aiWord,
			host: profile.host,
			thumbnail: profile.image,
			connection: null,
		};
		this.notify();
	}

	// an account's public card on the metaverse: its picture, and for an AI resident its mark, word and host. `kind`
	// ("person", "ai" or "bot") comes first: a bot has `ai` false, and is never shown as AI
	private fetchProfile(username: string) {
		if (this.fetching[username]) return;
		this.fetching[username] = true;

		const req = new XMLHttpRequest();
		req.onreadystatechange = () => {
			if (req.readyState != 4) return;
			delete this.fetching[username];
			if (req.status != 200) return;

			let json: any;
			try {
				json = JSON.parse(req.responseText);
			} catch (err) {
				return;
			}
			if (json == null || typeof json.username != "string") return;
			// the profile is the account asked about, or it isn't used (a name the world reports can't borrow
			// another account's mark)
			if (json.username.toLowerCase() != username.toLowerCase()) return;

			const bot = json.kind === "bot";
			const ai = !bot && (json.kind === "ai" || json.ai === true);
			const profile: Profile = {
				username: json.username,
				ai,
				bot,
				aiWord: (ai && json.aiWord) || "",
				host: json.host || "",
				image: json.image || "",
				fetchedAt: Date.now(),
			};
			this.profiles[username] = profile;
			for (const sessionId of Object.keys(this.names)) {
				if (this.names[sessionId] == username) this.wear(sessionId, profile);
			}
		};
		req.open(
			"GET",
			AccountServices.metaverseServerURL +
				"/api/user/" +
				encodeURIComponent(username) +
				"/profile",
			true,
		);
		req.send();
	}

	private notify() {
		for (const listener of this.listeners) {
			try {
				listener(this.bySession);
			} catch (err) {
				console.error(err);
			}
		}
	}

	cleanup() {
		if (this.interval) Script.clearInterval(this.interval);
		if (this.askTimeout) Script.clearTimeout(this.askTimeout);
		this.interval = null;
		this.askTimeout = null;
		this.signals.cleanup();
		this.listeners = [];
		this.bySession = {};
		this.names = {};
		this.started = false;
	}
}

export const usersCache = new UsersCache();
