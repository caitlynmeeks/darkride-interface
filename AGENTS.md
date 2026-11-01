# Notes for AI coding agents

This page is for AI coding agents working in this repository. Claude Code reads [CLAUDE.md](CLAUDE.md), which
brings in this page; other agents read this page directly. [CONTRIBUTING.md](CONTRIBUTING.md) is the full guide for
everyone, people and agents alike.

## The vehicle path is frozen

Since 2026-10-31 the code that moves vehicles and the people riding them is frozen: the monorail and other rides on
a track, vehicle physics, the camera on a ride, and the parts of the frame loop and the renderer that share a
vehicle's per-frame time. It is tuned to the millisecond, and small "harmless" changes have broken the ride before
(strobing, judder, frame drops).

**Before you edit a file, read its top.** A frozen file has this note just under its license header:

```
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
```

The note's "Frozen here:" line says whether the whole file is frozen or only some of it. In a partly frozen file,
each frozen function or member also has this marker just above it:

```
// FROZEN (vehicle path): see the note at the top of this file.
```

## The rule

Don't change frozen code, and don't work around it: no copy of a frozen function with your edit in it, no wrapper
or override that changes what it does. A change to frozen code happens only this way:

1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a lighter
   model), with one written master plan agreed before any code;
2. the full vehicle check runs, ending with Caity's own ride at the display's real rate in a heavy world;
3. it is merged only as checked.

A change outside the frozen files also counts when it shares the vehicle's per-frame time in a way that could change
the ride: entity updates, the physics step, the camera, the render loop, presents and pacing, the shadow pass. When
unsure whether a change counts, it counts.

## If your task needs a frozen change

Stop before editing and say so: which file and function, and why. If you're working on a contribution from outside
the project, open an issue that describes the change instead of a pull request; a pull request that changes frozen
code without the master plan won't be merged.

Code that only reads from the vehicle path, and the parts of a partly frozen file that aren't marked, follow the
usual rules in [CONTRIBUTING.md](CONTRIBUTING.md).
