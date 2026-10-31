//
//  RadioSkinPicker.h
//  interface/src/ui
//
//  The Radio panel's "Load a skin…" (qml/darkride/RadioPanel.qml, its native controls under the player): the app's own
//  file dialog, and the classic skin the person picks in it handed to the radio's page as bytes (base64), size-capped.
//  The page can't open a file chooser itself unless clicked inside it, and can't read the computer's files; this takes
//  no path from anyone, so it reads only a file the person chose in its own dialog. Only the app's own panels see it
//  (the desktop's QML context, ApplicationScripting.cpp), never world scripts. Main thread.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_RadioSkinPicker_h
#define hifi_RadioSkinPicker_h

#include <QtCore/QObject>
#include <QtCore/QPointer>

class QFileDialog;

class RadioSkinPicker : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    // opens the dialog (or brings an open one to the front); its answer comes as picked()
    Q_INVOKABLE void pick();

signals:
    // a skin chosen: its file name and its bytes in base64; "" and "" for a Cancel; problem set (and no bytes) when the
    // file is too big or can't be read
    void picked(const QString& name, const QString& base64, const QString& problem);

private:
    QPointer<QFileDialog> _dialog;
};

#endif // hifi_RadioSkinPicker_h
