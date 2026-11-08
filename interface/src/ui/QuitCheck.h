//
//  QuitCheck.h
//  interface/src/ui
//
//  The unsaved-work check before a quit the person asked for (Caity 11-08, "yes quit warning", 1.1.4): File > Quit
//  and Cmd+Q, the Dock's Quit (or another app asking), the main window's close button. First, terrain sculpting still
//  waiting to be saved goes up now and the quit waits for it, as an autosave would (a notice shows if it takes a
//  moment). Then, when Code holds typing that isn't saved (the open script, the drafts of scripts the person moved
//  away from, in any world, a new script not saved yet) or the terrain couldn't be saved, one of Darkride's popups
//  asks, in mary's words (11-08): Cancel (on the left, the default) keeps Darkride open; Discard Changes (red) quits.
//  Never on a macOS shutdown, restart or log out, nor at the end of a session elsewhere, nor for a quit the app or a
//  script asks for itself (the test harness, a TERM): those quit as before.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_QuitCheck_h
#define hifi_QuitCheck_h

#include <QtCore/QtGlobal>

namespace quitcheck {

// At the start, after the main window: its close button asks first, and the end of a session (Windows, Linux) is
// noted so that its quit never asks.
void install();

// Before a quit the person asked for: true when it may go ahead (nothing unsaved, or Discard Changes). One quit asks
// once: after a yes, every later check says yes at once (Qt closes the windows after the app's Quit event, and each
// close comes here). A second quit while the first is still asking says no (the first one's answer stands).
bool mayQuit();

// File > Quit and Cmd+Q: the check, then the quit.
void askThenQuit();

// The app's Quit event (Application::event): true when it is a person's request from outside the app (the Dock's
// Quit, another app asking it to quit), which the check sees before Qt closes any window. False for the system ending
// the session (shutdown, restart, log out) and for a quit the app asks for itself.
bool isOutsideRequest();

// The quit goes ahead (the app's Quit event, passed): the windows Qt closes next don't ask.
void goingAhead();

#if defined(Q_OS_MACOS)
namespace mac {
// The Apple event being handled now is a quit (kAEQuitApplication): fromSystem says whether it carries the system's
// reason (log out, restart, shut down, or their dialogs). QuitCheckMac.mm.
bool isQuitEvent(bool& fromSystem);
// Darkride to the front, as a Mac app asked to quit from the Dock does before it asks about unsaved work
void bringToFront();
}
#endif

}

#endif  // hifi_QuitCheck_h
