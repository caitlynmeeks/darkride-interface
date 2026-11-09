//
//  MainWindow.h
//  interface
//
//  Created by Mohammed Nafees on 04/06/2014.
//  Copyright (c) 2014 High Fidelity, Inc. All rights reserved.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef __hifi__MainWindow__
#define __hifi__MainWindow__

#include <functional>

#include <QMainWindow>

#include <SettingHandle.h>

class DockWidget;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = NULL);
    ~MainWindow();

    static QWindow* findMainWindow();

    // This offset is used for positioning children window relative to the main window.
    void setDockedWidgetRelativePositionOffset(const QSize& newOffset) { _dockedWidgetRelativePositionOffset.setWidth(newOffset.width()); _dockedWidgetRelativePositionOffset.setHeight(newOffset.height()); }
    QSize getDockedWidgetRelativePositionOffset() { return _dockedWidgetRelativePositionOffset; }
    // asked before closing the window quits the app (Darkride's unsaved-work check, interface/src/ui/QuitCheck.h):
    // false keeps the window and the app open
    void setCloseCheck(std::function<bool()> check) { _closeCheck = std::move(check); }
public slots:
    // where it was left at the last quit, if that still fits a connected screen and isn't tiny; else the whole of the
    // main screen, as at a first start
    void restoreGeometry();
    void saveGeometry();
    // soon after the start: logs where the window is, and puts it back if something shrank it below the smallest
    // size a start allows before anyone touched it
    void checkStartGeometry();

signals:
    void windowGeometryChanged(QRect geometry);
    void windowShown(bool shown);
    void windowMinimizedChanged(bool minimized);

protected:
    virtual void closeEvent(QCloseEvent* event) override;
    virtual void moveEvent(QMoveEvent* event) override;
    virtual void resizeEvent(QResizeEvent* event) override;
    virtual void showEvent(QShowEvent* event) override;
    virtual void hideEvent(QHideEvent* event) override;
    virtual void changeEvent(QEvent* event) override;
    virtual void dragEnterEvent(QDragEnterEvent *e) override;
    virtual void dropEvent(QDropEvent *e) override;

private:
    // a normal window (not maximised or full screen) whose inner area or title bar is outside the usable part of its
    // screen is moved in, and made smaller only where it can't fit
    void fitOnScreen(const char* when);

    Setting::Handle<QRect> _windowGeometry;
    Setting::Handle<int> _windowState;
    QRect _startGeometry;
    QSize _dockedWidgetRelativePositionOffset{ 0, 0 };
    std::function<bool()> _closeCheck;
};

#endif /* defined(__hifi__MainWindow__) */
