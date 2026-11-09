//
//  MetaverseRequests.h
//  interface/src/ui
//
//  Calls to the metaverse's API for Darkride's own QML panels (ARCHITECTURE.md §5.2.1): what the web pages did with an
//  HTTP request that the browser's request filter signed with the account's token. A panel names a path under /api/;
//  the request goes to the metaverse this client signs in to, signed when an account is signed in. QML panels only:
//  scripts never see the object, so no script can send a request in the account's name or read its token.
//
//  One object per QML engine (each offscreen surface has its own engine): the answer is made in the engine whose
//  panel asked, as a callback can't take a value made in another engine (Qt 6 refuses the call; the panel never hears).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_MetaverseRequests_h
#define hifi_MetaverseRequests_h

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtQml/QJSEngine>
#include <QtQml/QJSValue>

class MetaverseRequests : public QObject {
    Q_OBJECT

public:
    /// engine: the QML engine of the panels that see this object (their context's engine); parent: that context
    MetaverseRequests(QJSEngine* engine, QObject* parent);

    /// method "GET", "POST", "PUT" or "DELETE"; path under "/api/"; body sent as JSON with a POST or a PUT;
    /// callback(error, status, data):
    /// error null on a 2xx answer, otherwise a short description; data the answer parsed as JSON (null if it is not),
    /// its lists real JavaScript arrays (Array.isArray is true on Qt 5 and Qt 6)
    Q_INVOKABLE void send(const QString& method, const QString& path, const QVariant& body, QJSValue callback);

    /// the metaverse's address, for links a panel shows or opens (a user's settings page, a nametag picture)
    Q_INVOKABLE QString metaverseUrl() const;

    /// the rights line (ui/RightsLine.h), shown under a panel's buttons that put something where others see it, and
    /// the Terms paragraph its "Terms" link opens
    Q_INVOKABLE QString rightsLine() const;
    Q_INVOKABLE QString termsUrl() const;

private:
    QPointer<QJSEngine> _engine;
};

#endif // hifi_MetaverseRequests_h
