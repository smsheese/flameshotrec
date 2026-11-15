#include "ffmpegdiscovery.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace Recording {
QString findFFmpeg(const QString &configuredPath, const QStringList &searchPaths)
{
    const auto executable = [](const QString &path) {
        const QFileInfo info(path);
        return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
    };
    if (!configuredPath.trimmed().isEmpty()) {
        const QString direct = executable(configuredPath);
        if (!direct.isEmpty()) return direct;
        const QString fromPath = QStandardPaths::findExecutable(configuredPath);
        return fromPath.isEmpty() ? QString() : fromPath;
    }
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!fromPath.isEmpty()) return fromPath;
    QStringList dirs = searchPaths;
    dirs << QStringLiteral("/usr/bin") << QStringLiteral("/usr/local/bin")
         << QStringLiteral("/bin") << QStringLiteral("/usr/lib/ffmpeg")
         << QStringLiteral("/usr/lib64/ffmpeg") << QStringLiteral("/opt/bin");
    for (const QString &dir : dirs) {
        const QString found = executable(QDir(dir).filePath(QStringLiteral("ffmpeg")));
        if (!found.isEmpty()) return found;
    }
    return {};
}
}
