//
//  UnityAvatar.h
//  libraries/unity-import/src
//
//  Created on 2026-10-14 for Darkride's import of avatars from Unity packages.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_UnityAvatar_h
#define hifi_UnityAvatar_h

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace unity {
class Package;
}

namespace avatarimport {

// An avatar a package holds: a prefab with an avatar descriptor, and the model its body is built on.
struct AvatarSource {
    QString name;         // the prefab's name
    QString prefabGuid;
    QString prefabPath;
    QString modelGuid;    // the FBX the descriptor's object comes from
    QString modelPath;
    QVariantMap descriptor;  // the VRCAvatarDescriptor's fields
};

// The .fst's jointMap lines ("jointMap = <standard name> = <the model's joint>") from Unity's humanoid map, the
// humanDescription of the model's .meta; skeletonNames gets each mapped joint's standard name. A joint whose name an
// .fst line cannot hold (FSTReader::isLineValue) is left out, with a note.
QStringList jointMapLines(const QVariantMap& humanDescription, QHash<QString, QString>& skeletonNames, QStringList& notes);

// The avatars in a package, PC ones before those for phones and Quest.
QVector<AvatarSource> findAvatars(const unity::Package& package);

struct WrittenAvatar {
    bool ok { false };
    QString error;
    QString fstPath;
    QString modelPath;
    QStringList notes;  // what was carried over, what was not yet, and anything to look at
};

// Writes an avatar as a Darkride avatar into a folder: the model file unchanged, the textures its materials use, and
// <name>.fst, which names the humanoid joints from Unity's humanoid map (jointMap), asks for humanoid retargeting,
// maps each material to a stand-in for its shader (lilToon and the rest drawn with the engine's own material for
// now), hides what the effects layer starts hidden, and gives the mouth and the blink the avatar's own shape keys.
WrittenAvatar writeAvatar(const unity::Package& package, const AvatarSource& avatar, const QString& folder,
                          const QString& name);

}

#endif // hifi_UnityAvatar_h
