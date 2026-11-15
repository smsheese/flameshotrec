// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors
//
// An independent process that reads the clipboard. It links nothing from the
// application, so the flavours it sees were transferred over X11 from another
// process' selection -- that is the property under test, not a QClipboard read in
// the process that set it.

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QUrl>

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
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    if (data == nullptr) {
        writeLine("READER_HASURLS ", QByteArrayLiteral("0"));
        return 1;
    }
    writeLine("READER_HASURLS ", QByteArray::number(data->hasUrls() ? 1 : 0));
    writeLine("READER_URI ", data->urls().value(0).toString().toUtf8());
    writeLine("READER_GNOME ",
         data->data(QStringLiteral("x-special/gnome-copied-files")).toBase64());
    writeLine("READER_KDE ",
         data->data(QStringLiteral("application/x-kde-cutselection")).toBase64());
    writeLine("READER_TEXT ", data->text().toUtf8());
    return data->hasUrls() ? 0 : 1;
}
