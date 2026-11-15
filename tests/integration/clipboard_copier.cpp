// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors
//
// The process that copies a recording, for the two-process clipboard check.
// It is the capture process' side of "Copy": put the file URI on the clipboard
// and hand it to the clipboard daemon, then exit -- the daemon has to keep it.
//
// `--stay` keeps it running instead, which is the no-daemon fallback: without a
// session bus this process owns the selection, so it must not exit and must not
// lose the data.

#include "widgets/capture/recordingclipboard.h"
#include "widgets/capture/recordingclipboarddaemon.h"

#include <QApplication>

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
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (argc < 2) {
        std::fputs("usage: clipboard_copier <file> [--stay]\n", stderr);
        return 2;
    }
    const QString path = QString::fromLocal8Bit(argv[1]);
    const bool stay = argc > 2 && QString::fromLocal8Bit(argv[2]) == "--stay";

    // The shipped wiring, not a test-only variant.
    Recording::Clipboard::installDaemonHandOff();
    const bool offered = Recording::Clipboard::copyFileUri(path);
    writeLine("COPIER_OFFERED ", QByteArray::number(offered ? 1 : 0));
    writeLine("COPIER_FILE ", Recording::Clipboard::offeredFile().toUtf8());
    if (!stay) {
        return offered ? 0 : 1;
    }
    return app.exec();
}
