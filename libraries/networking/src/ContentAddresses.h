//
//  ContentAddresses.h
//  libraries/networking/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//
//  Whether a content address (a model, a picture, a sound, a page, ...) names a file on this computer. The rule is
//  ResourceManager::normalizeURL's own reading, kept here so every reader (the entity readers through
//  EntityContentAddresses, a received avatar identity in AvatarData) applies the one rule. Pure string and QUrl
//  work: no globals, no network, safe in every process.
//

#pragma once

#include <QtCore/QString>

namespace ContentAddresses {

    // Whether the resource manager would read this address from a file on this computer. ResourceManager::normalizeURL
    // turns every address without a known scheme into a file: one, so this is true for "file:" in any case, a bare
    // path ("/Users/...", "\\host\...", "//host/...", a relative name), a drive letter ("C:\...", "C:/...", which
    // parse as a one-letter scheme) and any other unknown scheme. "/~/" names the app's own resources and is allowed
    // unless it climbs out of them with "..". Empty, http, https, ftp, atp, tea and qrc addresses are not.
    bool isOnThisComputer(const QString& address);

    // The narrower form for addresses the resource manager doesn't fetch (a Web entity's page and injected script, a
    // Text entity's font): "file:" in any case, a drive letter, a bare absolute or "\\host" path, or "/~/" climbing
    // out. A relative name is a font family, or a page beside the app's own, never a file read.
    bool isPathOnThisComputer(const QString& address);

}
