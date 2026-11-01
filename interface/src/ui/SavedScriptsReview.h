//
//  SavedScriptsReview.h
//  interface/src/ui
//
//  The one review of the saved scripts that start with Darkride (security #30): ScriptEngines holds the ones from the
//  network (savedScriptsAwaitingReview); this shows the app's dialog (qml/dialogs/SavedScriptsReview.qml) and hands the
//  visitor's answer back (ScriptEngines::finishSavedScriptsReview, which no script can reach). Main thread.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_SavedScriptsReview_h
#define hifi_SavedScriptsReview_h

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

class SavedScriptsReview : public QObject {
    Q_OBJECT
public:
    // after the saved scripts load: the dialog, when some are held for the review (once the app's windows are up)
    static void showPending();
    // the dialog for these held scripts
    static void show(const QStringList& scripts);

private slots:
    // the dialog's Done: the addresses kept (the dialog's answered signal)
    void answered(const QVariant& kept);

private:
    using QObject::QObject;
};

#endif // hifi_SavedScriptsReview_h
