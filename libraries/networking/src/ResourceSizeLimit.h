//
//  ResourceSizeLimit.h
//  libraries/networking/src
//
//  Created by servers on 2026-11-02.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ResourceSizeLimit_h
#define hifi_ResourceSizeLimit_h

#include <QtCore/QString>
#include <QtCore/QStringList>

// The most a download from a web server may be, by the kind of file its address names: past it the download stops
// and the request fails, and the same number caps what a gzip or brotli download may unpack to. The numbers leave
// room above the largest real files in Darkride's worlds (measured 2026-11-02: models 22 MB, textures 18 MB, sounds
// 19 MB, scripts 30 KB) for avatars and imported worlds, and stay well under what the app can hold in memory.
namespace ResourceSizeLimit {

static const int MIB = 1024 * 1024;
static const int MODEL = 128 * MIB;     // fbx, glb, gltf, obj
static const int TEXTURE = 64 * MIB;    // png, jpg, ktx, webp, hdr and the like
static const int SOUND = 96 * MIB;      // wav, mp3, ogg, flac: a few minutes of uncompressed music
static const int SCRIPT = 16 * MIB;     // js
static const int OTHER = 128 * MIB;     // world files, JSON, and anything whose ending names no kind

// path: the address's path, without its query; its ending (case ignored) decides
inline int forPath(const QString& path) {
    const int slash = path.lastIndexOf('/');
    const int dot = path.lastIndexOf('.');
    if (dot < 0 || dot < slash) {
        return OTHER;
    }
    const QString ending = path.mid(dot + 1).toLower();
    static const QStringList models { "fbx", "glb", "gltf", "obj" };
    static const QStringList textures { "png", "jpg", "jpeg", "jfif", "ktx", "ktx2", "webp", "hdr", "exr", "bmp", "tga", "gif" };
    static const QStringList sounds { "wav", "mp3", "ogg", "flac", "raw", "opus" };
    static const QStringList scripts { "js", "mjs" };
    if (models.contains(ending)) {
        return MODEL;
    }
    if (textures.contains(ending)) {
        return TEXTURE;
    }
    if (sounds.contains(ending)) {
        return SOUND;
    }
    if (scripts.contains(ending)) {
        return SCRIPT;
    }
    return OTHER;
}

}

#endif // hifi_ResourceSizeLimit_h
