//
//  MakeFiles.h
//  libraries/make-link/src
//
//  Make's files in the person's My Files (Caity, 11-08: the default; the world's files stay a choice): the folder a
//  world's files go in, the test that an upload's answer is a link of the metaverse's own files host, and whether a
//  thing still points at a file (Undo deletes a file of an undone request only when none does).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_MakeFiles_h
#define hifi_MakeFiles_h

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

class MakeFiles {
public:
    static const int FOLDER_NAME_MAX;   // 40

    /// A world's folder under /Make: the name's ASCII letters and digits (accents dropped), every other run one "-",
    /// at most FOLDER_NAME_MAX; "world" when nothing is left. Never a "/" or a ".", so it can't leave /Make.
    static QString folderName(const QString& worldName);

    /// Whether `link` is a link the metaverse's files host gives for a file: https on files.<metaverse host> (the
    /// default port), under /f/, with no user part and no step up a folder.
    static bool isOwnFilesLink(const QUrl& link, const QUrl& metaverse);

    /// Whether any of these field values (a thing's script, server scripts, model, collision model or material)
    /// points at `link`. A value holding it among other text counts too, and an empty link counts as used: a doubtful
    /// case keeps the file.
    static bool usedBy(const QString& link, const QStringList& values);
};

#endif  // hifi_MakeFiles_h
