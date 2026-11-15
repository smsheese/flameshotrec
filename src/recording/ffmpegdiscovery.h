#pragma once
#include <QString>
#include <QStringList>

namespace Recording {
// A non-empty configured path is authoritative: invalid paths do not silently
// fall back to a different FFmpeg binary.
QString findFFmpeg(const QString &configuredPath = {},
                   const QStringList &searchPaths = {});
}
