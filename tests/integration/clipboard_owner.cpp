// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors
//
// The clipboard daemon, as its own process, for the two-process clipboard check
// (tests/integration/clipboard_handoff.sh). It is the same object the application
// runs: FlameshotDaemon owns the X11 selection so a copied file URI outlives the
// process that copied it.
//
// No capture, no overlay, no audio: it only registers the D-Bus service and runs
// the event loop.

#include "core/flameshotdaemon.h"
#include "core/flameshotdbusadapter.h"
#include "utils/confighandler.h"

#include <QApplication>
#include <QDBusConnection>
#include <QObject>

#include <cstdio>
#include <cstring>

namespace {
void writeLine(const char* key, const QByteArray& value = QByteArray())
{
    std::fwrite(key, 1, std::strlen(key), stdout);
    if (!value.isEmpty()) {
        std::fwrite(value.constData(), 1, size_t(value.size()), stdout);
    }
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
void fail(const char* message)
{
    std::fwrite(message, 1, std::strlen(message), stderr);
    std::fputc('\n', stderr);
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("flameshot"));

    // Touch the configuration first: the daemon's constructor and the tray icon
    // read it, and the throw-away XDG_CONFIG_HOME in the harness is empty.
    ConfigHandler();
    FlameshotDaemon::start();

    // Export the interface the capture process calls. Without this the D-Bus call
    // would have no object to reach and the hand-off would silently do nothing.
    auto* busObject = new QObject(&app);
    new FlameshotDBusAdapter(busObject);
    QDBusConnection dbus = QDBusConnection::sessionBus();
    if (!dbus.isConnected()) {
        fail("owner: no session bus");
        return 2;
    }
    if (!dbus.registerObject(QStringLiteral("/"), busObject)) {
        fail("owner: could not register the object");
        return 3;
    }
    if (!dbus.registerService(QStringLiteral("org.flameshot.Flameshot"))) {
        fail("owner: could not own org.flameshot.Flameshot");
        return 4;
    }

    writeLine("OWNER_READY");
    return app.exec();
}
