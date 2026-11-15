// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QFileInfo>
#include <QMimeData>
#include <QString>
#include <QUrl>

// The clipboard flavours that make a local file *pasteable* rather than merely
// named.
//
// A file target does not agree on one flavour: GTK/Nautilus/Cinnamon read
// `x-special/gnome-copied-files` to tell a copy from a cut, Dolphin and KDE
// applications read the `application/x-kde-cutselection` flag, everything that
// speaks the XDG clipboard reads `text/uri-list`, and a terminal or a text field
// can only use the plain path. One QMimeData carrying all four is what makes
// "copy the recording" behave the same in every target.
//
// Header-only and Qt-only on purpose: the capture overlay and the clipboard
// daemon both have to build the *same* data, and the daemon is compiled with and
// without the recording feature, so this must not depend on anything else.
namespace FileMimeData {

// Fresh mime data for `path`, or nullptr when it is not a non-empty regular
// file (a directory or a vanished path must never be offered as a paste source).
inline QMimeData* forLocalFile(const QString& path)
{
    if (path.isEmpty()) {
        return nullptr;
    }
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile() || file.size() <= 0) {
        return nullptr;
    }
    const QString absolute = file.absoluteFilePath();
    const QString uri = QUrl::fromLocalFile(absolute).toString();

    auto* data = new QMimeData;
    data->setUrls({ QUrl::fromLocalFile(absolute) });
    data->setData(QStringLiteral("x-special/gnome-copied-files"),
                  QStringLiteral("copy\n%1").arg(uri).toUtf8());
    // KDE's convention is the cut flag, and "0" means copy, not move.
    data->setData(QStringLiteral("application/x-kde-cutselection"),
                  QByteArrayLiteral("0"));
    data->setText(absolute);
    return data;
}

} // namespace FileMimeData
