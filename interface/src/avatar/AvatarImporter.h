//
//  AvatarImporter.h
//  interface/src/avatar
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AvatarImporter_h
#define hifi_AvatarImporter_h

#include <QString>
#include <QUrl>

// File > Import Avatar. A .unitypackage (an avatar, as avatar shops sell them) is converted by libraries/unity-import.
// An .fst is worn as it is. A character file (FBX, DAE, OBJ, glTF and the rest the client reads)
// gets the standard skeleton from libraries/auto-rigging, after asking when the file already has a rig, and an
// offer to fix a height that looks like a units mistake. The result is saved under the app data folder's
// Avatars/<name>/, and the client offers to wear it, here only or put in My Files.
//
// The Avatar panel's Wear from Link… with a character file's link (a .glb or .vrm): downloaded into the app data
// folder's Avatars/Downloads/, then imported as File > Import Avatar imports it.
//
// File > Put Avatar in My Files. An avatar on this computer is seen only by this client (others get a blank address
// and see the default). This uploads the worn one to the user's files on the metaverse, in a private folder (signed-in
// Darkride apps load it; it has no public link), and wears that copy. A .vrm model goes up named .glb, which every
// version reads.
class AvatarImporter {
public:
    static void importFile(const QString& path);
    static void importLink(const QUrl& url);
    static void putInMyFiles(const QUrl& fstUrl);
};

#endif // hifi_AvatarImporter_h
