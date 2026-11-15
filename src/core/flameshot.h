// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#pragma once

#include "core/capturerequest.h"
#include "widgets/capture/capturewidget.h"

#include <QObject>
#include <QPointer>
#include <QVersionNumber>
#include <QWindow>

class ConfigWindow;
class InfoWindow;
class CaptureLauncher;
class QWidget;
#ifdef ENABLE_IMGUR
class UploadHistory;
#endif
#if (defined(Q_OS_MACOS) || defined(Q_OS_WIN))
class QHotkey;
#endif

enum ErrCode : uint8_t
{
    E_OK = 0,
    E_GENERAL,
    E_ABORTED,
    E_DBUSCONN,
    E_SIG_BASE = 128,
    E_SIGINT = E_SIG_BASE + 2,
    E_SIGTERM = E_SIG_BASE + 15,
};

class Flameshot : public QObject
{
    Q_OBJECT

public:
    enum Origin
    {
        CLI,
        DAEMON
    };

    static Flameshot* instance();

public slots:
    CaptureWidget* gui(
      const CaptureRequest& req = CaptureRequest::GRAPHICAL_MODE);
    void screen(CaptureRequest req, int const screenNumber = -1);
    void full(const CaptureRequest& req);
    void launcher();
    void config();

    void info();

#ifdef ENABLE_IMGUR
    void history();
#endif

    void openSavePath();

    QVersionNumber getVersion();

public:
    static void setOrigin(Origin origin);
    static Origin origin();
    // Windows this process owns that must outlive a capture: the AI prompt and
    // the AI chat windows. Counted, not a flag, because several can be open at
    // once and closing one must not declare the others gone.
    void addExternalWindow();
    void removeExternalWindow();
    int externalWindowCount() const;
    bool haveExternalWidget() const;
    // Whether finishing a capture means this process has nothing left to show.
    // `flameshot gui` exits on it; keeping it a named predicate makes the rule
    // testable without running main() (see tests/ai).
    bool shouldExitAfterCapture() const;

signals:
    void captureTaken(QPixmap p);
    void captureFailed();

public slots:
    void requestCapture(const CaptureRequest& request);
    void exportCapture(const QPixmap& p,
                       QRect& selection,
                       const CaptureRequest& req);

private:
    Flameshot();
    bool resolveAnyConfigErrors();
    void runAIBackend(const QPixmap& capture);

    // class members
    static Origin m_origin;
    int m_externalWidgets;

    QPointer<CaptureWidget> m_captureWindow;
    QPointer<InfoWindow> m_infoWindow;
    QPointer<CaptureLauncher> m_launcherWindow;
    QPointer<ConfigWindow> m_configWindow;

#if defined(Q_OS_MACOS)
public:
    void showDockIcon(QWidget* window);

private:
    void onWindowVisibilityChanged(QWindow::Visibility newVisibility);
    int m_dockIconVisibleCount = 0;
#endif

#if (defined(Q_OS_MACOS) || defined(Q_OS_WIN))
    QHotkey* m_HotkeyScreenshotCapture;
#endif
#if (defined(Q_OS_MACOS) && ENABLE_IMGUR)
    QHotkey* m_HotkeyScreenshotHistory;
#endif
};
