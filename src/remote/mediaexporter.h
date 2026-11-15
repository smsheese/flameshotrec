// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "remote/remotestorage.h"

#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTemporaryDir;

namespace RemoteStorage {

// What a conversion produced. `file` points at a temporary file that the caller
// owns and must delete; the original saved capture is never modified.
struct ExportedMedia
{
    QString file;
    QString remoteName;
    QString mimeType;
    bool converted = false;
};

// Format facts, queried from the installed Qt image plugins rather than
// hardcoded, so the widget only offers writers that can actually run.
struct ImageFormatInfo
{
    QString name; // QImageWriter::supportedImageFormats() token, e.g. "png"
    QString suffix;
    QString mimeType;
    bool supportsQuality = false;
    bool supportsCompression = false;
};

QList<ImageFormatInfo> imageFormats();
ImageFormatInfo imageFormat(const QString& name);

QString mimeTypeForFile(const QString& fileName);

// Image re-encode through QImageWriter. Synchronous: a screenshot is small and
// the writer has no progress to report.
bool exportImage(const QString& source,
                 const ImageProfile& profile,
                 QTemporaryDir* workspace,
                 ExportedMedia* exported,
                 QString* error);

// Sanitised remote file name: base name only, no path separators or control
// characters. The extension is replaced when the conversion changed the format.
QString remoteFileName(const QString& sourceFile,
                       const QString& convertedSuffix = {});

} // namespace RemoteStorage
