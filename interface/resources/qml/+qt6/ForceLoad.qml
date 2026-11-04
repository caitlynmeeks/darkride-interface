// Qt 6 twin of ForceLoad.qml (the qt6 file selector): the modules the client warms up at start. Qt 6's QtMultimedia
// has no Audio type; a MediaPlayer with an AudioOutput loads the same module.
import QtQuick
import QtMultimedia

Item {
    MediaPlayer {
        id: audio
        audioOutput: AudioOutput { }
    }
}
