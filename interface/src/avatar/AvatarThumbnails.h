//
//  AvatarThumbnails.h
//  interface/src/avatar
//
//  An avatar's thumbnail (Caity, 10-24: "a little picture of each one ... like a snap taken from a camera or an image
//  you can upload"): the picture the panels show for an avatar in My Files, kept beside its .fst as
//  <name>.preview.jpg, the file an .fst's standard `preview` line names. A picture is taken with the secondary camera
//  (the spectator camera's), placed in front of you for one frame and then put back as it was; or made from an image
//  on this computer. Either is cropped to 4:3, 640×480, a JPEG. The Avatar panel uploads one beside the avatar worn
//  when that avatar is in your own My Files; the avatar importer takes one when it puts an avatar there. And each
//  avatar worn gets one by itself, once (Caity, 11-08), kept on this computer only, in the app's data folder and
//  per account, for the panel's Recent, favourites and web links (autoPictures).
//  QML panels only, as MetaverseRequests: scripts never see the object.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_AvatarThumbnails_h
#define hifi_AvatarThumbnails_h

#include <functional>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtGui/QImage>
#include <QtQml/QJSValue>

#include <DependencyManager.h>

class QTimer;

class AvatarThumbnails : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

public:
    using Done = std::function<void(const QByteArray& jpeg, const QString& error)>;

    static const int WIDTH { 640 };
    static const int HEIGHT { 480 };

    /// a picture of you from the front, chest up, once the avatar worn has loaded (at most 20 s), as a JPEG
    void capture(Done done);

    /// the same picture for a panel: callback(error, url), url a file:// address of the JPEG kept here
    Q_INVOKABLE void take(QJSValue callback);
    /// a picture made from the image at path (a file:// address or a local path): callback(error, url)
    Q_INVOKABLE void fromFile(const QString& path, QJSValue callback);
    /// { folder, name } of an avatar's .fst in the signed-in user's My Files ("/avatars/meehae", "meehae"), or {}
    Q_INVOKABLE QVariantMap myFilesPlace(const QString& fstUrl) const;
    /// the path in My Files an avatar's picture has ("/avatars/meehae/meehae.preview.jpg"), or "" when it isn't yours
    Q_INVOKABLE QString pictureKey(const QString& fstUrl) const;
    /// uploads the picture at pictureUrl as <name>.preview.jpg beside the .fst: callback(error, url)
    Q_INVOKABLE void upload(const QString& pictureUrl, const QString& fstUrl, QJSValue callback);

    /// the picture's file name for an .fst named name, as the uploads store it ("Auxi Day" → "Auxi_Day.preview.jpg")
    static QString pictureName(const QString& name);
    /// an image cropped to 4:3 and scaled to 640×480, as a JPEG
    static QByteArray fit(const QImage& image);

    /// the pictures taken by themselves of the avatars worn: { the avatar's link as worn: "file:///…/<n>.jpg?<taken>" },
    /// kept on this computer only and never uploaded (autoPictureFor below)
    Q_INVOKABLE QVariantMap autoPictures() const;

signals:
    /// a picture of an avatar worn was taken by itself and kept: autoPictures() has it
    void autoPicturesChanged();

private:
    AvatarThumbnails();

    // frames: the rendered frames to wait with the camera on before the picture, or 0 for CAMERA_SETTLE_MS
    void shoot(Done done, int frames = 0);
    QString keep(const QByteArray& jpeg, QString& error) const;

    // An automatic picture (Caity 11-08, "every avatar I wear gets its picture"): once an avatar worn has loaded and
    // there is none of it yet, taken as Take Its Picture takes one but over a few frames, and only once you've
    // settled with frame time to spare: not seated or riding, standing still, the spectator camera not in use, the
    // window shown, not in a headset, physics on for 20 s, the refresh governor at full rate and past its arrival
    // grace. Tried every few seconds for two minutes, then left until the avatar is worn again.
    void autoPictureFor(const QString& url);
    void tryAutoPicture();
    void keepAutoPicture(const QString& url, const QByteArray& jpeg);
    QString autoFolder() const;
    const QVariantMap& autoIndex() const;   // { link: { file, taken } } of the signed-in account, read once each

    bool _taking { false };
    QString _autoUrl;                       // the avatar whose picture is waited for, "" when none is
    int _autoTries { 0 };
    qint64 _physicsOnAt { 0 };              // when physics was first seen on in this stay (ms), 0 while it's off
    QTimer* _autoTimer { nullptr };
    mutable QVariantMap _autoIndex;
    mutable QString _autoIndexFolder;       // the account's folder _autoIndex was read from
};

#endif // hifi_AvatarThumbnails_h
