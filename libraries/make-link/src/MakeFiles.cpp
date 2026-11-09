//
//  MakeFiles.cpp
//  libraries/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "MakeFiles.h"

const int MakeFiles::FOLDER_NAME_MAX = 40;

QString MakeFiles::folderName(const QString& worldName) {
    const QString decomposed = worldName.normalized(QString::NormalizationForm_KD);
    QString out;
    bool dash = false;
    for (const QChar c : decomposed) {
        const QChar::Category category = c.category();
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining ||
            category == QChar::Mark_Enclosing) {
            continue;   // the accent of a letter: "á" is "a" and its mark
        }
        if (c.unicode() < 128 && c.isLetterOrNumber()) {
            out.append(c);
            dash = false;
        } else if (!dash) {
            out.append('-');
            dash = true;
        }
    }
    auto trim = [](QString& text) {
        while (text.startsWith('-')) {
            text.remove(0, 1);
        }
        while (text.endsWith('-')) {
            text.chop(1);
        }
    };
    trim(out);
    if (out.size() > FOLDER_NAME_MAX) {
        out.truncate(FOLDER_NAME_MAX);
        trim(out);
    }
    return out.isEmpty() ? QStringLiteral("world") : out;
}

bool MakeFiles::isOwnFilesLink(const QUrl& link, const QUrl& metaverse) {
    if (!link.isValid() || link.scheme() != "https" || !link.userInfo().isEmpty() || metaverse.host().isEmpty()) {
        return false;
    }
    if (link.port(443) != 443 || link.host().compare("files." + metaverse.host(), Qt::CaseInsensitive) != 0) {
        return false;
    }
    const QString path = link.path(QUrl::FullyDecoded);
    return path.startsWith("/f/") && path.size() > 3 && !path.contains("/../") && !path.endsWith("/..") &&
           !path.contains('\\');
}

bool MakeFiles::usedBy(const QString& link, const QStringList& values) {
    if (link.isEmpty()) {
        return true;
    }
    for (const QString& value : values) {
        if (value.contains(link)) {
            return true;
        }
    }
    return false;
}
