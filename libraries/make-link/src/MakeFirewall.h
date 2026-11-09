//
//  MakeFirewall.h
//  libraries/make-link/src
//
//  Make's Blender box on Windows and the installer's firewall rules. The box (an AppContainer profile with no
//  capabilities) can still reach this computer's own network address, where services listen, so on Windows Blender is
//  offered only where the installer's two block rules for the box's package SID are in place and exactly as the
//  installer writes them (release, windows-installer-1), and no other rule names the box. This only reads the
//  firewall's settings: no admin, nothing changed. The rules alone aren't trusted: the box's self-test still runs after
//  this read, because Group Policy can make Windows ignore local rules while they still read as present and enabled
//  (security, 11-07).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakeFirewall_h
#define hifi_MakeFirewall_h

#include <QtCore/QString>

#ifdef Q_OS_WIN

namespace MakeFirewall {

/// The box's AppContainer profile name, fixed: the installer's rules name the package SID Windows derives from it.
extern const wchar_t* const PROFILE_NAME;
/// That SID, as the installer writes it into both rules.
extern const wchar_t* const RULE_SID;

/// Whether the installer's two rules (outbound and inbound) are each there once and exactly right, no other rule names
/// the box's SID, the SID derived from PROFILE_NAME is RULE_SID, and the firewall is on for the current network
/// profiles. False with `why` otherwise: for the log only, never shown to the builder. A read made in the last few
/// seconds is reused (the dock asks again at each change of state).
bool boxRulesInPlace(QString& why);

}  // namespace MakeFirewall

#endif  // Q_OS_WIN

#endif  // hifi_MakeFirewall_h
