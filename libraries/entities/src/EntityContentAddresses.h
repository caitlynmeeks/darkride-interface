//
//  EntityContentAddresses.h
//  libraries/entities/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//
//  The entity properties that name content to fetch (a model, a picture, a sound, a material, a page, ...) and the
//  check for an address that names a file on this computer. A world's entities arrive from its server; one whose
//  model is "file:///Users/me/car.glb" was built on its maker's computer and, on a visitor's, reads a file there. When
//  the world isn't local those addresses are emptied as the entity arrives or as a world's script sets them; a copy,
//  paste or export of such an entity carries the empty value. A world played from this computer (a local server, a
//  world file here) keeps them.
//

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

class EntityItemProperties;

namespace EntityContentAddresses {

    // Whether the resource manager would read this address from a file on this computer. ResourceManager::normalizeURL
    // turns every address without a known network scheme into a file: one, so this is true for "file:" in any case,
    // a bare path ("/Users/...", "\\host\...", "//host/...", a relative name), a drive letter ("C:\...", "C:/...",
    // which parse as a one-letter scheme) and any other unknown scheme. "/~/" names the app's own resources and is
    // allowed unless it climbs out of them with "..". Empty, http, https, ftp, atp, tea and qrc addresses are not.
    bool isOnThisComputer(const QString& address);

    // The narrower form for addresses the resource manager doesn't fetch (a Web entity's page and injected script,
    // a Text entity's font): "file:" in any case, a drive letter, a bare absolute or "\\host" path, or "/~/" climbing
    // out. A relative name is a font family, or a page beside the app's own, never a file read.
    bool isPathOnThisComputer(const QString& address);

    // Empties every content address in properties that names this computer and returns the script names of the
    // properties emptied ("modelURL", "animation.url", ...). Only a property the object carries as changed is looked at
    // (a world script's partial edit, { name: "x" }, comes back untouched, no unchanged property marked changed); for
    // an add every set value counts as changed. The addresses inside textures, materialData and userData's
    // "impostor" entry and procedural shader data are checked one by one: a textures or materialData entry is
    // emptied on its own ("textures", "materialData" are reported), a textures value no renderer can read is emptied
    // unless it is a fetched address, the impostor entry is removed whole ("userData.impostor"; a card can't draw
    // without its atlases, so the model draws as if never baked), a shader or channel address under userData's
    // "ProceduralEntity" or at its top level is emptied ("userData.procedural"). href is emptied only when it is a
    // "file:" address (a click on it would open a world file from this computer); its viewpoint form "/x,y,z" stays.
    // materialURL's
    // value "materialData" (use the inline material) stays. script and serverScripts are the script shield's, not here.
    QStringList clearThisComputer(EntityItemProperties& properties);

    // The textures property read as the Model renderer reads it (parseTexturesToMap): JSON, or the legacy list of
    // name:"address"; false when the renderer would ignore the value.
    bool readTexturesMap(const QString& textures, QJsonObject& map);

    // The content-address properties by their script names, one address each (the three with several addresses inside
    // them, textures, materialData and userData's impostor entry, are not in this list).
    const QStringList& propertyNames();

}
