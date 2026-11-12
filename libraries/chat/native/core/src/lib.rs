//! The viewer's chat (CHAT.md 4.5): Matrix through matrix-rust-sdk, the library behind Element X, wrapped in a small
//! interface the viewer's Qt panels can call. The host sends commands as JSON and gets events back as JSON, through a
//! callback on one of this library's threads (Qt posts them to its own thread). The C interface is ../ffi; the
//! spike (src/bin/spike.rs) drives the same commands from stdin.
//!
//! Commands: {"op": ...}
//!   login_jwt {token}            sign in with the metaverse's chat token (POST /api/v1/user/chat-token); once
//!                                signed in (and at each start after), General (#general, else #lobby) and Darkride's
//!                                space (#darkride) are joined when you aren't in them, as the web chat does
//!   open {room, count?}          follow a room's timeline (a room id or #alias), at least count (30) of its
//!                                newest events; "timeline" events follow
//!   close {room}
//!   back {room, count}           older messages; the room's timeline events carry count more of its items from now on
//!   send {room, body}
//!   react {room, id, key}        a reaction (an emoji) on a message, or yours taken back when it is there already
//!   edit {room, id, body}        a new text for one of your own messages
//!   delete {room, id}            one of your own messages, gone for everyone
//!   read {room}                  mark it read
//!   join {room}                  accept an invite, or join by id or alias
//!   leave {room}
//!   create_dm {user, encrypted?} a DM with someone, inviting them (encrypted when asked)
//!   media {key, source, width?, height?, crop?}
//!                                a picture or file into the data folder's media/ (decrypted when it is encrypted):
//!                                source as a timeline item's media has it ({"url": mxc} or {"file": {...}}), or an
//!                                avatar's mxc address as {"url": ...}; width and height ask the server for a
//!                                thumbnail (crop: cut to fill it, for avatars), else the whole file
//!   logout
//! Events: {"event": ...}
//!   state {state: signed_out|starting|ready, user?, reason?}
//!   sync {state: running|offline|error|idle|terminated, message?}   offline: waiting for the server, by itself
//!   rooms {rooms: [{id, name, dm, dm_user?, encrypted, invited, unread, notifications, members, avatar?, space?}],
//!          spaces: [{id, name, avatar?}]}
//!                                space: the joined space that lists the room among its children; a DM is named
//!                                after the other person, dm_user; avatar: an mxc address
//!   profile {name?, avatar?}     who you are, once signed in
//!   timeline {room, items: [{id, sender, name, avatar?, body, kind, ts, own, encrypted, state, media?, change?,
//!          user?, user_name?, reactions, edited, editable}]}
//!                                the newest items: at first 100 (or the count open asked for), more after each back;
//!                                id: the event's id ($...), or txn:<transaction id> for your own while it is sent (it
//!                                stays good after); reactions: [{key, count, mine, senders}], the same emoji with and
//!                                without the emoji-style mark (U+FE0F) as one; edited: its text was changed;
//!                                editable: yours, and a text that edit can change;
//!                                kind: text, emote, notice, image, file, video, audio, undecryptable, other, member;
//!                                state: sending, waiting (for the server), sent, failed; media (image, file, video,
//!                                audio): {source, thumbnail_source?, mimetype?, width?, height?, size?}; member:
//!                                change (joined, left, invited, kicked, banned, ...) of user, named user_name
//!   media {key, path?, error?}   a media command finished: the file, or why not
//!   done {op, room?, start?}     a command finished (open, back, create_dm, join, leave); start: the room's
//!                                first message is in, there's nothing older
//!   error {op, message}          op join_general: joining General or the space at the start didn't work

// the SDK's futures are deep; matrix-rust-sdk's own crates raise this too
#![recursion_limit = "256"]

use std::{
    collections::HashMap,
    hash::{DefaultHasher, Hash, Hasher},
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};

use anyhow::{Context, Result, anyhow};
use futures_util::StreamExt;
use matrix_sdk::{
    Client, Room, RoomState, SessionChange,
    authentication::matrix::MatrixSession,
    deserialized_responses::RawSyncOrStrippedState,
    room::edit::EditedContent,
    media::{MediaFormat, MediaRequestParameters, MediaThumbnailSettings},
    ruma::{
        EventId, OwnedRoomId, RoomAliasId, RoomId, RoomOrAliasId, TransactionId, UInt, UserId,
        api::client::{
            media::get_content_thumbnail::v3::Method, membership::joined_rooms, receipt::create_receipt::v3::ReceiptType,
            room::create_room,
        },
        events::{
            InitialStateEvent,
            room::{
                MediaSource,
                encryption::RoomEncryptionEventContent,
                message::{MessageType, RoomMessageEventContent, RoomMessageEventContentWithoutRelation},
            },
            space::child::SpaceChildEventContent,
        },
    },
};
use matrix_sdk_ui::{
    eyeball_im::Vector,
    room_list_service::{RoomListItem, filters::new_filter_non_left},
    sync_service::{State as SyncState, SyncService},
    timeline::{
        EventSendState, EventTimelineItem, MembershipChange, RoomExt, Timeline, TimelineDetails, TimelineEventItemId,
        TimelineItem, TimelineItemContent,
    },
};
use serde::Deserialize;
use serde_json::{Value, json};
use tokio::{runtime::Runtime, sync::mpsc, task::JoinHandle};

pub fn version() -> &'static str {
    env!("CARGO_PKG_VERSION")
}

/// Where the chat lives: the homeserver (https://chat.darkride.ai), and a folder of its own for the session and the
/// SQLite stores (encryption keys among them).
#[derive(Deserialize)]
pub struct Config {
    pub homeserver: String,
    pub data_dir: PathBuf,
    #[serde(default = "default_device_name")]
    pub device_name: String,
}

fn default_device_name() -> String {
    "Darkride viewer".into()
}

/// How many of a timeline's newest items an event carries at first; open can ask for more, and each back adds its count
const TIMELINE_ITEMS: usize = 100;
/// the most of a room's items the viewer is sent, however far back it scrolls: every timeline event carries them all,
/// so a busy room's events stay small (the viewer stops asking at the same number, ChatWindow.cpp)
const TIMELINE_ITEMS_MAX: usize = 500;

pub type EventSink = Arc<dyn Fn(String) + Send + Sync>;

/// A running chat: a Tokio runtime of its own, and a queue of commands into it
pub struct Chat {
    runtime: Option<Runtime>,
    commands: mpsc::UnboundedSender<Value>,
    // commands sent and not finished yet
    busy: Arc<AtomicUsize>,
}

impl Chat {
    pub fn start(config: Config, events: EventSink) -> Result<Chat> {
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .thread_name("darkride-chat")
            .enable_all()
            .build()?;
        let (commands, queue) = mpsc::unbounded_channel();
        let busy = Arc::new(AtomicUsize::new(0));
        runtime.spawn(run(config, events, queue, busy.clone()));
        Ok(Chat { runtime: Some(runtime), commands, busy })
    }

    /// A command as JSON; its outcome comes back as events
    pub fn command(&self, json: &str) {
        self.busy.fetch_add(1, Ordering::SeqCst);
        // Windows tools often start text with a byte-order mark
        let json = json.trim_start_matches('\u{feff}');
        match serde_json::from_str::<Value>(json) {
            Ok(command) => {
                let _ = self.commands.send(command);
            }
            Err(e) => {
                // nowhere else to say it: the queue's reader would
                let _ = self.commands.send(json!({ "op": "invalid", "message": e.to_string() }));
            }
        }
    }
}

impl Chat {
    /// Waits, up to a time, until every command sent so far has finished: for a host about to stop (the spike at the
    /// end of its input), so a sign-in isn't cut off halfway. True if they all did.
    pub fn wait_idle(&self, timeout: Duration) -> bool {
        let until = Instant::now() + timeout;
        while self.busy.load(Ordering::SeqCst) > 0 {
            if Instant::now() >= until {
                return false;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        true
    }
}

impl Drop for Chat {
    fn drop(&mut self) {
        if let Some(runtime) = self.runtime.take() {
            runtime.shutdown_timeout(std::time::Duration::from_secs(2));
        }
    }
}

struct State {
    config: Config,
    events: EventSink,
    client: Mutex<Option<Client>>,
    sync: Mutex<Option<Arc<SyncService>>>,
    // the room list, the sync's state and the session's, while signed in
    tasks: Mutex<Vec<JoinHandle<()>>>,
    open: Mutex<HashMap<OwnedRoomId, OpenRoom>>,
}

/// An open room: its timeline, the task sending its events, and how many of its newest items they carry
struct OpenRoom {
    timeline: Arc<Timeline>,
    task: JoinHandle<()>,
    shown: Arc<AtomicUsize>,
}

impl State {
    fn emit(&self, event: Value) {
        (self.events)(event.to_string());
    }

    fn client(&self) -> Result<Client> {
        self.client.lock().unwrap().clone().ok_or_else(|| anyhow!("not signed in"))
    }

    fn session_file(&self) -> PathBuf {
        self.config.data_dir.join("session.json")
    }
}

async fn run(config: Config, events: EventSink, mut queue: mpsc::UnboundedReceiver<Value>, busy: Arc<AtomicUsize>) {
    let state = Arc::new(State {
        config,
        events,
        client: Mutex::new(None),
        sync: Mutex::new(None),
        tasks: Mutex::new(Vec::new()),
        open: Mutex::new(HashMap::new()),
    });

    // a session kept from before starts by itself
    match restore(&state).await {
        Ok(true) => {}
        Ok(false) => state.emit(json!({ "event": "state", "state": "signed_out" })),
        Err(e) => {
            state.emit(json!({ "event": "error", "op": "restore", "message": format!("{e:#}") }));
            state.emit(json!({ "event": "state", "state": "signed_out" }));
        }
    }

    // signing in and out finish before the next command starts (an "open" sent right after "login_jwt" waits for
    // it), and so do the commands that queue something to send (they only wait for the queue, not the server), so
    // messages go out in the order they were typed and an edit follows its message; the rest run side by side, so a
    // long pagination doesn't hold up a send
    while let Some(command) = queue.recv().await {
        let op = command["op"].as_str().unwrap_or("").to_owned();
        let in_turn = matches!(op.as_str(), "login_jwt" | "logout" | "send" | "edit" | "react" | "delete");
        let state = state.clone();
        let busy = busy.clone();
        let work = async move {
            // the session this command runs in, to ask about if it fails
            let client = state.client().ok();
            if let Err(e) = Box::pin(handle(&state, &op, &command)).await {
                let message = format!("{e:#}");
                // a sign-in's refusal, and anything to do with a line being sent, always shows: a line must never go
                // without a sign
                let quiet_kind = !matches!(op.as_str(), "login_jwt" | "send" | "edit" | "react" | "delete");
                let ended = match &client {
                    Some(client) if quiet_kind => refused_for_ended_session(&e) || session_ended(client).await,
                    _ => false,
                };
                if ended {
                    // the server ended the session while this was on its way: the session watcher signs out and the
                    // app signs in again, so this isn't an error of its own to show ("Couldn't open: … 401 …")
                    tracing::debug!("{op} refused, the session has ended: {message}");
                } else {
                    state.emit(json!({ "event": "error", "op": op, "message": message }));
                }
            }
            busy.fetch_sub(1, Ordering::SeqCst);
        };
        if in_turn {
            work.await;
        } else {
            tokio::spawn(work);
        }
    }
}

fn arg<'a>(command: &'a Value, name: &str) -> Result<&'a str> {
    command[name].as_str().ok_or_else(|| anyhow!("{name} is missing"))
}

async fn handle(state: &Arc<State>, op: &str, command: &Value) -> Result<()> {
    match op {
        "login_jwt" => login_jwt(state, arg(command, "token")?).await,
        "open" => {
            open(state, arg(command, "room")?).await?;
            // the room list syncs one event a room; a room opened shows a screenful
            let timeline = open_timeline(state, arg(command, "room")?).await?;
            let wanted = command["count"].as_u64().unwrap_or(30).min(200) as usize;
            show_more(state, &timeline, wanted.saturating_sub(TIMELINE_ITEMS)).await;
            let mut start = false;
            for _ in 0..5 {
                let shown = event_count(&timeline.items().await);
                if shown >= wanted {
                    break;
                }
                start = timeline.paginate_backwards((wanted - shown) as u16).await?;
                if start {
                    break;
                }
            }
            state.emit(json!({ "event": "done", "op": "open", "room": timeline.room().room_id(), "start": start }));
            Ok(())
        }
        "close" => {
            let room = room_by_id_or_alias(state, arg(command, "room")?).await?;
            if let Some(open) = state.open.lock().unwrap().remove(room.room_id()) {
                open.task.abort();
            }
            Ok(())
        }
        "back" => {
            let timeline = open_timeline(state, arg(command, "room")?).await?;
            let count = command["count"].as_u64().unwrap_or(20).min(200) as u16;
            let start = timeline.paginate_backwards(count).await?;
            // the items loaded before but not shown come in too, even when this page brought nothing new
            show_more(state, &timeline, count as usize).await;
            state.emit(json!({ "event": "done", "op": "back", "room": timeline.room().room_id(), "start": start }));
            Ok(())
        }
        "send" => {
            let timeline = open_timeline(state, arg(command, "room")?).await?;
            timeline.send(RoomMessageEventContent::text_plain(arg(command, "body")?).into()).await?;
            Ok(())
        }
        "react" => {
            let timeline = open_timeline(state, arg(command, "room")?).await?;
            let id = item_id(command)?;
            let wanted = arg(command, "key")?;
            // the way the message's reactions already write this emoji (yours first, so a second click takes it back),
            // else as given
            let me = timeline.room().own_user_id().to_owned();
            let key = find_item(&timeline, &id)
                .await
                .and_then(|item| {
                    let reactions = item.content().reactions()?;
                    let same: Vec<&String> = reactions.keys().filter(|key| same_emoji(key) == same_emoji(wanted)).collect();
                    let mine = same.iter().find(|key| reactions[key.as_str()].contains_key(&me)).copied();
                    mine.or(same.first().copied()).cloned()
                })
                .unwrap_or_else(|| wanted.to_owned());
            timeline.toggle_reaction(&id, &key).await?;
            Ok(())
        }
        "edit" => {
            let timeline = open_timeline(state, arg(command, "room")?).await?;
            let content = RoomMessageEventContentWithoutRelation::text_plain(arg(command, "body")?);
            timeline.edit(&item_id(command)?, EditedContent::RoomMessage(content)).await?;
            Ok(())
        }
        "delete" => {
            let timeline = open_timeline(state, arg(command, "room")?).await?;
            let id = item_id(command)?;
            let item = find_item(&timeline, &id).await.ok_or_else(|| anyhow!("no such message"))?;
            if !item.is_own() {
                return Err(anyhow!("not your message"));
            }
            timeline.redact(&id, None).await?;
            Ok(())
        }
        "read" => {
            let timeline = open_timeline(state, arg(command, "room")?).await?;
            timeline.mark_as_read(ReceiptType::Read).await?;
            Ok(())
        }
        "join" => {
            let client = state.client()?;
            let target = arg(command, "room")?;
            let invited = <&RoomId>::try_from(target)
                .ok()
                .and_then(|id| client.get_room(id))
                .filter(|room| room.state() == RoomState::Invited);
            let room = match invited {
                Some(room) => {
                    room.join().await?;
                    room
                }
                None => client.join_room_by_id_or_alias(<&RoomOrAliasId>::try_from(target)?, &[]).await?,
            };
            state.emit(json!({ "event": "done", "op": "join", "room": room.room_id() }));
            Ok(())
        }
        "leave" => {
            let room = room_by_id_or_alias(state, arg(command, "room")?).await?;
            if let Some(open) = state.open.lock().unwrap().remove(room.room_id()) {
                open.task.abort();
            }
            room.leave().await?;
            state.emit(json!({ "event": "done", "op": "leave", "room": room.room_id() }));
            Ok(())
        }
        "create_dm" => {
            let client = state.client()?;
            let user = <&UserId>::try_from(arg(command, "user")?)?;
            // the one you already have with them, if any: asked twice (two clicks, or before the room list came in),
            // there's still one conversation
            if let Some(room) = client.get_dm_room(user) {
                state.emit(json!({ "event": "done", "op": "create_dm", "room": room.room_id() }));
                return Ok(());
            }
            // encrypted only when asked, as in the web client, which encrypts only behind ?e2ee=1 until round 5: a DM
            // the other side can't decrypt is worse than one that isn't encrypted (CHAT.md 4.9)
            let initial_state = if command["encrypted"].as_bool().unwrap_or(false) {
                vec![InitialStateEvent::with_empty_state_key(RoomEncryptionEventContent::with_recommended_defaults()).to_raw_any()]
            } else {
                vec![]
            };
            let mut request = create_room::v3::Request::new();
            request.invite = vec![user.to_owned()];
            request.is_direct = true;
            request.preset = Some(create_room::v3::RoomPreset::TrustedPrivateChat);
            request.initial_state = initial_state;
            let room = client.create_room(request).await?;
            state.emit(json!({ "event": "done", "op": "create_dm", "room": room.room_id() }));
            Ok(())
        }
        "media" => {
            let key = arg(command, "key")?.to_owned();
            match fetch_media(state, command).await {
                Ok(path) => state.emit(json!({ "event": "media", "key": key, "path": path })),
                Err(e) => state.emit(json!({ "event": "media", "key": key, "error": format!("{e:#}") })),
            }
            Ok(())
        }
        // the account's own name, for one that has none yet (ChatService, on its profile event)
        "set_display_name" => {
            let client = state.client()?;
            let name = arg(command, "name")?;
            client.account().set_display_name(Some(name)).await?;
            state.emit(json!({ "event": "done", "op": "set_display_name", "name": name }));
            Ok(())
        }
        "logout" => {
            forget(state, true).await;
            state.emit(json!({ "event": "state", "state": "signed_out" }));
            Ok(())
        }
        "invalid" => Err(anyhow!("not JSON: {}", command["message"].as_str().unwrap_or(""))),
        _ => Err(anyhow!("no such command")),
    }
}

async fn build_client(state: &State, store: &Path) -> Result<Client> {
    std::fs::create_dir_all(&state.config.data_dir)?;
    Ok(Client::builder()
        .homeserver_url(&state.config.homeserver)
        .sqlite_store(store, None)
        .build()
        .await?)
}

/// names the store a sign-in made when "store" couldn't be deleted for it, for the next start's restore
const STORE_NAME_FILE: &str = "store-name";

fn is_store_name(name: &str) -> bool {
    name == "store" || name.strip_prefix("store-").is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
}

/// the kept session's store: "store", or the one store-name names
fn store_dir(state: &State) -> PathBuf {
    let dir = &state.config.data_dir;
    match std::fs::read_to_string(dir.join(STORE_NAME_FILE)) {
        Ok(name) if is_store_name(name.trim()) => dir.join(name.trim()),
        _ => dir.join("store"),
    }
}

/// Deletes every store in the folder but keep, and says which stay (on Windows, one with a file still open) and why
fn remove_stores(dir: &Path, keep: Option<&Path>) -> Vec<(PathBuf, std::io::Error)> {
    let mut left = Vec::new();
    let Ok(entries) = std::fs::read_dir(dir) else { return left };
    for entry in entries.flatten() {
        let path = entry.path();
        if is_store_name(&entry.file_name().to_string_lossy()) && path.is_dir() && Some(path.as_path()) != keep {
            if let Err(e) = std::fs::remove_dir_all(&path) {
                if path.exists() {
                    left.push((path, e));
                }
            }
        }
    }
    left
}

/// A new sign-in is a new device: nothing an earlier one left in the folder (its session, keys, stored messages,
/// files) is any use to it, and the next person at this computer shouldn't find it. A store that can't be deleted
/// never stops the sign-in (it made the crypto store refuse every one, 11-08): a new store goes beside it, named in
/// store-name for the next start, and the old one goes at the next sign-in or sign-out.
fn fresh_store(state: &State) -> PathBuf {
    let dir = &state.config.data_dir;
    let _ = std::fs::remove_file(state.session_file());
    let _ = std::fs::remove_file(dir.join(STORE_NAME_FILE));
    let _ = std::fs::remove_dir_all(dir.join("media"));
    let left = remove_stores(dir, None);
    if !left.iter().any(|(path, _)| path.ends_with("store")) {
        return dir.join("store");
    }
    for (path, e) in &left {
        state.emit(json!({ "event": "error", "op": "store", "message": format!("an earlier store stays in {}: {e}; signing in with a new one", path.display()) }));
    }
    let millis = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_millis());
    let name = format!("store-{millis}");
    let _ = std::fs::create_dir_all(dir);
    let _ = std::fs::write(dir.join(STORE_NAME_FILE), &name);
    dir.join(name)
}

async fn restore(state: &Arc<State>) -> Result<bool> {
    let Ok(saved) = std::fs::read_to_string(state.session_file()) else { return Ok(false) };
    let session: MatrixSession = serde_json::from_str(&saved).context("the kept session")?;
    let store = store_dir(state);
    // a store an earlier sign-in couldn't delete (store-name names the newer one): gone now that nothing has it open
    let _ = remove_stores(&state.config.data_dir, Some(&store));
    let client = build_client(state, &store).await?;
    client.restore_session(session).await?;
    started(state, client).await?;
    Ok(true)
}

async fn login_jwt(state: &Arc<State>, token: &str) -> Result<()> {
    if state.client.lock().unwrap().is_some() {
        return Err(anyhow!("already signed in"));
    }
    state.emit(json!({ "event": "state", "state": "starting" }));
    let store = fresh_store(state);
    let client = match build_client(state, &store).await {
        Ok(client) => client,
        Err(e) => {
            state.emit(json!({ "event": "state", "state": "signed_out" }));
            return Err(e);
        }
    };
    let mut data = matrix_sdk::ruma::serde::JsonObject::new();
    data.insert("token".into(), token.into());
    let login = client
        .matrix_auth()
        .login_custom("org.matrix.login.jwt", data)?
        .initial_device_display_name(&state.config.device_name)
        .await;
    if let Err(e) = login {
        // refused: the store made for it goes too (the client first, which holds it open)
        drop(client);
        let _ = std::fs::remove_dir_all(&store);
        let _ = std::fs::remove_file(state.config.data_dir.join(STORE_NAME_FILE));
        state.emit(json!({ "event": "state", "state": "signed_out" }));
        return Err(e.into());
    }
    let session = client.matrix_auth().session().ok_or_else(|| anyhow!("signed in, but no session"))?;
    write_private(&state.session_file(), &serde_json::to_string(&session)?)?;
    started(state, client).await
}

/// the session file holds the access token: only this account may read it
fn write_private(path: &PathBuf, text: &str) -> Result<()> {
    let new = path.with_extension("new");
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        let mut file = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&new)?;
        std::io::Write::write_all(&mut file, text.as_bytes())?;
    }
    #[cfg(not(unix))]
    std::fs::write(&new, text)?;
    std::fs::rename(&new, path)?;
    Ok(())
}

async fn started(state: &Arc<State>, client: Client) -> Result<()> {
    // offline mode: when the server can't be reached the service waits for it and carries on by itself
    let sync = Arc::new(SyncService::builder(client.clone()).with_offline_mode().build().await?);
    sync.start().await;
    let rooms = sync.room_list_service().all_rooms().await?;
    *state.client.lock().unwrap() = Some(client.clone());
    *state.sync.lock().unwrap() = Some(sync.clone());
    let mut tasks = Vec::new();

    // the sync's state, for the panel ("Reconnecting…"); after an error it starts again in a while
    let sync_state = state.clone();
    let sync_client = client.clone();
    tasks.push(tokio::spawn(async move {
        let mut states = sync.state();
        let mut now = states.get();
        loop {
            let (name, message) = match &now {
                SyncState::Idle => ("idle", None),
                SyncState::Running => ("running", None),
                SyncState::Terminated => ("terminated", None),
                SyncState::Offline => ("offline", None),
                SyncState::Error(e) => ("error", Some(e.to_string())),
            };
            sync_state.emit(json!({ "event": "sync", "state": name, "message": message }));
            // a network error pauses sending; messages typed meanwhile go out once the server is back
            if matches!(now, SyncState::Running) {
                sync_client.send_queue().set_enabled(true).await;
            }
            if matches!(now, SyncState::Error(_)) {
                tokio::time::sleep(std::time::Duration::from_secs(5)).await;
                sync.start().await;
            }
            match states.next().await {
                Some(next) => now = next,
                None => break,
            }
        }
    }));

    // a session the server has ended (signed out elsewhere, the account closed, its device removed): signed out here
    // too. The SDK's channel for these keeps one change, so when two requests are refused at once (a send and the
    // sync) this falls behind ("lagged"); the change it then reads is the newest one, so it carries on, never stops
    // there, or the app would stay "ready" with every request refused
    let session_state = state.clone();
    let mut changes = client.subscribe_to_session_changes();
    tasks.push(tokio::spawn(async move {
        loop {
            let change = match changes.recv().await {
                Ok(change) => change,
                Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => continue,
                Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
            };
            if let SessionChange::UnknownToken(_) = change {
                let state = session_state.clone();
                // from a task of its own: forgetting stops this one
                tokio::spawn(async move {
                    forget(&state, false).await;
                    state.emit(json!({ "event": "state", "state": "signed_out", "reason": "the server ended this session" }));
                });
                break;
            }
        }
    }));

    let rooms_state = state.clone();
    tasks.push(tokio::spawn(async move {
        let (stream, controller) = rooms.entries_with_dynamic_adapters(200);
        controller.set_filter(Box::new(new_filter_non_left()));
        let mut stream = std::pin::pin!(stream);
        let mut list: Vector<RoomListItem> = Vector::new();
        while let Some(diffs) = stream.next().await {
            for diff in diffs {
                diff.apply(&mut list);
            }
            // the joined spaces (Darkride's), and which rooms each lists as its children: the channels
            let mut spaces = Vec::new();
            let mut parents: HashMap<OwnedRoomId, OwnedRoomId> = HashMap::new();
            for item in list.iter().filter(|item| item.is_space() && item.state() == RoomState::Joined) {
                for child in space_children(item).await {
                    parents.entry(child).or_insert_with(|| item.room_id().to_owned());
                }
                spaces.push(json!({
                    "id": item.room_id(),
                    "name": item.cached_display_name().map(|n| n.to_string()).unwrap_or_default(),
                    "avatar": item.avatar_url(),
                }));
            }
            let mut out = Vec::with_capacity(list.len());
            for item in list.iter() {
                if item.is_space() {
                    continue;
                }
                out.push(room_json(item, parents.get(item.room_id())).await);
            }
            rooms_state.emit(json!({ "event": "rooms", "rooms": out, "spaces": spaces }));
        }
    }));
    // General and the space: the server's own auto-join skips sign-ins with a chat token, so a new account would
    // otherwise start with no rooms at all
    let join_state = state.clone();
    let join_client = client.clone();
    tasks.push(tokio::spawn(async move {
        if let Err(e) = join_general(&join_client).await {
            join_state.emit(json!({ "event": "error", "op": "join_general", "message": format!("{e:#}") }));
        }
    }));
    *state.tasks.lock().unwrap() = tasks;
    state.emit(json!({ "event": "state", "state": "ready", "user": client.user_id() }));

    // who you are, for the panel's corner: asked of the server once
    let profile_state = state.clone();
    let account = client.account();
    tokio::spawn(async move {
        let name = account.get_display_name().await.ok().flatten();
        let avatar = account.get_avatar_url().await.ok().flatten();
        profile_state.emit(json!({ "event": "profile", "name": name, "avatar": avatar }));
    });
    Ok(())
}

/// The rooms a space lists as its children (m.space.child state events; one with no "via" has been taken out)
async fn space_children(space: &Room) -> Vec<OwnedRoomId> {
    let Ok(events) = space.get_state_events_static::<SpaceChildEventContent>().await else { return Vec::new() };
    events
        .iter()
        .filter_map(|raw| {
            let json = match raw {
                RawSyncOrStrippedState::Sync(event) => event.json().get(),
                RawSyncOrStrippedState::Stripped(event) => event.json().get(),
            };
            let event: Value = serde_json::from_str(json).ok()?;
            if event["content"]["via"].as_array().is_none_or(|via| via.is_empty()) {
                return None;
            }
            OwnedRoomId::try_from(event["state_key"].as_str()?).ok()
        })
        .collect()
}

async fn room_json(room: &Room, space: Option<&OwnedRoomId>) -> Value {
    let mut name = match room.cached_display_name() {
        Some(name) => name.to_string(),
        None => room.display_name().await.map(|n| n.to_string()).unwrap_or_default(),
    };
    let mut avatar = room.avatar_url().map(|url| url.to_string());
    let dm = room.is_direct().await.unwrap_or(false);
    let mut dm_user = None;
    if dm {
        // named and pictured after the other person, as the web chat does: without the server's summary of who is
        // in it, the name computed for a DM can be "2 people"
        let other = room.direct_targets().iter().find_map(|target| target.as_user_id().map(|user| user.to_owned()));
        dm_user = other.clone();
        if let Some(other) = other {
            match room.get_member_no_sync(&other).await {
                Ok(Some(member)) => {
                    name = member.name().to_owned();
                    if avatar.is_none() {
                        avatar = member.avatar_url().map(|url| url.to_string());
                    }
                }
                _ => name = other.localpart().to_owned(),
            }
        }
    }
    json!({
        "id": room.room_id(),
        "name": name,
        "avatar": avatar,
        "space": space,
        "members": room.joined_members_count(),
        "dm": dm,
        "dm_user": dm_user,
        "encrypted": room.encryption_state().is_encrypted(),
        "invited": room.state() == RoomState::Invited,
        "unread": room.num_unread_messages(),
        "notifications": room.unread_notification_counts().notification_count,
    })
}

/// joins General (#general, else #lobby on older servers) and Darkride's space (#darkride) when the server says you
/// aren't in them; the ones you're in are left alone
async fn join_general(client: &Client) -> Result<()> {
    let server = client.user_id().ok_or_else(|| anyhow!("not signed in"))?.server_name().to_owned();
    let joined = client.send(joined_rooms::v3::Request::new()).await?.joined_rooms;
    for names in [&["general", "lobby"][..], &["darkride"][..]] {
        for name in names {
            let alias = RoomAliasId::parse(format!("#{name}:{server}"))?;
            // a name this server doesn't have: the next one
            let Ok(found) = client.resolve_room_alias(&alias).await else {
                continue;
            };
            if !joined.contains(&found.room_id) {
                client.join_room_by_id_or_alias(<&RoomOrAliasId>::from(&*found.room_id), &found.servers).await?;
            }
            break;
        }
    }
    Ok(())
}

async fn room_by_id_or_alias(state: &State, target: &str) -> Result<Room> {
    let client = state.client()?;
    let id: OwnedRoomId = if target.starts_with('#') {
        // a room you're in is found by its names here, without asking the server (which may be away)
        let alias = <&RoomAliasId>::try_from(target)?;
        let known = client.rooms().into_iter().find(|room| {
            room.canonical_alias().as_deref() == Some(alias) || room.alt_aliases().iter().any(|a| a == alias)
        });
        match known {
            Some(room) => return Ok(room),
            None => client.resolve_room_alias(alias).await?.room_id,
        }
    } else {
        target.try_into()?
    };
    if let Some(room) = client.get_room(&id) {
        return Ok(room);
    }
    // right after signing in the first sync may not have brought it yet
    tokio::time::timeout(std::time::Duration::from_secs(10), client.await_room_remote_echo(&id))
        .await
        .map_err(|_| anyhow!("not in {target}"))
}

fn event_count(items: &Vector<Arc<TimelineItem>>) -> usize {
    items.iter().filter(|item| item.as_event().is_some()).count()
}

/// a message's id as the timeline events give it: the event's id, or txn:<transaction id>
fn item_id(command: &Value) -> Result<TimelineEventItemId> {
    let id = arg(command, "id")?;
    Ok(match id.strip_prefix("txn:") {
        Some(txn) => TimelineEventItemId::TransactionId(<&TransactionId>::from(txn).to_owned()),
        None => TimelineEventItemId::EventId(<&EventId>::try_from(id)?.to_owned()),
    })
}

fn item_id_text(event: &EventTimelineItem) -> String {
    match event.identifier() {
        TimelineEventItemId::EventId(id) => id.to_string(),
        TimelineEventItemId::TransactionId(txn) => format!("txn:{txn}"),
    }
}

async fn find_item(timeline: &Timeline, id: &TimelineEventItemId) -> Option<EventTimelineItem> {
    match id {
        TimelineEventItemId::EventId(id) => timeline.item_by_event_id(id).await,
        // a message of yours, sent or not yet
        TimelineEventItemId::TransactionId(txn) => timeline
            .items()
            .await
            .iter()
            .rev()
            .filter_map(|item| item.as_event())
            .find(|event| event.transaction_id() == Some(txn))
            .cloned(),
    }
}

/// the same emoji with and without its emoji-style mark (U+FE0F), which apps send either way
fn same_emoji(key: &str) -> String {
    key.replace('\u{fe0f}', "")
}

/// an open room's timeline events carry count more of its items from now on, starting with one now
async fn show_more(state: &Arc<State>, timeline: &Timeline, count: usize) {
    let room = timeline.room().room_id().to_owned();
    let Some(shown) = state.open.lock().unwrap().get(&room).map(|open| open.shown.clone()) else { return };
    if count == 0 {
        return;
    }
    let more = (shown.load(Ordering::SeqCst) + count).min(TIMELINE_ITEMS_MAX);
    shown.store(more, Ordering::SeqCst);
    state.emit(timeline_json(&room, &timeline.items().await, more, timeline.room().own_user_id()));
}

async fn open_timeline(state: &Arc<State>, target: &str) -> Result<Arc<Timeline>> {
    let room = room_by_id_or_alias(state, target).await?;
    if let Some(open) = state.open.lock().unwrap().get(room.room_id()) {
        return Ok(open.timeline.clone());
    }
    open(state, target).await?;
    let open = state.open.lock().unwrap();
    Ok(open.get(room.room_id()).ok_or_else(|| anyhow!("closed"))?.timeline.clone())
}

async fn open(state: &Arc<State>, target: &str) -> Result<()> {
    let room = room_by_id_or_alias(state, target).await?;
    if state.open.lock().unwrap().contains_key(room.room_id()) {
        return Ok(());
    }
    let timeline = Arc::new(room.timeline().await?);
    let (items, mut stream) = timeline.subscribe().await;
    let room_id = room.room_id().to_owned();
    let me = room.own_user_id().to_owned();
    let shown = Arc::new(AtomicUsize::new(TIMELINE_ITEMS));
    let events = state.clone();
    let id = room_id.clone();
    let count = shown.clone();
    let refill = timeline.clone();
    let task = tokio::spawn(async move {
        let mut items = items;
        let mut had = event_count(&items);
        let refilling = Arc::new(std::sync::atomic::AtomicBool::new(false));
        events.emit(timeline_json(&id, &items, count.load(Ordering::SeqCst), &me));
        while let Some(diffs) = stream.next().await {
            for diff in diffs {
                diff.apply(&mut items);
            }
            events.emit(timeline_json(&id, &items, count.load(Ordering::SeqCst), &me));
            // after a gap in the sync (the server skipped some events) the SDK keeps only the newest of the room's
            // events in memory and leaves the rest in the store: they're loaded back, from the store, so the
            // conversation doesn't shrink under the reader (and edit, react and delete still find its messages)
            let now = event_count(&items);
            let wanted = had.min(count.load(Ordering::SeqCst));
            if now >= wanted {
                had = now;
            } else if !refilling.swap(true, Ordering::SeqCst) {
                let timeline = refill.clone();
                let refilling = refilling.clone();
                tokio::spawn(async move {
                    for _ in 0..10 {
                        let have = event_count(&timeline.items().await);
                        if have >= wanted || !matches!(timeline.paginate_backwards((wanted - have).min(200) as u16).await, Ok(false)) {
                            break;
                        }
                    }
                    refilling.store(false, Ordering::SeqCst);
                });
            }
        }
    });
    let mut open = state.open.lock().unwrap();
    if open.contains_key(&room_id) {
        // opened twice at once: keep the first
        task.abort();
    } else {
        open.insert(room_id, OpenRoom { timeline, task, shown });
    }
    Ok(())
}

fn timeline_json(room: &OwnedRoomId, items: &Vector<Arc<TimelineItem>>, count: usize, me: &UserId) -> Value {
    let shown: Vec<Value> = items
        .iter()
        .skip(items.len().saturating_sub(count))
        .filter_map(|item| {
            let event = item.as_event()?;
            let content = event.content();
            let mut media = Value::Null;
            let mut member = Value::Null;
            let (kind, body) = if content.is_unable_to_decrypt() {
                ("undecryptable", String::new())
            } else if let Some(message) = content.as_message() {
                let kind = match message.msgtype() {
                    MessageType::Text(_) => "text",
                    MessageType::Emote(_) => "emote",
                    MessageType::Notice(_) => "notice",
                    MessageType::Image(image) => {
                        let info = image.info.as_deref();
                        media = media_json(
                            &image.source,
                            info.and_then(|i| i.thumbnail_source.as_ref()),
                            info.and_then(|i| i.mimetype.as_deref()),
                            info.and_then(|i| i.width),
                            info.and_then(|i| i.height),
                            info.and_then(|i| i.size),
                        );
                        "image"
                    }
                    MessageType::File(file) => {
                        let info = file.info.as_deref();
                        media = media_json(
                            &file.source,
                            info.and_then(|i| i.thumbnail_source.as_ref()),
                            info.and_then(|i| i.mimetype.as_deref()),
                            None,
                            None,
                            info.and_then(|i| i.size),
                        );
                        "file"
                    }
                    MessageType::Video(video) => {
                        let info = video.info.as_deref();
                        media = media_json(
                            &video.source,
                            info.and_then(|i| i.thumbnail_source.as_ref()),
                            info.and_then(|i| i.mimetype.as_deref()),
                            info.and_then(|i| i.width),
                            info.and_then(|i| i.height),
                            info.and_then(|i| i.size),
                        );
                        "video"
                    }
                    MessageType::Audio(audio) => {
                        let info = audio.info.as_deref();
                        media = media_json(
                            &audio.source,
                            None,
                            info.and_then(|i| i.mimetype.as_deref()),
                            None,
                            None,
                            info.and_then(|i| i.size),
                        );
                        "audio"
                    }
                    _ => "other",
                };
                (kind, message.body().to_owned())
            } else if let TimelineItemContent::MembershipChange(change) = content {
                // joined, left and invited lines, as the web chat shows them
                let what = match change.change()? {
                    MembershipChange::Joined | MembershipChange::InvitationAccepted => "joined",
                    MembershipChange::Left => "left",
                    MembershipChange::Invited => "invited",
                    MembershipChange::Kicked => "kicked",
                    MembershipChange::Banned | MembershipChange::KickedAndBanned => "banned",
                    MembershipChange::InvitationRejected => "declined",
                    MembershipChange::InvitationRevoked => "uninvited",
                    _ => return None,
                };
                member = json!({
                    "change": what,
                    "user": change.user_id(),
                    "user_name": change.display_name().unwrap_or_else(|| change.user_id().localpart().to_owned()),
                });
                ("member", String::new())
            } else {
                return None;
            };
            let (name, avatar) = match event.sender_profile() {
                TimelineDetails::Ready(profile) => (profile.display_name.clone(), profile.avatar_url.clone()),
                _ => (None, None),
            };
            let send = match event.send_state() {
                None | Some(EventSendState::Sent { .. }) => "sent",
                Some(EventSendState::NotSentYet { .. }) => "sending",
                // waiting: it goes out once the server is back; failed: it won't
                Some(EventSendState::SendingFailed { is_recoverable: true, .. }) => "waiting",
                Some(EventSendState::SendingFailed { .. }) => "failed",
            };
            Some(json!({
                "id": item_id_text(event),
                "sender": event.sender(),
                "name": name,
                "avatar": avatar,
                "media": media,
                "change": member["change"],
                "user": member["user"],
                "user_name": member["user_name"],
                "body": body,
                "kind": kind,
                "ts": event.timestamp().0,
                "own": event.is_own(),
                "encrypted": event.encryption_info().is_some(),
                "state": send,
                "reactions": reactions_json(content, me),
                "edited": content.as_message().is_some_and(|message| message.is_edited()),
                "editable": event.is_own() && event.is_editable() && matches!(kind, "text" | "emote" | "notice"),
            }))
        })
        .collect();
    json!({ "event": "timeline", "room": room, "items": shown })
}

/// a message's reactions by emoji (with and without U+FE0F as one): [{key as the first sender wrote it, count, mine,
/// senders}], in the order they were first given
fn reactions_json(content: &TimelineItemContent, me: &UserId) -> Value {
    let mut groups: Vec<(String, String, Vec<String>, bool)> = Vec::new();
    for (key, senders) in content.reactions().into_iter().flat_map(|reactions| reactions.iter()) {
        let folded = same_emoji(key);
        let group = match groups.iter().position(|group| group.0 == folded) {
            Some(i) => &mut groups[i],
            None => {
                groups.push((folded, key.clone(), Vec::new(), false));
                groups.last_mut().unwrap()
            }
        };
        for sender in senders.keys() {
            if !group.2.iter().any(|s| s == sender.as_str()) {
                group.2.push(sender.to_string());
            }
            group.3 |= sender == me;
        }
    }
    groups
        .into_iter()
        .filter(|group| !group.2.is_empty())
        .map(|(_, key, senders, mine)| json!({ "key": key, "count": senders.len(), "mine": mine, "senders": senders }))
        .collect()
}

fn media_json(
    source: &MediaSource,
    thumbnail: Option<&MediaSource>,
    mimetype: Option<&str>,
    width: Option<UInt>,
    height: Option<UInt>,
    size: Option<UInt>,
) -> Value {
    json!({
        "source": source,
        "thumbnail_source": thumbnail,
        "mimetype": mimetype,
        "width": width,
        "height": height,
        "size": size,
    })
}

/// A media command: the file kept in the data folder's media/, fetched (and decrypted) the first time
/// media fetched at once, whoever asks (a room of pictures opened fetches them a few at a time, not all together)
static MEDIA_SLOTS: tokio::sync::Semaphore = tokio::sync::Semaphore::const_new(4);
/// the largest media file written to the chat's folder; the homeserver's own upload limit is the same
const MEDIA_MAX_BYTES: usize = 50 * 1024 * 1024;

async fn fetch_media(state: &Arc<State>, command: &Value) -> Result<String> {
    let client = state.client()?;
    let source: MediaSource = serde_json::from_value(command["source"].clone()).context("the media's source")?;
    let format = match (command["width"].as_u64(), command["height"].as_u64()) {
        (Some(width), Some(height)) => {
            let method = if command["crop"].as_bool().unwrap_or(false) { Method::Crop } else { Method::Scale };
            MediaFormat::Thumbnail(MediaThumbnailSettings::with_method(
                method,
                UInt::try_from(width)?,
                UInt::try_from(height)?,
            ))
        }
        _ => MediaFormat::File,
    };
    let request = MediaRequestParameters { source, format };
    // a name from what was asked for, so the same picture at the same size is fetched once
    let mut hasher = DefaultHasher::new();
    serde_json::to_string(&request)?.hash(&mut hasher);
    let folder = state.config.data_dir.join("media");
    let path = folder.join(format!("{:016x}", hasher.finish()));
    if !path.exists() {
        let _slot = MEDIA_SLOTS.acquire().await?;
        // the SDK's own cache would keep a second copy in its database
        let bytes = client.media().get_media_content(&request, false).await?;
        if bytes.len() > MEDIA_MAX_BYTES {
            return Err(anyhow!("too big to fetch ({} MB)", bytes.len() / (1024 * 1024)));
        }
        std::fs::create_dir_all(&folder)?;
        let partial = path.with_extension("part");
        std::fs::write(&partial, bytes)?;
        std::fs::rename(&partial, &path)?;
    }
    Ok(path.to_string_lossy().into_owned())
}

/// Whether a request was refused because the server no longer knows this session's access token (it ended the
/// session: signed out everywhere, a password change, an admin): the SDK's own error, with the server's error kind,
/// among the error's causes. Never a match on the text, which could quote anything. Some of the SDK's wrappers pass
/// their inner error's message on but not the error itself, so session_ended asks the server when this can't tell.
fn refused_for_ended_session(e: &anyhow::Error) -> bool {
    e.chain().any(|cause| {
        let kind = if let Some(http) = cause.downcast_ref::<matrix_sdk::HttpError>() {
            http.client_api_error_kind()
        } else if let Some(sdk) = cause.downcast_ref::<matrix_sdk::Error>() {
            sdk.client_api_error_kind()
        } else {
            None
        };
        unknown_token(kind)
    })
}

/// Whether the server has ended this session: it refuses the session's token (one small request, made only after
/// something has failed)
async fn session_ended(client: &Client) -> bool {
    match client.whoami().await {
        Err(e) => unknown_token(e.client_api_error_kind()),
        Ok(_) => false,
    }
}

fn unknown_token(kind: Option<&matrix_sdk::ruma::api::error::ErrorKind>) -> bool {
    matches!(kind, Some(matrix_sdk::ruma::api::error::ErrorKind::UnknownToken(_)))
}

/// Signed out: everything stopped and waited for (Windows can't remove files still open), the server told when asked
/// (it forgets the device), then the session, the stores (keys too) and the fetched media, gone from the folder
async fn forget(state: &Arc<State>, tell_server: bool) {
    let mut tasks: Vec<JoinHandle<()>> = state.open.lock().unwrap().drain().map(|(_, open)| open.task).collect();
    tasks.extend(state.tasks.lock().unwrap().drain(..));
    for task in &tasks {
        task.abort();
    }
    for task in tasks {
        let _ = task.await;
    }
    let sync = state.sync.lock().unwrap().take();
    if let Some(sync) = sync {
        sync.stop().await;
    }
    let client = state.client.lock().unwrap().take();
    if let Some(client) = client {
        if tell_server {
            let _ = client.logout().await;
        }
    }
    let _ = std::fs::remove_file(state.session_file());
    // the pictures and files fetched for this account go too: the next person on this computer shouldn't find them
    let _ = std::fs::remove_dir_all(state.config.data_dir.join("media"));
    let _ = std::fs::remove_file(state.config.data_dir.join(STORE_NAME_FILE));
    // on Windows a store's files can stay open in this process until the library stops: the viewer deletes the
    // folder again once it has (ChatService::forgetChatData)
    for (store, e) in remove_stores(&state.config.data_dir, None) {
        state.emit(json!({ "event": "error", "op": "logout", "message": format!("the chat's store stays in {}: {e}", store.display()) }));
    }
}
