# Contributing to Darkride

Thank you for helping. This page covers how a change gets into Darkride, what needs a second look, and what never
goes in.

## How a change gets in

1. Fork the repository, or push a branch here if you're a collaborator. Make a branch for the change, rather than
   working on your fork's `main`.
2. Open a pull request from that branch into `main`. Say what it changes, why, and how you tested it, including on
   which system. If it fixes an issue, write "Fixes #123" in the description, and the issue closes when it merges.
3. Someone with write access other than you approves it, and then it can be merged. If you push again after an
   approval, it needs a fresh one.
4. Nobody pushes straight to `main`, and nobody force-pushes it. GitHub enforces both.

Keep each pull request to one change. Two unrelated fixes are easier to review, test and undo as two pull requests.

## Who does what

- **Caity** ([@caitlynmeeks](https://github.com/caitlynmeeks)) owns the project. She has the final say on what
  goes in, and she alone makes releases and their tags, runs the live worlds' servers, and holds the signing keys.
- **Collaborators** with write access review and merge pull requests, and sort issues. A collaborator's own pull
  request is approved by someone else.

## Ask Caity before merging

- **Network protocol changes.** These are any change to the packet types or their versions in
  `libraries/networking/src/udt/PacketHeaders.h` and `.cpp`, and any change to what a packet carries (avatar data,
  entity properties and so on). The protocol signature is a hash of how many packet types there are and each one's
  version, so adding a type or bumping a version changes it. A client built from such a change can't join the live
  worlds until their servers are updated, so it ships together with a server update. Put "(protocol change)" in the
  pull request's title.

  A change to what a packet carries needs that packet's version bumped. Without the bump the signature stays the
  same, so old and new builds still connect, and each silently misreads the other's packets.
- **New dependencies,** or anything the build downloads.
- **Sign-in and accounts.**

## What never goes in

- **Keys, tokens, passwords or server addresses.** GitHub's push protection catches some of them, but not all.
- **Personal details.** A path to your own home folder in a script counts, so use `$HOME` instead.
- **Assets we can't give away.** Only your own work, CC0, or a licence that allows redistribution. Never paid
  assets, such as avatars bought on Booth.

## The name

The app is called **Darkride** everywhere a person sees it: menus, dialogs, window titles, installers and file
names. The older names (Interface, Tivoli Cloud VR, High Fidelity) stay only where nobody sees them, such as in
code, settings folders and licence notices, until they're migrated.

## Commit messages

Write a sentence a person can follow: the area, what changed and why. For example, "FBX: a node with fewer properties
than expected reads as empty instead of crashing the client".

## Building

- [BUILD_WINDOWS.md](BUILD_WINDOWS.md), [BUILD_LINUX.md](BUILD_LINUX.md) and [BUILD_MACOS.md](BUILD_MACOS.md)
  cover building. BUILD_MACOS.md predates the Apple silicon build: [RELEASING.md](RELEASING.md) shows how the Mac
  release is built today.
- [CODING_STANDARD.md](CODING_STANDARD.md) is the code style.
- **To join the live worlds, build from the latest release tag, not `main`.** `main` can be ahead of the live
  servers.

## Reporting a bug

Open an issue. Say what you did, what happened and what you expected, along with your system and the version
(Help > About). Attach the log if you can.

## Releases

[RELEASING.md](RELEASING.md) describes how a release and its downloads are made.
