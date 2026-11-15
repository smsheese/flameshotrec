// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/remoteuploader.h"
#include "remote/mediaexporter.h"
#include "remote/remotestoragekeys.h"
#include "remote/s3signer.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>

namespace RemoteStorage {
namespace {

// curl configuration values are double quoted; a newline would break out of the
// value, so it is escaped rather than passed through.
QString configQuote(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

// sftp batch commands use double quotes around both operands.
QString sftpQuote(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QStringLiteral(" "));
    value.replace(QLatin1Char('\r'), QStringLiteral(" "));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

// POSIX single quoting for the remote side of an SSH transfer: everything up to
// the next single quote is literal, so no shell expansion can happen inside a
// hostile file name.
QString posixQuote(const QString& value)
{
    QString quoted;
    quoted.reserve(value.size() + 2);
    quoted.append(QLatin1Char('\''));
    for (const QChar character : value) {
        if (character == QLatin1Char('\'')) {
            quoted.append(QStringLiteral("'\\''"));
        } else {
            quoted.append(character);
        }
    }
    quoted.append(QLatin1Char('\''));
    return quoted;
}

QString bracketsForIpv6(const QString& host)
{
    return host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']')
                                           : host;
}

QString dotOrSlash(const QString& directory)
{
    const QString trimmed = directory.trimmed();
    return trimmed.isEmpty() ? QStringLiteral(".") : trimmed;
}

QString joinRemotePath(const QString& directory, const QString& name)
{
    QString path = dotOrSlash(directory);
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (path == QLatin1String(".")) {
        return name;
    }
    return path + QLatin1Char('/') + name;
}

// Percent-encoded path for a URL based transfer.
QString encodedPath(const QString& directory, const QString& name)
{
    QString raw = joinRemotePath(directory, name);
    if (!raw.startsWith(QLatin1Char('/'))) {
        raw.prepend(QLatin1Char('/'));
    }
    QUrl url;
    url.setPath(raw, QUrl::DecodedMode);
    return url.path(QUrl::FullyEncoded);
}

QString ftpUrl(const Connection& connection, const QString& encoded)
{
    // `encoded` comes from encodedPath() and is already percent-encoded; passing
    // it through QUrl::DecodedMode again would turn %20 into %2520.
    QString value = QStringLiteral("ftp://") + bracketsForIpv6(normalizedHost(connection.host));
    if (connection.port != 0) {
        value += QLatin1Char(':') + QString::number(connection.port);
    }
    value += encoded.startsWith(QLatin1Char('/')) ? encoded : QLatin1Char('/') + encoded;
    return value;
}

QString escapedUrl(const QString& scheme,
                   const QString& user,
                   const QString& host,
                   quint16 port,
                   const QString& encoded)
{
    QString value = scheme + QStringLiteral("://");
    if (!user.isEmpty()) {
        value += user + QLatin1Char('@');
    }
    value += bracketsForIpv6(host);
    if (port != 0) {
        value += QLatin1Char(':') + QString::number(port);
    }
    value += encoded.startsWith(QLatin1Char('/')) ? encoded : QLatin1Char('/') + encoded;
    return value;
}

QStringList sshCommonOptions(const Connection& connection)
{
    // The selected known_hosts file is the only trust anchor: the global
    // known-hosts database is disabled explicitly, and every client config file
    // is skipped with -F none (see the invocations below), so a host key trusted
    // elsewhere cannot satisfy verification here.
    return { QStringLiteral("-oBatchMode=yes"),
             QStringLiteral("-oIdentitiesOnly=yes"),
             QStringLiteral("-oConnectTimeout=15"),
             QStringLiteral("-oServerAliveInterval=15"),
             QStringLiteral("-oServerAliveCountMax=3"),
             QStringLiteral("-oStrictHostKeyChecking=yes"),
             QStringLiteral("-oGlobalKnownHostsFile=/dev/null"),
             QStringLiteral("-oUserKnownHostsFile=") +
               configQuote(QFileInfo(connection.knownHostsPath).absoluteFilePath()) };
}

QStringList sftpInvocation(const Connection& connection)
{
    QStringList arguments{ QStringLiteral("-F"), QStringLiteral("none"),
                           QStringLiteral("-b"), QStringLiteral("-"),
                           QStringLiteral("-P"),
                           QString::number(connection.port == 0 ? 22 : connection.port),
                           QStringLiteral("-i"), connection.privateKeyPath };
    arguments += sshCommonOptions(connection);
    arguments += QStringList{ connection.username + QLatin1Char('@') +
                              bracketsForIpv6(normalizedHost(connection.host)) };
    return arguments;
}

QStringList sshInvocation(const Connection& connection)
{
    QStringList arguments{ QStringLiteral("-F"), QStringLiteral("none"),
                           QStringLiteral("-T"),
                           QStringLiteral("-p"),
                           QString::number(connection.port == 0 ? 22 : connection.port),
                           QStringLiteral("-i"), connection.privateKeyPath };
    arguments += sshCommonOptions(connection);
    arguments += QStringList{ QStringLiteral("-oPasswordAuthentication=no"),
                              QStringLiteral("-oKbdInteractiveAuthentication=no"),
                              connection.username + QLatin1Char('@') +
                                bracketsForIpv6(normalizedHost(connection.host)) };
    return arguments;
}

QByteArray sha256File(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return hash.result().toHex();
}

} // namespace

// Parses the one `ls -l` record for `remotePath` and compares its size FIELD.
// A whitespace token anywhere in the output is not evidence: link counts, owner
// names, dates and echoed commands can contain the expected number.
SizeCheck checkSftpListingSize(const QString& output, const QString& remotePath,
                               qint64 expectedSize)
{
    if (remotePath.isEmpty()) {
        return SizeCheck::Unparsable;
    }
    // -rw-r--r--    1 owner group 1234 Nov 15 10:00 /path/to/.name.part
    // The size is the fifth field; the name is the rest of the line, so match the
    // path at the end and require the record to be for that path only.
    const QString escaped = QRegularExpression::escape(remotePath);
    // OpenSSH prints "?" when it cannot report a link count, so that field is
    // "?" or a number. The size is still the fifth field, and the record must be
    // for exactly `remotePath`.
    const QRegularExpression record(
      QStringLiteral("^[bcdlps-][rwxsStT-]{9}[.+@]?\\s+(?:\\d+|\\?)\\s+\\S+\\s+\\S+\\s+"
                     "(\\d+)\\s+\\S+\\s+\\d+\\s+[\\d:]+\\s+") + escaped +
      QStringLiteral("$"));
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QRegularExpressionMatch match = record.match(line.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        bool ok = false;
        const qint64 size = match.captured(1).toLongLong(&ok);
        if (!ok) {
            return SizeCheck::Unparsable;
        }
        return size == expectedSize ? SizeCheck::Match : SizeCheck::Mismatch;
    }
    return SizeCheck::Unparsable;
}

// Reads the byte count an FTP server reports for a path. curl's `--head` asks
// with SIZE and prints the answer as a `Content-Length:` pseudo-header, while a
// raw server reply is `213 <size>`; both spellings are accepted. A server that
// does not implement SIZE, or denies it, produces neither, which is treated as
// "cannot verify" and fails the upload.
SizeCheck checkFtpSizeReply(const QString& output, qint64 expectedSize)
{
    static const QRegularExpression contentLength(
      QStringLiteral("(?:^|\\n)\\s*Content-Length:\\s*(\\d+)\\s*$"),
      QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rawReply(
      QStringLiteral("(?:^|\\n)\\s*213\\s+(\\d+)\\s*$"));
    for (const QRegularExpression& pattern : { contentLength, rawReply }) {
        const QRegularExpressionMatch match = pattern.match(output);
        if (!match.hasMatch()) {
            continue;
        }
        bool ok = false;
        const qint64 size = match.captured(1).toLongLong(&ok);
        if (!ok) {
            return SizeCheck::Unparsable;
        }
        return size == expectedSize ? SizeCheck::Match : SizeCheck::Mismatch;
    }
    return SizeCheck::Unparsable;
}

QString remoteLocation(const Settings& settings,
                       MediaKind kind,
                       const QString& remoteName)
{
    const Connection& connection = settings.connection;
    const QString directory = kind == MediaKind::Image ? settings.image.directory
                                                       : settings.video.directory;
    const QString destination =
      directory.isEmpty() ? (connection.protocol == Protocol::S3 ? connection.keyPrefix
                                                                : connection.remoteDirectory)
                          : directory;
    if (connection.protocol == Protocol::S3) {
        QString scheme;
        QString host;
        QString ignored;
        if (!parseEndpoint(connection.endpoint, &scheme, &host, &ignored)) {
            return {};
        }
        QList<QString> segments;
        segments.append(connection.bucket);
        for (const QString& part : objectKeySegments(destination, remoteName)) {
            segments.append(part);
        }
        QString encoded;
        for (const QString& segment : segments) {
            encoded += QLatin1Char('/') + S3::uriEncode(segment);
        }
        return scheme + QStringLiteral("://") + host + encoded;
    }
    const QString encoded = encodedPath(destination, remoteName);
    const QString host = normalizedHost(connection.host);
    switch (connection.protocol) {
        case Protocol::FTP:
            return escapedUrl(QStringLiteral("ftp"), {}, host, connection.port, encoded);
        case Protocol::SFTP:
            return escapedUrl(QStringLiteral("sftp"), connection.username, host,
                              connection.port, encoded);
        case Protocol::SSH:
            return escapedUrl(QStringLiteral("ssh"), connection.username, host,
                              connection.port, encoded);
        case Protocol::S3:
            break;
    }
    return {};
}

bool buildVideoArguments(const VideoProfile& profile,
                         const QString& source,
                         const QString& target,
                         QStringList* arguments,
                         QString* error)
{
    if (error) {
        error->clear();
    }
    if (arguments == nullptr) {
        return false;
    }
    if (!videoContainers().contains(profile.container) ||
        !isSupportedVideoEncoder(profile.container, profile.codec)) {
        if (error) {
            *error = QStringLiteral("The %1 encoder cannot be stored in a %2 file.")
                       .arg(profile.codec, profile.container);
        }
        return false;
    }
    QStringList args{ QStringLiteral("-nostdin"),
                      QStringLiteral("-hide_banner"),
                      QStringLiteral("-loglevel"),
                      QStringLiteral("error"),
                      QStringLiteral("-y"),
                      QStringLiteral("-i"),
                      source,
                      QStringLiteral("-map"),
                      QStringLiteral("0:v:0") };
    if (profile.audioMode != QLatin1String("none")) {
        args << QStringLiteral("-map") << QStringLiteral("0:a?");
    }
    args << QStringLiteral("-c:v") << profile.codec;
    if (profile.rateControl == QLatin1String("abr")) {
        args << QStringLiteral("-b:v")
             << QStringLiteral("%1k").arg(std::clamp(profile.bitrateKbps, 100, 100000));
    } else if (profile.codec.startsWith(QLatin1String("libvpx"))) {
        // Constant-quality VP9 needs an explicit target of zero.
        args << QStringLiteral("-crf") << QString::number(std::clamp(profile.quality, 0, 63))
             << QStringLiteral("-b:v") << QStringLiteral("0");
    } else {
        args << QStringLiteral("-crf") << QString::number(std::clamp(profile.quality, 0, 63));
    }
    if (profile.audioMode == QLatin1String("none")) {
        args << QStringLiteral("-an");
    } else if (profile.audioMode == QLatin1String("copy")) {
        args << QStringLiteral("-c:a") << QStringLiteral("copy");
    } else {
        QString codec = profile.audioCodec;
        if (codec.isEmpty() || codec == QLatin1String("auto")) {
            codec = profile.container == QLatin1String("webm") ? QStringLiteral("libopus")
                                                               : QStringLiteral("aac");
        }
        if (!videoAudioCodecs(profile.container).contains(codec)) {
            if (error) {
                *error = QStringLiteral("The %1 audio encoder cannot be stored in a %2 file.")
                           .arg(codec, profile.container);
            }
            return false;
        }
        args << QStringLiteral("-c:a") << codec << QStringLiteral("-b:a")
             << QStringLiteral("%1k").arg(std::clamp(profile.audioBitrateKbps, 32, 512));
    }
    if (profile.container == QLatin1String("mp4")) {
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    }
    args << target;
    *arguments = args;
    return true;
}

QString destinationDirectory(const Settings& settings, MediaKind kind)
{
    const QString directory =
      kind == MediaKind::Image ? settings.image.directory : settings.video.directory;
    if (!directory.isEmpty()) {
        return directory;
    }
    return settings.connection.protocol == Protocol::S3 ? settings.connection.keyPrefix
                                                        : settings.connection.remoteDirectory;
}

QString temporaryRemoteName(const QString& remoteName, const QString& transferId)
{
    // Unique per transfer, in the destination directory, and clearly temporary.
    // Collisions between two transfers are impossible in practice, so one
    // transfer can never truncate another's data.
    return QLatin1Char('.') + remoteName + QStringLiteral(".flameshot-") + transferId +
           QStringLiteral(".part");
}

QString uniqueRemoteFileName(const QString& remoteName, const QString& transferId)
{
    // Insert a short unique suffix into the base name, keeping the extension so
    // the object is still a .png/.mp4 for anything that looks at the suffix.
    const QFileInfo info(remoteName);
    const QString suffix = info.suffix();
    const QString base = suffix.isEmpty()
                           ? remoteName
                           : remoteName.left(remoteName.size() - suffix.size() - 1);
    const QString unique = transferId.left(8).toLower();
    return suffix.isEmpty() ? base + QLatin1Char('-') + unique
                            : base + QLatin1Char('-') + unique + QLatin1Char('.') + suffix;
}

QString noReplaceGuarantee(Protocol protocol)
{
    switch (protocol) {
        case Protocol::S3:
            return QStringLiteral(
              "S3 is asked for a conditional create (If-None-Match: *), so a "
              "collision with an existing object is refused by the server and "
              "the upload fails instead of replacing it.");
        case Protocol::SSH:
            return QStringLiteral(
              "The file is streamed to a per-transfer temporary, its byte count "
              "is verified, and it is published with link(2), which fails rather "
              "than replacing an existing file. If the connection drops before "
              "the remote shell cleans up, the temporary can be left behind; "
              "removal is best effort, not guaranteed.");
        case Protocol::SFTP:
            return QStringLiteral(
              "Each upload gets its own unique final name, so two uploads of the "
              "same screenshot or recording name cannot collide. The bytes go to "
              "a per-transfer temporary whose size is verified before the "
              "publish, so a truncated or cancelled transfer is not published. "
              "Publishing uses the server's rename, which some servers implement "
              "as a replace; a concurrent writer that creates the same unique "
              "name could still be overwritten, and cleanup of a temporary after "
              "a dropped connection is best effort only.");
        case Protocol::FTP:
            return QStringLiteral(
              "Each upload gets its own unique final name, so two uploads of the "
              "same name cannot collide. The bytes go to a per-transfer "
              "temporary, its size is read back before publishing, and the "
              "publish is an RNFR/RNTO rename whose result is confirmed by "
              "stat-ing the final object. FTP has no atomic rename and no way to "
              "clean up reliably after a connection loss: a temporary can be left "
              "behind and cleanup is best effort only.");
    }
    return {};
}

QList<Command> buildTransferPlan(const Settings& settings,
                                 MediaKind kind,
                                 const QString& artifactPath,
                                 const QString& remoteName,
                                 const QString& mimeType,
                                 const Credentials& credentials,
                                 const QDateTime& now,
                                 const QString& transferId,
                                 QString* error)
{
    if (error) {
        error->clear();
    }
    QList<Command> plan;
    const Connection& connection = settings.connection;
    const QString directory =
      kind == MediaKind::Image ? settings.image.directory : settings.video.directory;
    const QString destination =
      directory.isEmpty() ? (connection.protocol == Protocol::S3 ? connection.keyPrefix
                                                                : connection.remoteDirectory)
                          : directory;
    const QString tempName = temporaryRemoteName(remoteName, transferId);

    auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return QList<Command>{};
    };

    switch (connection.protocol) {
        case Protocol::S3: {
            QString scheme;
            QString endpointHost;
            QString endpointError;
            if (!parseEndpoint(connection.endpoint, &scheme, &endpointHost, &endpointError)) {
                return fail(endpointError);
            }
            const QString curl = RemoteUploader::curlExecutable();
            if (curl.isEmpty()) {
                return fail(QStringLiteral("curl was not found. Install curl to upload to S3."));
            }
            const QByteArray payloadHash = sha256File(artifactPath);
            if (payloadHash.isEmpty()) {
                return fail(QStringLiteral("The file to upload could not be read."));
            }
            // The configured prefix is used literally: repeated slashes and dot
            // segments are part of the key and must not be normalised.
            QList<QString> segments;
            segments.append(connection.bucket);
            for (const QString& part : objectKeySegments(destination, remoteName)) {
                segments.append(part);
            }

            const QString contentType =
              mimeType.isEmpty() ? mimeTypeForFile(remoteName) : mimeType;

            S3::Request request;
            request.method = QByteArrayLiteral("PUT");
            request.host = endpointHost;
            request.pathSegments = segments;
            request.payloadHash = payloadHash;
            request.headers.append({ QByteArrayLiteral("content-type"),
                                     contentType.toUtf8() });
            // Conditional create: the object must not exist. This header is part
            // of the signature, so a redirect or proxy cannot strip it either.
            request.headers.append({ QByteArrayLiteral("if-none-match"),
                                     QByteArrayLiteral("*") });

            S3::Credentials s3Credentials;
            s3Credentials.accessKeyId = connection.accessKeyId;
            s3Credentials.secretAccessKey = credentials.secretAccessKey;
            s3Credentials.sessionToken = credentials.sessionToken;
            const S3::Signed signedRequest =
              S3::sign(request, s3Credentials, connection.region, now);

            QString url = scheme + QStringLiteral("://") + endpointHost;
            for (const QString& segment : segments) {
                url += QLatin1Char('/') + S3::uriEncode(segment);
            }

            Command command;
            command.program = curl;
            command.arguments = { QStringLiteral("--disable"),
                                  QStringLiteral("--silent"),
                                  QStringLiteral("--show-error"),
                                  QStringLiteral("--connect-timeout"),
                                  QStringLiteral("15"),
                                  QStringLiteral("--request"),
                                  QStringLiteral("PUT"),
                                  QStringLiteral("--url"),
                                  url,
                                  // Send the signed path exactly as signed: no
                                  // dot-segment resolution by the client.
                                  QStringLiteral("--path-as-is"),
                                  QStringLiteral("--output"),
                                  QStringLiteral("/dev/null"),
                                  QStringLiteral("--write-out"),
                                  QStringLiteral("%{http_code}"),
                                  QStringLiteral("--config"),
                                  QStringLiteral("-"),
                                  QStringLiteral("--upload-file"),
                                  artifactPath };
            command.uploadFile = artifactPath;
            command.label = QStringLiteral("uploading");
            command.expectOkHttpStatus = true;
            QString config;
            config += QStringLiteral("header = %1\n")
                        .arg(configQuote(QStringLiteral("Content-Type: ") + contentType));
            config += QStringLiteral("header = %1\n")
                        .arg(configQuote(QStringLiteral("If-None-Match: *")));
            config += QStringLiteral("header = %1\n")
                        .arg(configQuote(QString::fromLatin1(
                          "Authorization: " + signedRequest.authorization)));
            for (const auto& header : signedRequest.headers) {
                config += QStringLiteral("header = %1\n")
                            .arg(configQuote(QString::fromLatin1(header.first) +
                                             QStringLiteral(": ") +
                                             QString::fromLatin1(header.second)));
            }
            config += QStringLiteral("header = \"Expect:\"\n");
            command.stdinData = config.toUtf8();
            plan.append(command);
            return plan;
        }
        case Protocol::SSH: {
            const QString ssh = RemoteUploader::sshExecutable();
            if (ssh.isEmpty()) {
                return fail(QStringLiteral(
                  "OpenSSH ssh was not found; install the OpenSSH client."));
            }
            const QString remote = joinRemotePath(destination, remoteName);
            const QString remoteTemp = joinRemotePath(destination, tempName);
            const qint64 size = QFileInfo(artifactPath).size();
            // The remote script receives the destination, the temporary path and
            // the expected size as arguments (never interpolated into the script
            // text). It verifies the byte count, publishes with link(2) - which
            // fails on an existing target instead of replacing it - and removes
            // only its own temporary file.
            const QString script =
              QStringLiteral(
                "sh -c 'set -e; umask 022; d=$1; t=$2; n=$3; "
                "cat > \"$t\"; s=$(wc -c < \"$t\"); "
                "[ \"$s\" = \"$n\" ] || { rm -f \"$t\"; exit 4; }; "
                "ln \"$t\" \"$d\" 2>/dev/null || { rm -f \"$t\"; exit 3; }; "
                "rm -f \"$t\"' flameshot-upload ") +
              posixQuote(remote) + QLatin1Char(' ') + posixQuote(remoteTemp) +
              QLatin1Char(' ') + QString::number(size);

            Command command;
            command.program = ssh;
            command.arguments = sshInvocation(connection);
            command.arguments += QStringList{ script };
            command.stdinFile = artifactPath;
            command.label = QStringLiteral("uploading");
            command.collisionExitCode = 3;
            command.interruptedExitCode = 4;
            plan.append(command);
            return plan;
        }
        case Protocol::SFTP: {
            const QString sftp = RemoteUploader::sftpExecutable();
            if (sftp.isEmpty()) {
                return fail(QStringLiteral(
                  "OpenSSH sftp was not found; install the OpenSSH client."));
            }
            const QString remoteFinal = joinRemotePath(destination, remoteName);
            const QString remoteTemp = joinRemotePath(destination, tempName);

            // No existence probe: the final name is unique to this upload, so an
            // ordinary same-name collision cannot happen, and a server that
            // denies listing can no longer be mistaken for "the path is free".
            // 1. Upload to the per-transfer temporary and read back its size
            //    field, so a truncated transfer is never published.
            Command put;
            put.program = sftp;
            put.arguments = sftpInvocation(connection);
            put.stdinData = QStringLiteral("put %1 %2\nls -l %3\n")
                              .arg(sftpQuote(artifactPath), sftpQuote(remoteTemp),
                                   sftpQuote(remoteTemp))
                              .toUtf8();
            put.label = QStringLiteral("uploading");
            put.expectSizeInOutput = QFileInfo(artifactPath).size();
            put.sizeCheckKind = QByteArrayLiteral("sftp");
            plan.append(put);

            // 2. Publish. OpenSSH rename may replace an existing target, which is
            //    why the final name is unique; see noReplaceGuarantee().
            Command publish;
            publish.program = sftp;
            publish.arguments = sftpInvocation(connection);
            publish.stdinData =
              QStringLiteral("rename %1 %2\n").arg(sftpQuote(remoteTemp),
                                                   sftpQuote(remoteFinal)).toUtf8();
            publish.label = QStringLiteral("publishing");
            plan.append(publish);
            return plan;
        }
        case Protocol::FTP: {
            const QString curl = RemoteUploader::curlExecutable();
            if (curl.isEmpty()) {
                return fail(QStringLiteral(
                  "curl was not found. Install curl with FTP support."));
            }
            const QString tempUrl = ftpUrl(connection, encodedPath(destination, tempName));
            const QString finalUrl = ftpUrl(connection, encodedPath(destination, remoteName));
            const QString directoryUrl = ftpUrl(
              connection, encodedPath(destination, QString())).section(QLatin1Char('?'), 0, 0);
            const QString credentialLine =
              QStringLiteral("user = %1\n")
                .arg(configQuote(connection.username + QLatin1Char(':') +
                                 credentials.password));
            auto authConfig = [&credentialLine](const QString& url) {
                return (QStringLiteral("url = %1\n").arg(configQuote(url)) + credentialLine)
                  .toUtf8();
            };

            // No existence probe (a FTP server may deny SIZE while allowing
            // STOR, and a failed probe is not evidence of absence). The final
            // name is unique, and the bytes are written to a per-transfer
            // temporary first, so the destination is never written directly.
            Command upload;
            upload.program = curl;
            upload.arguments = { QStringLiteral("--disable"),
                                 QStringLiteral("--silent"),
                                 QStringLiteral("--show-error"),
                                 QStringLiteral("--fail"),
                                 QStringLiteral("--connect-timeout"),
                                 QStringLiteral("15"),
                                 QStringLiteral("--config"),
                                 QStringLiteral("-"),
                                 QStringLiteral("--upload-file"),
                                 artifactPath };
            upload.uploadFile = artifactPath;
            upload.stdinData = authConfig(tempUrl);
            upload.label = QStringLiteral("uploading");
            plan.append(upload);

            // Verify what the server actually stored before publishing.
            Command verifyTemp;
            verifyTemp.program = curl;
            verifyTemp.arguments = { QStringLiteral("--disable"),
                                     QStringLiteral("--silent"),
                                     QStringLiteral("--show-error"),
                                     QStringLiteral("--head"),
                                     QStringLiteral("--connect-timeout"),
                                     QStringLiteral("15"),
                                     QStringLiteral("--config"),
                                     QStringLiteral("-") };
            verifyTemp.stdinData = authConfig(tempUrl);
            verifyTemp.label = QStringLiteral("verifying");
            verifyTemp.expectSizeInOutput = QFileInfo(artifactPath).size();
            verifyTemp.sizeCheckKind = QByteArrayLiteral("ftp");
            plan.append(verifyTemp);

            // Publish by renaming the temporary. RNFR/RNTO is not atomic and may
            // be refused; the exit status is advisory and the check below decides.
            Command rename;
            rename.program = curl;
            rename.arguments = { QStringLiteral("--disable"),
                                 QStringLiteral("--silent"),
                                 QStringLiteral("--show-error"),
                                 QStringLiteral("--connect-timeout"),
                                 QStringLiteral("15"),
                                 QStringLiteral("--list-only"),
                                 QStringLiteral("--output"),
                                 QStringLiteral("/dev/null"),
                                 QStringLiteral("--quote"),
                                 QStringLiteral("RNFR ") + tempName,
                                 QStringLiteral("--quote"),
                                 QStringLiteral("RNTO ") + remoteName,
                                 QStringLiteral("--config"),
                                 QStringLiteral("-") };
            rename.stdinData = authConfig(directoryUrl);
            rename.label = QStringLiteral("publishing");
            rename.advisoryExitStatus = true;
            plan.append(rename);

            // Only a positively verified final object counts as success.
            Command verifyFinal;
            verifyFinal.program = curl;
            verifyFinal.arguments = { QStringLiteral("--disable"),
                                      QStringLiteral("--silent"),
                                      QStringLiteral("--show-error"),
                                      QStringLiteral("--head"),
                                      QStringLiteral("--connect-timeout"),
                                      QStringLiteral("15"),
                                      QStringLiteral("--config"),
                                      QStringLiteral("-") };
            verifyFinal.stdinData = authConfig(finalUrl);
            verifyFinal.label = QStringLiteral("verifying");
            verifyFinal.expectSizeInOutput = QFileInfo(artifactPath).size();
            verifyFinal.sizeCheckKind = QByteArrayLiteral("ftp");
            plan.append(verifyFinal);
            return plan;
        }
    }
    return fail(QStringLiteral("Unsupported transfer protocol."));
}

bool buildCleanupCommand(const Settings& settings,
                         MediaKind kind,
                         const QString& remoteName,
                         const QString& transferId,
                         const Credentials& credentials,
                         Command* command)
{
    if (command == nullptr) {
        return false;
    }
    *command = Command{};
    const Connection& connection = settings.connection;
    if (connection.protocol != Protocol::SFTP && connection.protocol != Protocol::FTP) {
        // SSH removes its own temporary inside the remote script; S3 publishes
        // conditionally and has no temporary object at all.
        return false;
    }
    const QString directory =
      kind == MediaKind::Image ? settings.image.directory : settings.video.directory;
    const QString destination = directory.isEmpty() ? connection.remoteDirectory : directory;
    const QString remoteTemp =
      joinRemotePath(destination, temporaryRemoteName(remoteName, transferId));
    if (connection.protocol == Protocol::FTP) {
        // Remove only this transfer's temporary, best effort.
        const QString curl = RemoteUploader::curlExecutable();
        if (curl.isEmpty()) {
            return false;
        }
        const QString url = ftpUrl(
          connection, encodedPath(directory.isEmpty() ? connection.remoteDirectory : directory,
                                  QString()));
        command->program = curl;
        command->arguments = { QStringLiteral("--disable"),
                               QStringLiteral("--silent"),
                               QStringLiteral("--list-only"),
                               QStringLiteral("--output"),
                               QStringLiteral("/dev/null"),
                               QStringLiteral("--quote"),
                               QStringLiteral("DELE ") + temporaryRemoteName(remoteName, transferId),
                               QStringLiteral("--config"),
                               QStringLiteral("-") };
        command->stdinData =
          (QStringLiteral("url = %1\n").arg(configQuote(url)) +
           QStringLiteral("user = %1\n")
             .arg(configQuote(connection.username + QLatin1Char(':') + credentials.password)))
            .toUtf8();
        command->label = QStringLiteral("cleanup");
        return true;
    }
    command->program = RemoteUploader::sftpExecutable();
    if (command->program.isEmpty()) {
        return false;
    }
    command->arguments = sftpInvocation(connection);
    command->stdinData = QStringLiteral("rm %1\n").arg(sftpQuote(remoteTemp)).toUtf8();
    command->label = QStringLiteral("cleanup");
    return true;
}

RemoteUploader::RemoteUploader(QObject* parent)
  : QObject(parent)
{}

RemoteUploader::~RemoteUploader()
{
    cleanupProcess();
    // Note: QString is implicitly shared, so this drops our reference rather than
    // guaranteeing the bytes are gone from memory.
    m_credentials = Credentials{};
}

QString RemoteUploader::curlExecutable()
{
    return QStandardPaths::findExecutable(QStringLiteral("curl"));
}

QString RemoteUploader::sftpExecutable()
{
    return QStandardPaths::findExecutable(QStringLiteral("sftp"));
}

QString RemoteUploader::sshExecutable()
{
    return QStandardPaths::findExecutable(QStringLiteral("ssh"));
}

QString RemoteUploader::ffmpegExecutable(const QString& configuredPath)
{
    const QString configured = configuredPath.trimmed();
    if (!configured.isEmpty()) {
        const QFileInfo info(configured);
        return info.isExecutable() && info.isFile() ? info.absoluteFilePath() : QString();
    }
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

bool RemoteUploader::isRunning() const
{
    return m_process != nullptr;
}

void RemoteUploader::cancel()
{
    if (m_terminal) {
        return;
    }
    // Record the decision first: a process that finishes between here and the
    // kill must not advance the plan or report success.
    m_cancelled = true;
    m_pendingFailure = tr("The upload was cancelled; the local file was kept.");
    if (m_process != nullptr) {
        m_process->kill();
    }
}

void RemoteUploader::cleanupProcess()
{
    if (m_process == nullptr) {
        return;
    }
    QProcess* process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(1000);
    }
    process->deleteLater();
}

bool RemoteUploader::upload(const QString& localFile,
                            MediaKind kind,
                            const Settings& settings,
                            const Credentials& credentials)
{
    if (isRunning()) {
        emit failed(tr("An upload is already in progress."));
        return false;
    }
    const QFileInfo file(localFile);
    if (!file.isFile() || !file.isReadable() || file.size() <= 0) {
        emit failed(tr("The file to upload is missing or unreadable; nothing was sent."));
        return false;
    }
    const QString settingsError = validate(settings, kind);
    if (!settingsError.isEmpty()) {
        emit failed(settingsError);
        return false;
    }
    if (settings.connection.protocol == Protocol::S3 &&
        credentials.secretAccessKey.isEmpty()) {
        emit failed(tr("The S3 secret access key is required and is never stored."));
        return false;
    }

    m_localFile = file.absoluteFilePath();
    m_settings = settings;
    m_credentials = credentials;
    m_kind = kind;
    m_remoteName.clear();
    m_mimeType.clear();
    m_artifact.clear();
    m_progressBuffer.clear();
    m_steps.clear();
    m_stepIndex = 0;
    m_pendingFailure.clear();
    m_cleaningUp = false;
    m_transferId = QString::number(QRandomGenerator::system()->generate64(), 16);
    m_cancelled = false;
    m_terminal = false;
    m_workspace.reset(new QTemporaryDir);
    if (!m_workspace->isValid()) {
        m_workspace.reset();
        m_credentials = Credentials{};
        emit failed(tr("A temporary directory for the upload could not be created."));
        return false;
    }

    const bool convert =
      kind == MediaKind::Image ? settings.image.conversion == Conversion::Convert
                               : settings.video.conversion == Conversion::Convert;

    if (!convert) {
        // "Use original" sends the saved file unchanged. The final object name is
        // unique to this upload, so two uploads of the same screenshot or
        // recording name never race for one destination.
        m_artifact = m_localFile;
        m_remoteName = uniqueRemoteFileName(remoteFileName(m_localFile), m_transferId);
        m_mimeType = mimeTypeForFile(m_localFile);
        emit prepared(m_artifact, m_remoteName, m_mimeType);
        startTransfer();
        return true;
    }

    if (kind == MediaKind::Image) {
        ExportedMedia exported;
        QString error;
        if (!exportImage(m_localFile, settings.image, m_workspace.data(), &exported, &error)) {
            finishWithError(error.isEmpty() ? tr("The screenshot could not be converted.")
                                            : error);
            return true;
        }
        m_artifact = exported.file;
        m_remoteName = uniqueRemoteFileName(exported.remoteName, m_transferId);
        m_mimeType = exported.mimeType;
        emit prepared(m_artifact, m_remoteName, m_mimeType);
        startTransfer();
        return true;
    }

    startTranscode();
    return true;
}

void RemoteUploader::startTranscode()
{
    const QString ffmpeg = ffmpegExecutable();
    if (ffmpeg.isEmpty()) {
        finishWithError(tr("FFmpeg was not found, so the video cannot be converted. "
                           "Install FFmpeg or set its path in Configuration > Video."));
        return;
    }
    const VideoProfile& profile = m_settings.video;
    const QString target = m_workspace->filePath(
      QStringLiteral("upload.") + profile.container.trimmed().toLower());
    QStringList arguments;
    QString error;
    if (!buildVideoArguments(profile, m_localFile, target, &arguments, &error)) {
        finishWithError(error);
        return;
    }
    m_process = new QProcess(this);
    m_process->setProgram(ffmpeg);
    m_process->setArguments(arguments);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::started, this, [this, profile]() {
        emit progress(tr("Converting to %1…").arg(profile.container));
    });
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        m_progressBuffer += m_process->readAllStandardOutput();
        while (true) {
            const qsizetype newline = m_progressBuffer.indexOf('\n');
            if (newline < 0) {
                break;
            }
            const QByteArray line = m_progressBuffer.left(newline);
            m_progressBuffer.remove(0, newline + 1);
            if (line.startsWith("out_time_us=")) {
                bool ok = false;
                const qint64 microseconds = line.mid(12).trimmed().toLongLong(&ok);
                if (ok && microseconds > 0) {
                    emit progress(tr("Converting… %1 s")
                                    .arg(QString::number(microseconds / 1000000)));
                }
            }
        }
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                auto* process = m_process;
                m_process = nullptr;
                const QString detail =
                  QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                if (status != QProcess::NormalExit || code != 0) {
                    finishWithError(detail.isEmpty()
                                      ? tr("The video conversion failed; the local file was kept.")
                                      : tr("The video conversion failed; the local file was kept. %1")
                                          .arg(detail));
                    return;
                }
                handleTranscodeOutput();
            });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart) {
            return;
        }
        cleanupProcess();
        finishWithError(tr("FFmpeg could not be started; the local file was kept."));
    });
    m_process->start();
}

void RemoteUploader::handleTranscodeOutput()
{
    const QString container = m_settings.video.container.trimmed().toLower();
    const QString target = m_workspace->filePath(QStringLiteral("upload.") + container);
    const QFileInfo converted(target);
    if (!converted.isFile() || converted.size() <= 0) {
        finishWithError(tr("The converted video is empty; the local file was kept."));
        return;
    }
    m_artifact = converted.absoluteFilePath();
    m_remoteName = uniqueRemoteFileName(remoteFileName(m_localFile, container), m_transferId);
    m_mimeType = mimeTypeForFile(m_artifact);
    emit prepared(m_artifact, m_remoteName, m_mimeType);
    startTransfer();
}

void RemoteUploader::startTransfer()
{
    QString error;
    m_steps = buildTransferPlan(m_settings, m_kind, m_artifact, m_remoteName, m_mimeType,
                                m_credentials, QDateTime::currentDateTimeUtc(),
                                m_transferId, &error);
    m_stepIndex = 0;
    m_cleaningUp = false;
    if (m_steps.isEmpty()) {
        finishWithError(error.isEmpty()
                          ? tr("The upload could not be prepared; the local file was kept.")
                          : error);
        return;
    }
    startNextStep();
}

void RemoteUploader::startNextStep()
{
    if (m_terminal) {
        return;
    }
    // Cancellation is checked before every step: a killed probe or upload never
    // advances the plan into the next stage.
    if (m_cancelled) {
        runCleanupThenFail(tr("The upload was cancelled; the local file was kept."));
        return;
    }
    if (m_stepIndex >= m_steps.size()) {
        finishSuccessfully();
        return;
    }

    const Command command = m_steps.at(m_stepIndex);
    m_process = new QProcess(this);
    QProcess* process = m_process;
    process->setProgram(command.program);
    process->setArguments(command.arguments);
    if (!command.stdinFile.isEmpty()) {
        process->setStandardInputFile(command.stdinFile);
    }
    const QByteArray payload = command.stdinData;
    const QString label = command.label;
    connect(process, &QProcess::started, this, [this, payload, label]() {
        if (m_terminal) {
            return;
        }
        if (label == QLatin1String("verifying")) {
            emit progress(tr("Verifying %1…").arg(m_remoteName));
        } else if (label == QLatin1String("publishing")) {
            emit progress(tr("Publishing %1…").arg(m_remoteName));
        } else {
            emit progress(tr("Transferring %1…").arg(m_remoteName));
        }
        if (payload.isEmpty() || m_process == nullptr) {
            return;
        }
        m_process->write(payload);
        m_process->closeWriteChannel();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, command](int code, QProcess::ExitStatus status) {
                auto* finished = m_process;
                m_process = nullptr;
                if (finished == nullptr) {
                    return;
                }
                const QString output = QString::fromUtf8(finished->readAllStandardOutput());
                const QString detail =
                  QString::fromUtf8(finished->readAllStandardError()).trimmed();
                finished->deleteLater();

                if (m_terminal) {
                    return;
                }
                if (m_cleaningUp) {
                    // The cleanup result never replaces the real error.
                    finishWithError(m_pendingFailure);
                    return;
                }
                if (m_cancelled) {
                    runCleanupThenFail(
                      tr("The upload was cancelled; the local file was kept."));
                    return;
                }

                const bool ok = status == QProcess::NormalExit && code == 0;

                if (ok) {
                    if (command.probeMustBeAbsent) {
                        // A successful existence probe means the destination is
                        // taken. (No plan probes today; kept fail-closed.)
                        finishWithError(
                          tr("The destination %1 already exists, so nothing was "
                             "overwritten.")
                            .arg(m_remoteName));
                        return;
                    }
                    if (command.expectOkHttpStatus) {
                        bool parsed = false;
                        const QString lastLine =
                          output.trimmed().section(QLatin1Char('\n'), -1).trimmed();
                        const int httpStatus = lastLine.toInt(&parsed);
                        if (!parsed || httpStatus < 200 || httpStatus > 299) {
                            if (httpStatus >= 300 && httpStatus < 400) {
                                finishWithError(
                                  tr("The server answered with an HTTP redirect (%1) "
                                     "instead of storing the object, so nothing was "
                                     "confirmed stored. Redirects are reported rather "
                                     "than followed, because following one would send "
                                     "the signed credentials to another URL.")
                                    .arg(httpStatus));
                            } else if (httpStatus == 409 || httpStatus == 412) {
                                finishWithError(
                                  tr("An object named %1 already exists at the "
                                     "destination, so nothing was overwritten.")
                                    .arg(m_remoteName));
                            } else {
                                finishWithError(
                                  tr("The server answered with HTTP %1, so the upload "
                                     "was not confirmed stored. The local file was kept.")
                                    .arg(httpStatus));
                            }
                            return;
                        }
                    }
                    if (!command.sizeCheckKind.isEmpty()) {
                        const QString remoteTemp =
                          joinRemotePath(destinationDirectory(m_settings, m_kind),
                                         temporaryRemoteName(m_remoteName, m_transferId));
                        const SizeCheck check =
                          command.sizeCheckKind == QByteArrayLiteral("ftp")
                            ? checkFtpSizeReply(output, command.expectSizeInOutput)
                            : checkSftpListingSize(output, remoteTemp,
                                                   command.expectSizeInOutput);
                        if (check != SizeCheck::Match) {
                            runCleanupThenFail(
                              check == SizeCheck::Mismatch
                                ? tr("The server stored a different number of bytes "
                                     "than were sent, so it was not published. The "
                                     "local file was kept.")
                                : tr("The size of the uploaded file could not be read "
                                     "back from the server, so it was not published. "
                                     "The local file was kept."));
                            return;
                        }
                    }
                    ++m_stepIndex;
                    startNextStep();
                    return;
                }

                // Failure path.
                if (command.advisoryExitStatus) {
                    // The rename result is not decisive: the verification step
                    // that follows decides, so an unsupported RNFR/RNTO is caught.
                    ++m_stepIndex;
                    startNextStep();
                    return;
                }
                if (command.probeMustBeAbsent) {
                    // Ambiguous probe failure: fail closed rather than assume the
                    // destination is free (a server can deny listing and allow
                    // writes).
                    finishWithError(
                      detail.isEmpty()
                        ? tr("The destination could not be checked, so nothing was "
                             "uploaded. The local file was kept.")
                        : tr("The destination could not be checked (%1), so nothing "
                             "was uploaded. The local file was kept.")
                            .arg(detail));
                    return;
                }
                if (command.collisionExitCode >= 0 && code == command.collisionExitCode) {
                    finishWithError(
                      tr("A file named %1 already exists at the destination, so "
                         "nothing was overwritten.")
                        .arg(m_remoteName));
                    return;
                }
                if (command.interruptedExitCode >= 0 && code == command.interruptedExitCode) {
                    finishWithError(
                      tr("Only part of the file arrived, so it was not published and "
                         "the partial file was removed. The local file was kept."));
                    return;
                }
                runCleanupThenFail(detail.isEmpty()
                                     ? tr("The upload failed; the local file was kept.")
                                     : tr("The upload failed; the local file was kept. %1")
                                         .arg(detail));
            });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart) {
            return;
        }
        runCleanupThenFail(
          tr("The transfer program could not be started; the local file was kept."));
    });
    process->start();
}

void RemoteUploader::runCleanupThenFail(const QString& message)
{
    if (m_terminal) {
        return;
    }
    m_pendingFailure = message;
    Command cleanup;
    if (!buildCleanupCommand(m_settings, m_kind, m_remoteName, m_transferId,
                             m_credentials, &cleanup) ||
        cleanup.program.isEmpty()) {
        finishWithError(message);
        return;
    }
    // Best effort: remove only this transfer's temporary, then report the real
    // failure. A dropped connection can leave the temporary behind.
    m_cleaningUp = true;
    m_process = new QProcess(this);
    QProcess* process = m_process;
    process->setProgram(cleanup.program);
    process->setArguments(cleanup.arguments);
    const QByteArray cleanupPayload = cleanup.stdinData;
    connect(process, &QProcess::started, this, [this, cleanupPayload]() {
        if (m_process != nullptr && !cleanupPayload.isEmpty()) {
            m_process->write(cleanupPayload);
            m_process->closeWriteChannel();
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
                auto* finished = m_process;
                m_process = nullptr;
                if (finished != nullptr) {
                    finished->deleteLater();
                }
                m_cleaningUp = false;
                finishWithError(m_pendingFailure);
            });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart) {
            return;
        }
        cleanupProcess();
        m_cleaningUp = false;
        finishWithError(m_pendingFailure);
    });
    process->start();
}

void RemoteUploader::finishWithError(const QString& message)
{
    if (m_terminal) {
        return;
    }
    m_terminal = true;
    m_steps.clear();
    m_workspace.reset();
    // Drop our reference immediately; QString storage is shared and is not
    // securely erased, which is why the secret is never persisted anywhere.
    m_credentials = Credentials{};
    // The original saved capture is deliberately left untouched.
    emit failed(message);
}

void RemoteUploader::finishSuccessfully()
{
    if (m_terminal) {
        return;
    }
    m_terminal = true;
    const QString location = remoteLocation(m_settings, m_kind, m_remoteName);
    m_steps.clear();
    m_workspace.reset();
    m_credentials = Credentials{};
    emit succeeded(m_localFile, location);
}

}
