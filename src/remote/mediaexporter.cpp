// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/mediaexporter.h"

#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <algorithm>

namespace RemoteStorage {
namespace {

constexpr int kMaxRemoteNameLength = 120;

QString preferredSuffix(const QString& format)
{
    const QString key = format.trimmed().toLower();
    if (key == QLatin1String("jpeg") || key == QLatin1String("jpg")) {
        return QStringLiteral("jpg");
    }
    if (key == QLatin1String("tiff") || key == QLatin1String("tif")) {
        return QStringLiteral("tif");
    }
    if (key.isEmpty()) {
        return QStringLiteral("png");
    }
    return key;
}

QString suffixForFormatToken(const QString& format)
{
    return preferredSuffix(format);
}

QString sanitizedBaseName(const QString& sourceFile)
{
    QString base = QFileInfo(sourceFile).completeBaseName();
    base.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")), QStringLiteral("_"));
    base.replace(QLatin1Char('/'), QLatin1Char('_'));
    base.replace(QLatin1Char('\\'), QLatin1Char('_'));
    base = base.trimmed();
    while (base.startsWith(QLatin1Char('.'))) {
        base.remove(0, 1);
    }
    if (base.isEmpty()) {
        base = QStringLiteral("capture");
    }
    return base;
}

bool writerSupports(const QString& format, QImageIOHandler::ImageOption option)
{
    // QImageWriter answers option queries from the plugin without opening the
    // device, so a throwaway file name is safe here.
    QImageWriter probe(QStringLiteral("probe.") + suffixForFormatToken(format), format.toLatin1());
    return probe.supportsOption(option);
}

} // namespace

QString mimeTypeForFile(const QString& fileName)
{
    const QMimeType type =
      QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
    return type.name();
}

QList<ImageFormatInfo> imageFormats()
{
    QList<ImageFormatInfo> formats;
    QStringList tokens;
    for (const QByteArray& token : QImageWriter::supportedImageFormats()) {
        tokens.append(QString::fromLatin1(token));
    }
    tokens.sort();
    for (const QString& token : tokens) {
        ImageFormatInfo info;
        info.name = token;
        info.suffix = suffixForFormatToken(token);
        info.mimeType = mimeTypeForFile(QStringLiteral("file.") + info.suffix);
        info.supportsQuality = writerSupports(token, QImageIOHandler::Quality);
        info.supportsCompression = writerSupports(token, QImageIOHandler::CompressionRatio);
        formats.append(info);
    }
    return formats;
}

ImageFormatInfo imageFormat(const QString& name)
{
    const QString key = name.trimmed().toLower();
    for (const ImageFormatInfo& info : imageFormats()) {
        if (info.name.compare(key, Qt::CaseInsensitive) == 0) {
            return info;
        }
    }
    return {};
}

QString remoteFileName(const QString& sourceFile, const QString& convertedSuffix)
{
    QString base = sanitizedBaseName(sourceFile);
    QString suffix = convertedSuffix.trimmed().toLower();
    if (suffix.isEmpty()) {
        suffix = QFileInfo(sourceFile).suffix().toLower();
    }
    while (suffix.startsWith(QLatin1Char('.'))) {
        suffix.remove(0, 1);
    }
    if (suffix.isEmpty()) {
        suffix = QStringLiteral("bin");
    }
    const int budget = kMaxRemoteNameLength - suffix.size() - 1;
    if (budget > 0 && base.size() > budget) {
        base = base.left(budget);
    }
    return base + QLatin1Char('.') + suffix;
}

bool exportImage(const QString& source,
                 const ImageProfile& profile,
                 QTemporaryDir* workspace,
                 ExportedMedia* exported,
                 QString* error)
{
    if (error) {
        error->clear();
    }
    if (exported == nullptr || workspace == nullptr) {
        if (error) {
            *error = QStringLiteral("Internal error: no image export destination.");
        }
        return false;
    }
    QString format = profile.format.trimmed().toLower();
    if (format.isEmpty()) {
        format = QFileInfo(source).suffix().trimmed().toLower();
    }
    if (format.isEmpty()) {
        if (error) {
            *error = QStringLiteral("The image has no recognizable format.");
        }
        return false;
    }
    QByteArray token = format.toLatin1();
    if (!QImageWriter::supportedImageFormats().contains(token) &&
        QImageWriter::supportedImageFormats().contains(preferredSuffix(format).toLatin1())) {
        token = preferredSuffix(format).toLatin1();
    }
    if (!QImageWriter::supportedImageFormats().contains(token)) {
        if (error) {
            *error = QStringLiteral("This Qt installation cannot write %1 images.")
                       .arg(format.toUpper());
        }
        return false;
    }

    QImageReader reader(source);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        if (error) {
            *error = QStringLiteral("The saved image could not be read: %1")
                       .arg(reader.errorString());
        }
        return false;
    }

    const QString suffix = suffixForFormatToken(QString::fromLatin1(token));
    const QString target = workspace->filePath(QStringLiteral("upload.") + suffix);
    QImageWriter writer(target, token);
    const bool supportsQuality = writer.supportsOption(QImageIOHandler::Quality);
    if (supportsQuality) {
        writer.setQuality(std::clamp(profile.quality, 0, 100));
    }
    if (writer.supportsOption(QImageIOHandler::CompressionRatio) &&
        profile.compression >= 0) {
        writer.setCompression(std::clamp(profile.compression, 0, 9));
    }
    if (!writer.write(image)) {
        if (error) {
            *error = QStringLiteral("The image could not be re-encoded as %1: %2")
                       .arg(QString::fromLatin1(token).toUpper(), writer.errorString());
        }
        return false;
    }

    exported->file = target;
    exported->remoteName = remoteFileName(source, suffix);
    exported->mimeType = mimeTypeForFile(target);
    exported->converted = true;
    return true;
}

}
