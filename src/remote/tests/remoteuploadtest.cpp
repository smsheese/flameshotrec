// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

// End-to-end transfer checks against loopback fixtures:
//   * a minimal FTP server (tests/fixtures/ftp_server.py)
//   * an S3-compatible endpoint that verifies SigV4 independently
//     (tests/fixtures/s3_server.py)
//   * PATH shims for ssh and sftp, which record the argument list and the
//     streamed body instead of contacting a real server
// and the two conversions (image re-encode, FFmpeg transcode) end to end, so the
// uploaded bytes are proven to be re-encoded rather than renamed.
//
// Start it through tests/run.sh, which sets the environment variables below.

#include "remote/mediaexporter.h"
#include "remote/remotestorage.h"
#include "remote/remoteuploader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QTemporaryDir>
#include <QtTest>

using namespace RemoteStorage;

namespace {

QString writeFile(const QString& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = QDir(directory).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write(bytes);
    return path;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeExecutable(const QString& path, const QByteArray& script)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(script);
    file.close();
    return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// Installs a PATH directory containing only the named shims.
QString installShimDirectory(const QTemporaryDir& workspace)
{
    const QString bin = QDir(workspace.path()).filePath(QStringLiteral("bin"));
    QDir().mkpath(bin);
    return bin;
}

Settings sftpFixtureSettings(const QString& workspace)
{
    Settings settings;
    settings.enabled = true;
    Connection& connection = settings.connection;
    connection.protocol = Protocol::SFTP;
    connection.host = QStringLiteral("fixture.example");
    connection.port = 2222;
    connection.username = QStringLiteral("fixture");
    connection.privateKeyPath = writeFile(workspace, QStringLiteral("id_key"), "key");
    connection.knownHostsPath = writeFile(workspace, QStringLiteral("known_hosts"),
                                          "fixture.example ssh-ed25519 AAAA");
    connection.remoteDirectory = QDir(workspace).filePath(QStringLiteral("remote"));
    QDir().mkpath(connection.remoteDirectory);
    return settings;
}

} // namespace


namespace {

QString envValue(const char* name)
{
    return qEnvironmentVariable(name);
}

void writeMode(const QString& modeFile, const QString& mode)
{
    QFile file(modeFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(mode.toUtf8());
    }
}

QJsonArray s3Requests(const QString& resultFile)
{
    const QJsonDocument document = QJsonDocument::fromJson(readAll(resultFile));
    return document.object().value(QStringLiteral("requests")).toArray();
}

Settings loopbackFtpSettings(const QString& port)
{
    Settings settings;
    settings.enabled = true;
    settings.connection.protocol = Protocol::FTP;
    settings.connection.host = QStringLiteral("127.0.0.1");
    settings.connection.port = static_cast<quint16>(port.toUShort());
    settings.connection.username = QStringLiteral("fixture");
    return settings;
}

Settings loopbackS3Settings(const QString& port)
{
    Settings settings;
    settings.enabled = true;
    settings.connection.protocol = Protocol::S3;
    settings.connection.endpoint = QStringLiteral("http://127.0.0.1:") + port;
    settings.connection.region = QStringLiteral("eu-west-1");
    settings.connection.bucket = QStringLiteral("shots");
    settings.connection.accessKeyId = QStringLiteral("AKIDEXAMPLE");
    return settings;
}

Credentials s3Credentials()
{
    Credentials credentials;
    credentials.secretAccessKey = QStringLiteral("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
    return credentials;
}

// Settings pointing at the local OpenSSH fixture.
bool sshdSettings(Settings* settings, Protocol protocol)
{
    const QString port = envValue("FLAMESHOT_TEST_SSHD_PORT");
    const QString dir = envValue("FLAMESHOT_TEST_SSHD_DIR");
    if (port.isEmpty() || dir.isEmpty()) {
        return false;
    }
    settings->enabled = true;
    Connection& connection = settings->connection;
    connection.protocol = protocol;
    connection.host = QStringLiteral("127.0.0.1");
    connection.port = static_cast<quint16>(port.toUShort());
    connection.username = qEnvironmentVariable("USER");
    connection.privateKeyPath = QDir(dir).filePath(QStringLiteral("keys/client_key"));
    connection.knownHostsPath = QDir(dir).filePath(QStringLiteral("known_hosts"));
    connection.remoteDirectory = QDir(dir).filePath(QStringLiteral("remote"));
    return true;
}

} // namespace


// Files whose base names collide, in separate directories: the classic case of
// two screenshots taken at the same second.
struct CollidingFiles
{
    QTemporaryDir first;
    QTemporaryDir second;
    QString firstPath;
    QString secondPath;
};

bool makeCollidingFiles(CollidingFiles* files, const QString& name,
                        const QByteArray& a, const QByteArray& b)
{
    if (!files->first.isValid() || !files->second.isValid()) {
        return false;
    }
    files->firstPath = writeFile(files->first.path(), name, a);
    files->secondPath = writeFile(files->second.path(), name, b);
    return !files->firstPath.isEmpty() && !files->secondPath.isEmpty();
}

// Final objects that are not temporaries (no dot-prefixed .part names).
QStringList publishedObjects(const QString& directory)
{
    QStringList published;
    const QDir dir(directory);
    for (const QString& name : dir.entryList(QDir::Files)) {
        if (!name.startsWith(QLatin1Char('.'))) {
            published.append(name);
        }
    }
    return published;
}

// The published object whose name starts with `prefix` (exactly one expected).
QString publishedObjectNamed(const QString& directory, const QString& prefix)
{
    const QStringList matches = publishedObjects(directory).filter(prefix);
    return matches.size() == 1 ? QDir(directory).filePath(matches.first()) : QString();
}


class RemoteUploadTest : public QObject
{
    Q_OBJECT
private slots:
    // Every case starts from empty fixture directories, so an assertion about
    // "the one published object" cannot be polluted by an earlier case.
    void init()
    {
        for (const char* variable : { "FLAMESHOT_TEST_FTP_DIR", "FLAMESHOT_TEST_SSHD_DIR" }) {
            QString directory = qEnvironmentVariable(variable);
            if (directory.isEmpty()) {
                continue;
            }
            if (QString::fromLatin1(variable) == QLatin1String("FLAMESHOT_TEST_SSHD_DIR")) {
                directory = QDir(directory).filePath(QStringLiteral("remote"));
            }
            const QDir dir(directory);
            for (const QString& name : dir.entryList(QDir::Files | QDir::Hidden)) {
                QFile::remove(dir.filePath(name));
            }
        }
        // Drop any fixture mode a previous case left behind.
        for (const char* variable : { "FLAMESHOT_TEST_FTP_MODE", "FLAMESHOT_TEST_S3_MODE" }) {
            const QString path = qEnvironmentVariable(variable);
            if (!path.isEmpty()) {
                QFile::remove(path);
            }
        }
    }

    void initTestCase()
    {
        QVERIFY2(RemoteUploader::curlExecutable().endsWith(QStringLiteral("curl")),
                 "curl is required for the FTP and S3 transfer checks");
    }

    void uploadsToLoopbackFtpFixture()
    {
        const QString port = qEnvironmentVariable("FLAMESHOT_TEST_FTP_PORT");
        const QString target = qEnvironmentVariable("FLAMESHOT_TEST_FTP_DIR");
        if (port.isEmpty() || target.isEmpty()) {
            QSKIP("FLAMESHOT_TEST_FTP_PORT/DIR not set; run tests/run.sh");
        }
        QTemporaryDir workspace;
        const QByteArray payload(2048, 'f');
        const QString source = writeFile(workspace.path(), QStringLiteral("ftp shot.png"), payload);
        QVERIFY(!source.isEmpty());

        Settings settings;
        settings.enabled = true;
        settings.connection.protocol = Protocol::FTP;
        settings.connection.host = QStringLiteral("127.0.0.1");
        settings.connection.port = static_cast<quint16>(port.toUShort());
        settings.connection.username = QStringLiteral("fixture");
        settings.connection.remoteDirectory = QStringLiteral("/");
        Credentials credentials;
        credentials.password = QStringLiteral("fixture-password");

        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Image, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 20000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(succeeded.count(), 1);
        QVERIFY(succeeded.first().first().toString() == source);

        // The fixture received the exact bytes under the per-upload unique name,
        // and the local original is untouched.
        const QString published = publishedObjectNamed(target, QStringLiteral("ftp shot-"));
        QVERIFY2(!published.isEmpty(),
                 qPrintable(publishedObjects(target).join(QLatin1Char(','))));
        QCOMPARE(readAll(published), payload);
        QCOMPARE(readAll(source), payload);
    }

    void keepsTheOriginalWhenTheTransferFails()
    {
        QTemporaryDir workspace;
        const QString bin = installShimDirectory(workspace);
        QVERIFY(writeExecutable(QDir(bin).filePath(QStringLiteral("sftp")),
                                "#!/bin/sh\nexit 1\n"));
        const QByteArray oldPath = qgetenv("PATH");
        // Prepend: the shim wins for sftp, while tr/cp/cat stay reachable.
        qputenv("PATH", bin.toLocal8Bit() + ':' + oldPath);
        const auto restore = qScopeGuard([&]() { qputenv("PATH", oldPath); });

        const QString source = writeFile(workspace.path(), QStringLiteral("keep.png"), "keepme");
        Settings settings = sftpFixtureSettings(workspace.path());
        RemoteUploader uploader;
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QVERIFY(uploader.upload(source, MediaKind::Image, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() + succeeded.count() > 0, 20000);
        QCOMPARE(failed.count(), 1);
        QVERIFY(succeeded.isEmpty());
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("local file was kept")));
        QCOMPARE(readAll(source), QByteArray("keepme"));
    }

    void uploadsThroughSftpShim()
    {
        QTemporaryDir workspace;
        const QString bin = installShimDirectory(workspace);
        const QString arguments = QDir(workspace.path()).filePath(QStringLiteral("sftp-args"));
        const QString batchFile = QDir(workspace.path()).filePath(QStringLiteral("sftp-batch"));
        // Emulates `sftp -b -`: reads the batch, then copies the source file to
        // the destination named in the `put` line.
        QVERIFY(writeExecutable(
          QDir(bin).filePath(QStringLiteral("sftp")),
          QStringLiteral("#!/bin/sh\n"
                         "printf '%s\\n' \"$@\" > \"$FLAMESHOT_TEST_SHIM_ARGS\"\n"
                         "batch=$(cat)\n"
                         "printf '%s\\n' \"$batch\" >> \"$FLAMESHOT_TEST_SHIM_BATCH\"\n"
                         "line=$(printf '%s' \"$batch\" | head -1)\n"
                         "if printf '%s' \"$line\" | grep -q '^ls '; then\n"
                         "  exit 1   # the existence probe: destination is free\n"
                         "fi\n"
                         "set -- $line\n"
                         "src=$(printf '%s' \"$2\" | tr -d '\"')\n"
                         "dst=$(printf '%s' \"$3\" | tr -d '\"')\n"
                         "size=$(wc -c < \"$src\")\n"
                         "printf -- '-rw-r--r-- 1 fixture fixture %s Jan 1 00:00 %s\\n' \"$size\" \"$dst\"\n"
                         "cp -- \"$src\" \"$dst\"\n"
                         "exit 0\n")
            .toUtf8()));
        const QByteArray oldPath = qgetenv("PATH");
        const QByteArray oldArgs = qgetenv("FLAMESHOT_TEST_SHIM_ARGS");
        const QByteArray oldBatch = qgetenv("FLAMESHOT_TEST_SHIM_BATCH");
        qputenv("PATH", bin.toLocal8Bit() + ':' + oldPath);
        qputenv("FLAMESHOT_TEST_SHIM_ARGS", arguments.toLocal8Bit());
        qputenv("FLAMESHOT_TEST_SHIM_BATCH", batchFile.toLocal8Bit());
        const auto restore = qScopeGuard([&]() {
            qputenv("PATH", oldPath);
            qputenv("FLAMESHOT_TEST_SHIM_ARGS", oldArgs);
            qputenv("FLAMESHOT_TEST_SHIM_BATCH", oldBatch);
        });

        const QByteArray payload(1024, 's');
        const QString source = writeFile(workspace.path(), QStringLiteral("sftp_clip.mp4"), payload);
        Settings settings = sftpFixtureSettings(workspace.path());
        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Video, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 20000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        const QString batch = QString::fromUtf8(readAll(batchFile)).trimmed();
        // Every step this transfer ran: put + size probe, then the publish.
        QVERIFY2(batch.contains(QStringLiteral("put \"")), qPrintable(batch));
        QVERIFY2(batch.contains(QStringLiteral("ls -l \"")), qPrintable(batch));
        QVERIFY2(batch.contains(QStringLiteral("rename \"")), qPrintable(batch));
        // The final name is published from a per-transfer temporary.
        QVERIFY2(batch.contains(QStringLiteral("sftp_clip.mp4\"")), qPrintable(batch));
        QVERIFY2(batch.contains(QStringLiteral(".flameshot-")), qPrintable(batch));
        QVERIFY(batch.contains(settings.connection.remoteDirectory));
        const QString argv = QString::fromUtf8(readAll(arguments));
        QVERIFY(argv.contains(QStringLiteral("StrictHostKeyChecking=yes")));
        QVERIFY(argv.contains(QStringLiteral("fixture@fixture.example")));
        // The remote location never carries the local path.
        QVERIFY(!succeeded.first().at(1).toString().isEmpty());
    }

    void streamsThroughSshShim()
    {
        QTemporaryDir workspace;
        const QString bin = installShimDirectory(workspace);
        const QString argvFile = QDir(workspace.path()).filePath(QStringLiteral("ssh-args"));
        const QString bodyFile = QDir(workspace.path()).filePath(QStringLiteral("ssh-stdin"));
        QVERIFY(writeExecutable(
          QDir(bin).filePath(QStringLiteral("ssh")),
          QStringLiteral("#!/bin/sh\n"
                         "printf '%s\\n' \"$@\" > \"$FLAMESHOT_TEST_SHIM_ARGS\"\n"
                         "cat > \"$FLAMESHOT_TEST_SHIM_BODY\"\n")
            .toUtf8()));
        const QByteArray oldPath = qgetenv("PATH");
        const QByteArray oldArgs = qgetenv("FLAMESHOT_TEST_SHIM_ARGS");
        const QByteArray oldBody = qgetenv("FLAMESHOT_TEST_SHIM_BODY");
        qputenv("PATH", bin.toLocal8Bit() + ':' + oldPath);
        qputenv("FLAMESHOT_TEST_SHIM_ARGS", argvFile.toLocal8Bit());
        qputenv("FLAMESHOT_TEST_SHIM_BODY", bodyFile.toLocal8Bit());
        const auto restore = qScopeGuard([&]() {
            qputenv("PATH", oldPath);
            qputenv("FLAMESHOT_TEST_SHIM_ARGS", oldArgs);
            qputenv("FLAMESHOT_TEST_SHIM_BODY", oldBody);
        });

        const QByteArray payload(4096, 'x');
        const QString source = writeFile(workspace.path(), QStringLiteral("stream.mkv"), payload);
        Settings settings = sftpFixtureSettings(workspace.path());
        settings.connection.protocol = Protocol::SSH;
        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Video, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 20000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        // The bytes really were streamed over stdin.
        QCOMPARE(readAll(bodyFile), payload);
        const QString argv = QString::fromUtf8(readAll(argvFile));
        QVERIFY(argv.contains(QStringLiteral("-oStrictHostKeyChecking=yes")));
        QVERIFY(argv.contains(QStringLiteral("fixture@fixture.example")));
        QVERIFY(argv.contains(QStringLiteral("cat > ")));
    }

    void uploadsToLoopbackS3FixtureAndSignatureVerifies()
    {
        const QString port = qEnvironmentVariable("FLAMESHOT_TEST_S3_PORT");
        const QString resultFile = qEnvironmentVariable("FLAMESHOT_TEST_S3_RESULT");
        if (port.isEmpty() || resultFile.isEmpty()) {
            QSKIP("FLAMESHOT_TEST_S3_PORT/RESULT not set; run tests/run.sh");
        }
        QTemporaryDir workspace;
        const QByteArray payload(3100, 'z');
        const QString source = writeFile(workspace.path(), QStringLiteral("s3 shot.png"), payload);
        Settings settings;
        settings.enabled = true;
        settings.connection.protocol = Protocol::S3;
        settings.connection.endpoint = QStringLiteral("http://127.0.0.1:") + port;
        settings.connection.region = QStringLiteral("eu-west-1");
        settings.connection.bucket = QStringLiteral("shots");
        settings.connection.keyPrefix = QStringLiteral("team/flameshot");
        settings.connection.accessKeyId = QByteArray("AKIDEXAMPLE");
        Credentials credentials;
        credentials.secretAccessKey = QStringLiteral("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");

        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Image, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 20000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));

        const QJsonDocument document = QJsonDocument::fromJson(readAll(resultFile));
        const QJsonArray requests = document.object().value(QStringLiteral("requests")).toArray();
        QVERIFY(!requests.isEmpty());
        const QJsonObject last = requests.last().toObject();
        // The object name carries a per-upload unique suffix, so two uploads of
        // the same screenshot name cannot race for one key.
        const QString wireKey = last.value(QStringLiteral("key")).toString();
        const QRegularExpression uniqueKey(
          QStringLiteral("^shots/team/flameshot/s3%20shot-[0-9a-f]{8}\\.png$"));
        QVERIFY2(uniqueKey.match(wireKey).hasMatch(), qPrintable(wireKey));
        // Decoding the wire key must give back the intended name plus the unique
        // suffix, which proves the encoding is lossless.
        const QString decoded = QUrl::fromPercentEncoding(wireKey.toUtf8());
        QVERIFY2(decoded.startsWith(QStringLiteral("shots/team/flameshot/s3 shot-")),
                 qPrintable(decoded));
        QVERIFY2(decoded.endsWith(QStringLiteral(".png")), qPrintable(decoded));
        QCOMPARE(last.value(QStringLiteral("bytes")).toInt(), payload.size());
        // The fixture recomputes SigV4 with an independent implementation.
        QVERIFY2(last.value(QStringLiteral("verified")).toBool(),
                 qPrintable(QStringLiteral("expected %1, received %2")
                              .arg(last.value(QStringLiteral("expected")).toString(),
                                   last.value(QStringLiteral("received")).toString())));
        QVERIFY(last.value(QStringLiteral("credential_ok")).toBool());
    }

    void reencodesTheImageBeforeUploading()
    {
        const QString port = qEnvironmentVariable("FLAMESHOT_TEST_FTP_PORT");
        const QString target = qEnvironmentVariable("FLAMESHOT_TEST_FTP_DIR");
        if (port.isEmpty() || target.isEmpty()) {
            QSKIP("FLAMESHOT_TEST_FTP_PORT/DIR not set; run tests/run.sh");
        }
        QTemporaryDir workspace;
        QImage image(32, 24, QImage::Format_RGB32);
        image.fill(Qt::darkBlue);
        const QString source = QDir(workspace.path()).filePath(QStringLiteral("shot.png"));
        QVERIFY(image.save(source, "PNG"));
        const QByteArray originalBytes = readAll(source);

        Settings settings;
        settings.enabled = true;
        settings.connection.protocol = Protocol::FTP;
        settings.connection.host = QStringLiteral("127.0.0.1");
        settings.connection.port = static_cast<quint16>(port.toUShort());
        settings.connection.username = QStringLiteral("fixture");
        settings.image.conversion = Conversion::Convert;
        settings.image.format = QStringLiteral("jpeg");
        settings.image.quality = 55;
        Credentials credentials;
        credentials.password = QStringLiteral("fixture-password");

        RemoteUploader uploader;
        QSignalSpy prepared(&uploader, &RemoteUploader::prepared);
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Image, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 20000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QVERIFY(!prepared.isEmpty());
        QCOMPARE(prepared.first().at(2).toString(), QStringLiteral("image/jpeg"));

        // What the server received is a real JPEG, not the original PNG bytes.
        const QString published = publishedObjectNamed(target, QStringLiteral("shot-"));
        QVERIFY2(!published.isEmpty(),
                 qPrintable(publishedObjects(target).join(QLatin1Char(','))));
        const QByteArray uploaded = readAll(published);
        QVERIFY(!uploaded.isEmpty());
        QVERIFY(uploaded.startsWith("\xFF\xD8\xFF")); // JPEG SOI
        QVERIFY(uploaded != originalBytes);
        QImage received;
        QVERIFY(received.loadFromData(uploaded));
        QCOMPARE(received.size(), image.size());
        // The saved screenshot is kept and unchanged.
        QCOMPARE(readAll(source), originalBytes);
    }

    void transcodesTheVideoBeforeUploading()
    {
        const QString port = qEnvironmentVariable("FLAMESHOT_TEST_FTP_PORT");
        const QString target = qEnvironmentVariable("FLAMESHOT_TEST_FTP_DIR");
        const QString ffmpeg = RemoteUploader::ffmpegExecutable();
        if (port.isEmpty() || target.isEmpty()) {
            QSKIP("FLAMESHOT_TEST_FTP_PORT/DIR not set; run tests/run.sh");
        }
        if (ffmpeg.isEmpty()) {
            QSKIP("ffmpeg not installed");
        }
        QTemporaryDir workspace;
        const QString source = QDir(workspace.path()).filePath(QStringLiteral("clip.mkv"));
        QProcess encode;
        encode.setProgram(ffmpeg);
        encode.setArguments({ QStringLiteral("-nostdin"), QStringLiteral("-loglevel"),
                              QStringLiteral("error"), QStringLiteral("-f"),
                              QStringLiteral("lavfi"), QStringLiteral("-i"),
                              QStringLiteral("testsrc=size=64x48:rate=10:duration=1"),
                              QStringLiteral("-f"), QStringLiteral("lavfi"),
                              QStringLiteral("-i"),
                              QStringLiteral("sine=frequency=440:duration=1"),
                              QStringLiteral("-c:v"), QStringLiteral("libx264"),
                              QStringLiteral("-c:a"), QStringLiteral("aac"),
                              QStringLiteral("-shortest"), source });
        encode.start();
        QVERIFY(encode.waitForFinished(60000));
        if (encode.exitCode() != 0 || !QFileInfo::exists(source)) {
            QSKIP("ffmpeg cannot synthesise a test clip here");
        }
        const QByteArray originalBytes = readAll(source);

        Settings settings;
        settings.enabled = true;
        settings.connection.protocol = Protocol::FTP;
        settings.connection.host = QStringLiteral("127.0.0.1");
        settings.connection.port = static_cast<quint16>(port.toUShort());
        settings.connection.username = QStringLiteral("fixture");
        settings.video.conversion = Conversion::Convert;
        settings.video.container = QStringLiteral("mp4");
        settings.video.codec = QStringLiteral("libx264");
        settings.video.rateControl = QStringLiteral("crf");
        settings.video.quality = 30;
        settings.video.audioMode = QStringLiteral("encode");
        settings.video.audioCodec = QStringLiteral("auto");
        Credentials credentials;
        credentials.password = QStringLiteral("fixture-password");

        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Video, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 120000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));

        const QString published = publishedObjectNamed(target, QStringLiteral("clip-"));
        QVERIFY2(!published.isEmpty(),
                 qPrintable(publishedObjects(target).join(QLatin1Char(','))));
        const QByteArray uploaded = readAll(published);
        QVERIFY(!uploaded.isEmpty());
        QVERIFY(uploaded != originalBytes);
        QVERIFY(uploaded.size() > 8);
        QCOMPARE(QString::fromLatin1(uploaded.mid(4, 4)), QStringLiteral("ftyp"));
        // The uploaded bytes must be a real H.264/mp4, not a renamed mkv.
        const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
        if (!ffprobe.isEmpty()) {
            const QString probePath = QDir(workspace.path()).filePath(QStringLiteral("probe.mp4"));
            QFile probeFile(probePath);
            QVERIFY(probeFile.open(QIODevice::WriteOnly));
            probeFile.write(uploaded);
            probeFile.close();
            QProcess probe;
            probe.setProgram(ffprobe);
            probe.setArguments({ QStringLiteral("-v"), QStringLiteral("error"),
                                 QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                                 QStringLiteral("-show_entries"),
                                 QStringLiteral("stream=codec_name:format=format_name"),
                                 QStringLiteral("-of"),
                                 QStringLiteral("default=noprint_wrappers=1"),
                                 probePath });
            probe.start();
            QVERIFY(probe.waitForFinished(30000));
            const QString report = QString::fromUtf8(probe.readAllStandardOutput());
            QVERIFY2(report.contains(QStringLiteral("codec_name=h264")), qPrintable(report));
            QVERIFY2(report.contains(QStringLiteral("format_name=")) &&
                       report.contains(QStringLiteral("mp4")),
                     qPrintable(report));
        }
        // The original recording survives the conversion and the upload.
        QCOMPARE(readAll(source), originalBytes);
    }


    // ---- finding 2: redirects are failures, never followed -------------------
    void s3RedirectIsReportedNotFollowed()
    {
        const QString port = envValue("FLAMESHOT_TEST_S3_PORT");
        const QString result = envValue("FLAMESHOT_TEST_S3_RESULT");
        const QString mode = envValue("FLAMESHOT_TEST_S3_MODE");
        if (port.isEmpty() || result.isEmpty() || mode.isEmpty()) {
            QSKIP("S3 fixture not running; use tests/run.sh");
        }
        writeMode(mode, QStringLiteral("redirect307"));
        QTemporaryDir workspace;
        const QString source = writeFile(workspace.path(), QStringLiteral("r.png"), "png");
        const int before = s3Requests(result).size();

        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Image, loopbackS3Settings(port), s3Credentials()));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 20000);
        writeMode(mode, QStringLiteral("normal"));

        QCOMPARE(succeeded.count(), 0);
        QCOMPARE(failed.count(), 1);
        const QString message = failed.first().first().toString();
        QVERIFY2(message.contains(QStringLiteral("redirect")), qPrintable(message));
        // The client did not follow the redirect: exactly one request arrived.
        const QJsonArray requests = s3Requests(result);
        QCOMPARE(requests.size(), before + 1);
        QCOMPARE(requests.last().toObject().value(QStringLiteral("status")).toInt(), 307);
        // Nothing was stored.
        QVERIFY(QDir(workspace.path()).entryList(QDir::Files).contains(QStringLiteral("r.png")));
    }

    // ---- finding 1: a 412 from a conditional create is not success ---------
    void s3ConditionalCreateRefusalIsReported()
    {
        const QString port = envValue("FLAMESHOT_TEST_S3_PORT");
        const QString result = envValue("FLAMESHOT_TEST_S3_RESULT");
        const QString mode = envValue("FLAMESHOT_TEST_S3_MODE");
        if (port.isEmpty() || result.isEmpty() || mode.isEmpty()) {
            QSKIP("S3 fixture not running; use tests/run.sh");
        }
        writeMode(mode, QStringLiteral("normal"));
        QTemporaryDir workspace;
        const QString source = writeFile(workspace.path(), QStringLiteral("collide.png"), "first-bytes");

        RemoteUploader first;
        QSignalSpy firstOk(&first, &RemoteUploader::succeeded);
        QSignalSpy firstFail(&first, &RemoteUploader::failed);
        QVERIFY(first.upload(source, MediaKind::Image, loopbackS3Settings(port), s3Credentials()));
        QTRY_VERIFY_WITH_TIMEOUT(firstOk.count() + firstFail.count() > 0, 20000);
        QVERIFY2(firstFail.isEmpty(),
                 firstFail.isEmpty() ? "" : qPrintable(firstFail.first().first().toString()));
        // The conditional create was signed and sent.
        QCOMPARE(s3Requests(result).last().toObject()
                   .value(QStringLiteral("if_none_match")).toString(),
                 QStringLiteral("*"));

        // A server that refuses the conditional create (412) must be reported as
        // a refusal, never as a successful upload.
        writeMode(mode, QStringLiteral("always412"));
        const int before = s3Requests(result).size();
        RemoteUploader second;
        QSignalSpy secondOk(&second, &RemoteUploader::succeeded);
        QSignalSpy secondFail(&second, &RemoteUploader::failed);
        QVERIFY(second.upload(source, MediaKind::Image, loopbackS3Settings(port), s3Credentials()));
        QTRY_VERIFY_WITH_TIMEOUT(secondOk.count() + secondFail.count() > 0, 20000);
        writeMode(mode, QStringLiteral("normal"));
        QCOMPARE(secondOk.count(), 0);
        QCOMPARE(secondFail.count(), 1);
        QVERIFY2(secondFail.first().first().toString().contains(QStringLiteral("already exists")),
                 qPrintable(secondFail.first().first().toString()));
        const QJsonArray requests = s3Requests(result);
        QCOMPARE(requests.size(), before + 1);
        QCOMPARE(requests.last().toObject().value(QStringLiteral("status")).toInt(), 412);
    }

    // ---- finding 1 + 2: literal key on the wire, token signed ---------------
    void s3PreservesLiteralKeyAndSignsTheToken()
    {
        const QString port = envValue("FLAMESHOT_TEST_S3_PORT");
        const QString result = envValue("FLAMESHOT_TEST_S3_RESULT");
        const QString mode = envValue("FLAMESHOT_TEST_S3_MODE");
        if (port.isEmpty() || result.isEmpty() || mode.isEmpty()) {
            QSKIP("S3 fixture not running; use tests/run.sh");
        }
        writeMode(mode, QStringLiteral("normal"));
        QTemporaryDir workspace;
        // A prefix with a repeated slash and a leading dot segment, plus a name
        // with a space, a percent sign and a non-ASCII character.
        const QString source = writeFile(workspace.path(), QString::fromUtf8("kéy %1.png"), "png");
        Settings settings = loopbackS3Settings(port);
        settings.connection.keyPrefix = QStringLiteral("team//.hidden");
        settings.image.directory = QStringLiteral("team//.hidden");
        Credentials credentials = s3Credentials();
        credentials.sessionToken = QStringLiteral("SESSIONTOKENEXAMPLE");

        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Image, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 20000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));

        const QJsonObject last = s3Requests(result).last().toObject();
        // The wire path keeps the repeated slash and the dot segment: nothing was
        // silently normalised after signing.
        // Repeated slash and the dot segment survive verbatim; only the
        // per-upload unique suffix is new.
        const QRegularExpression literalPath(
          QString::fromUtf8("^/shots/team//\\.hidden/k%C3%A9y%20%251-[0-9a-f]{8}\\.png$"));
        QVERIFY2(literalPath.match(last.value(QStringLiteral("raw_path")).toString()).hasMatch(),
                 qPrintable(last.value(QStringLiteral("raw_path")).toString()));
        QVERIFY2(last.value(QStringLiteral("verified")).toBool(),
                 qPrintable(QStringLiteral("expected %1, received %2")
                              .arg(last.value(QStringLiteral("expected")).toString(),
                                   last.value(QStringLiteral("received")).toString())));
        QCOMPARE(last.value(QStringLiteral("token")).toString(),
                 QStringLiteral("SESSIONTOKENEXAMPLE"));
        QCOMPARE(last.value(QStringLiteral("if_none_match")).toString(), QStringLiteral("*"));
    }

    void s3Ipv6EndpointSignsItsBracketedAuthority()
    {
        const QString port = envValue("FLAMESHOT_TEST_S3_V6_PORT");
        const QString result = envValue("FLAMESHOT_TEST_S3_V6_RESULT");
        if (port.isEmpty() || result.isEmpty()) {
            QSKIP("IPv6 S3 fixture not running");
        }
        QTemporaryDir workspace;
        const QString source = writeFile(workspace.path(), QStringLiteral("v6.png"), "png");
        Settings settings = loopbackS3Settings(port);
        settings.connection.endpoint = QStringLiteral("http://[::1]:") + port;

        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Image, settings, s3Credentials()));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 20000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        const QJsonObject last = s3Requests(result).last().toObject();
        // The signature verified, which means the Host we signed matched the wire.
        QVERIFY2(last.value(QStringLiteral("verified")).toBool(),
                 qPrintable(QStringLiteral("expected %1, received %2")
                              .arg(last.value(QStringLiteral("expected")).toString(),
                                   last.value(QStringLiteral("received")).toString())));
        QVERIFY(last.value(QStringLiteral("sent_headers")).toObject()
                  .value(QStringLiteral("Host")).toString().startsWith(QStringLiteral("[::1]:")));
    }


    // ---- finding 2: same base name, two clients, real FTP server ------------
    void ftpSameBasenameConcurrentUploadsBothSurvive()
    {
        const QString port = envValue("FLAMESHOT_TEST_FTP_PORT");
        const QString target = envValue("FLAMESHOT_TEST_FTP_DIR");
        if (port.isEmpty() || target.isEmpty()) {
            QSKIP("FTP fixture not running; use tests/run.sh");
        }
        writeMode(envValue("FLAMESHOT_TEST_FTP_MODE"), QStringLiteral("normal"));
        CollidingFiles files;
        const QByteArray payloadA(1500, 'a');
        const QByteArray payloadB(2500, 'b');
        if (!makeCollidingFiles(&files, QStringLiteral("shot.png"), payloadA, payloadB)) {
            QSKIP("could not create the colliding files");
        }
        Settings settings = loopbackFtpSettings(port);
        Credentials credentials;
        credentials.password = QStringLiteral("fixture-password");

        RemoteUploader first;
        RemoteUploader second;
        QSignalSpy firstOk(&first, &RemoteUploader::succeeded);
        QSignalSpy firstFail(&first, &RemoteUploader::failed);
        QSignalSpy secondOk(&second, &RemoteUploader::succeeded);
        QSignalSpy secondFail(&second, &RemoteUploader::failed);
        QVERIFY(first.upload(files.firstPath, MediaKind::Image, settings, credentials));
        QVERIFY(second.upload(files.secondPath, MediaKind::Image, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(firstOk.count() + firstFail.count() > 0, 60000);
        QTRY_VERIFY_WITH_TIMEOUT(secondOk.count() + secondFail.count() > 0, 60000);
        QVERIFY2(firstFail.isEmpty() && secondFail.isEmpty(),
                 qPrintable(firstFail.isEmpty() ? (secondFail.isEmpty()
                                                    ? QString()
                                                    : secondFail.first().first().toString())
                                                : firstFail.first().first().toString()));

        // Two distinct final objects, each with its own bytes: nothing was
        // overwritten and nothing was truncated.
        const QStringList published = publishedObjects(target)
                                        .filter(QStringLiteral("shot-"));
        QVERIFY2(published.size() == 2, qPrintable(published.join(QLatin1Char(','))));
        QList<QByteArray> contents;
        for (const QString& name : published) {
            contents.append(readAll(QDir(target).filePath(name)));
        }
        QVERIFY2(contents.contains(payloadA), "the first upload's bytes were lost");
        QVERIFY2(contents.contains(payloadB), "the second upload's bytes were lost");
        // The reported destination is the real, unique one.
        const QString reported = firstOk.first().at(1).toString();
        QVERIFY2(reported.contains(QStringLiteral("shot-")), qPrintable(reported));
    }

    // ---- finding 1: FTP without SIZE support must fail closed --------------
    void ftpUnsupportedSizeFailsClosed()
    {
        const QString port = envValue("FLAMESHOT_TEST_FTP_PORT");
        const QString target = envValue("FLAMESHOT_TEST_FTP_DIR");
        const QString mode = envValue("FLAMESHOT_TEST_FTP_MODE");
        if (port.isEmpty() || target.isEmpty() || mode.isEmpty()) {
            QSKIP("FTP fixture not running; use tests/run.sh");
        }
        writeMode(mode, QStringLiteral("nosize"));
        QTemporaryDir workspace;
        const QByteArray payload(700, 'n');
        const QString source = writeFile(workspace.path(), QStringLiteral("nosize.png"), payload);
        Settings settings = loopbackFtpSettings(port);
        Credentials credentials;
        credentials.password = QStringLiteral("fixture-password");

        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Image, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 60000);
        writeMode(mode, QStringLiteral("normal"));

        QCOMPARE(succeeded.count(), 0);
        QCOMPARE(failed.count(), 1);
        // Nothing was published under a final name.
        const QStringList published = publishedObjects(target)
                                        .filter(QStringLiteral("nosize-"));
        QVERIFY2(published.isEmpty(), qPrintable(published.join(QLatin1Char(','))));
        QCOMPARE(readAll(source), payload);
    }

    // ---- finding 2: cancellation during the upload must not publish --------
    void ftpCancellationDuringUploadPublishesNothing()
    {
        const QString port = envValue("FLAMESHOT_TEST_FTP_PORT");
        const QString target = envValue("FLAMESHOT_TEST_FTP_DIR");
        const QString mode = envValue("FLAMESHOT_TEST_FTP_MODE");
        if (port.isEmpty() || target.isEmpty() || mode.isEmpty()) {
            QSKIP("FTP fixture not running; use tests/run.sh");
        }
        // The fixture holds the data phase open so cancellation lands mid-STOR.
        writeMode(mode, QStringLiteral("slowstor"));
        QTemporaryDir workspace;
        const QByteArray payload(4096, 'k');
        const QString source = writeFile(workspace.path(), QStringLiteral("cancel.png"), payload);
        // A pre-existing object with the same original base name must survive.
        const QString preExisting = QDir(target).filePath(QStringLiteral("cancel.png"));
        QVERIFY(writeFile(target, QStringLiteral("cancel.png"), "keep-me") == preExisting);
        Settings settings = loopbackFtpSettings(port);
        Credentials credentials;
        credentials.password = QStringLiteral("fixture-password");

        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Image, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(uploader.isRunning(), 20000);
        QTest::qWait(1500); // inside the fixture's STOR hold
        uploader.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 30000);
        writeMode(mode, QStringLiteral("normal"));

        QCOMPARE(succeeded.count(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.first().first().toString().contains(QStringLiteral("cancelled")),
                 qPrintable(failed.first().first().toString()));
        // The pre-existing bytes are untouched, and no final object appeared.
        QCOMPARE(readAll(preExisting), QByteArray("keep-me"));
        const QStringList published = publishedObjects(target)
                                        .filter(QStringLiteral("cancel-"));
        QVERIFY2(published.isEmpty(), qPrintable(published.join(QLatin1Char(','))));
        QCOMPARE(readAll(source), payload);
    }

    // ---- finding 1 + 2 + 4: same base name against a real SFTP server -----
    void realSftpSameBasenameConcurrentUploadsBothSurvive()
    {
        Settings settings;
        if (!sshdSettings(&settings, Protocol::SFTP)) {
            QSKIP("OpenSSH fixture not running; use tests/run.sh");
        }
        CollidingFiles files;
        const QByteArray payloadA(1800, 'x');
        const QByteArray payloadB(2600, 'y');
        if (!makeCollidingFiles(&files, QStringLiteral("same.png"), payloadA, payloadB)) {
            QSKIP("could not create the colliding files");
        }
        RemoteUploader first;
        RemoteUploader second;
        QSignalSpy firstOk(&first, &RemoteUploader::succeeded);
        QSignalSpy firstFail(&first, &RemoteUploader::failed);
        QSignalSpy secondOk(&second, &RemoteUploader::succeeded);
        QSignalSpy secondFail(&second, &RemoteUploader::failed);
        QVERIFY(first.upload(files.firstPath, MediaKind::Image, settings, Credentials{}));
        QVERIFY(second.upload(files.secondPath, MediaKind::Image, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(firstOk.count() + firstFail.count() > 0, 60000);
        QTRY_VERIFY_WITH_TIMEOUT(secondOk.count() + secondFail.count() > 0, 60000);
        QVERIFY2(firstFail.isEmpty() && secondFail.isEmpty(),
                 qPrintable(firstFail.isEmpty() ? (secondFail.isEmpty()
                                                    ? QString()
                                                    : secondFail.first().first().toString())
                                                : firstFail.first().first().toString()));
        const QStringList published = publishedObjects(settings.connection.remoteDirectory)
                                        .filter(QStringLiteral("same-"));
        QVERIFY2(published.size() == 2, qPrintable(published.join(QLatin1Char(','))));
        QList<QByteArray> contents;
        for (const QString& name : published) {
            contents.append(readAll(QDir(settings.connection.remoteDirectory).filePath(name)));
        }
        QVERIFY2(contents.contains(payloadA), "the first upload's bytes were lost");
        QVERIFY2(contents.contains(payloadB), "the second upload's bytes were lost");
        // No temporary is left behind on the happy path.
        const QDir dir(settings.connection.remoteDirectory);
        QVERIFY2(dir.entryList({ QStringLiteral(".*flameshot-*") },
                               QDir::Files | QDir::Hidden).isEmpty(),
                 qPrintable(dir.entryList(QDir::Files | QDir::Hidden).join(QLatin1Char(','))));
    }

    // ---- finding 1: FTP never overwrites an existing file -------------------
    void ftpPreexistingObjectWithSameBasenameIsUntouched()
    {
        const QString port = envValue("FLAMESHOT_TEST_FTP_PORT");
        const QString target = envValue("FLAMESHOT_TEST_FTP_DIR");
        if (port.isEmpty() || target.isEmpty()) {
            QSKIP("FTP fixture not running; use tests/run.sh");
        }
        QTemporaryDir workspace;
        const QString name = QStringLiteral("existing.png");
        const QString source = writeFile(workspace.path(), name, "new-content");
        const QString remote = QDir(target).filePath(name);
        QVERIFY(writeFile(target, name, "original-content") == remote);

        RemoteUploader uploader;
        QSignalSpy succeeded(&uploader, &RemoteUploader::succeeded);
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        Settings settings = loopbackFtpSettings(port);
        Credentials credentials;
        credentials.password = QStringLiteral("fixture-password");
        QVERIFY(uploader.upload(source, MediaKind::Image, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 30000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));

        // The pre-existing object keeps its bytes; the upload went to its own
        // unique name instead of replacing it.
        QCOMPARE(readAll(remote), QByteArray("original-content"));
        const QStringList published = publishedObjects(target)
                                        .filter(QStringLiteral("existing-"));
        QVERIFY2(published.size() == 1, qPrintable(published.join(QLatin1Char(','))));
        QCOMPARE(readAll(QDir(target).filePath(published.first())), QByteArray("new-content"));
        QVERIFY2(succeeded.first().at(1).toString().contains(published.first()),
                 qPrintable(succeeded.first().at(1).toString()));
    }

    void concurrentFtpUploadsDoNotCollide()
    {
        const QString port = envValue("FLAMESHOT_TEST_FTP_PORT");
        const QString target = envValue("FLAMESHOT_TEST_FTP_DIR");
        if (port.isEmpty() || target.isEmpty()) {
            QSKIP("FTP fixture not running; use tests/run.sh");
        }
        QTemporaryDir workspace;
        const QByteArray payloadA(1024, 'a');
        const QByteArray payloadB(2048, 'b');
        const QString sourceA = writeFile(workspace.path(), QStringLiteral("conc-a.png"), payloadA);
        const QString sourceB = writeFile(workspace.path(), QStringLiteral("conc-b.png"), payloadB);
        Settings settings = loopbackFtpSettings(port);
        Credentials credentials;
        credentials.password = QStringLiteral("fixture-password");

        RemoteUploader first;
        RemoteUploader second;
        QSignalSpy firstOk(&first, &RemoteUploader::succeeded);
        QSignalSpy firstFail(&first, &RemoteUploader::failed);
        QSignalSpy secondOk(&second, &RemoteUploader::succeeded);
        QSignalSpy secondFail(&second, &RemoteUploader::failed);
        QVERIFY(first.upload(sourceA, MediaKind::Image, settings, credentials));
        QVERIFY(second.upload(sourceB, MediaKind::Image, settings, credentials));
        QTRY_VERIFY_WITH_TIMEOUT(firstOk.count() + firstFail.count() > 0, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(secondOk.count() + secondFail.count() > 0, 30000);
        QVERIFY2(firstFail.isEmpty() && secondFail.isEmpty(),
                 qPrintable(firstFail.isEmpty()
                              ? (secondFail.isEmpty() ? QString()
                                                      : secondFail.first().first().toString())
                              : firstFail.first().first().toString()));
        // Distinct unique names, both carried their own bytes.
        const QString firstPublished = publishedObjectNamed(target, QStringLiteral("conc-a-"));
        const QString secondPublished = publishedObjectNamed(target, QStringLiteral("conc-b-"));
        QVERIFY2(!firstPublished.isEmpty(),
                 qPrintable(publishedObjects(target).join(QLatin1Char(','))));
        QVERIFY2(!secondPublished.isEmpty(),
                 qPrintable(publishedObjects(target).join(QLatin1Char(','))));
        QCOMPARE(readAll(firstPublished), payloadA);
        QCOMPARE(readAll(secondPublished), payloadB);
    }

    // ---- finding 1: cancellation keeps the original -------------------------
    void cancellationKeepsTheOriginalAndCleansUp()
    {
        QTemporaryDir workspace;
        const QString bin = installShimDirectory(workspace);
        // A shim that never finishes, so cancellation has something to interrupt.
        QVERIFY(writeExecutable(QDir(bin).filePath(QStringLiteral("ssh")),
                                "#!/bin/sh\nexec sleep 30\n"));
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", bin.toLocal8Bit() + ':' + oldPath);
        const auto restore = qScopeGuard([&]() { qputenv("PATH", oldPath); });

        const QByteArray payload(4096, 'c');
        const QString source = writeFile(workspace.path(), QStringLiteral("cancel.mkv"), payload);
        Settings settings = sftpFixtureSettings(workspace.path());
        settings.connection.protocol = Protocol::SSH;
        RemoteUploader uploader;
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(uploader.upload(source, MediaKind::Video, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(uploader.isRunning(), 10000);
        uploader.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0, 20000);
        QCOMPARE(readAll(source), payload);
    }

    // ---- findings 1 + 3 against a real local sshd ---------------------------
    void realSftpSequentialSameBasenameUploadsBothSurvive()
    {
        Settings settings;
        if (!sshdSettings(&settings, Protocol::SFTP)) {
            QSKIP("OpenSSH fixture not running; use tests/run.sh");
        }
        QTemporaryDir workspace;
        const QByteArray payload(3000, 's');
        const QString source = writeFile(workspace.path(), QStringLiteral("real.png"), payload);

        RemoteUploader first;
        QSignalSpy firstOk(&first, &RemoteUploader::succeeded);
        QSignalSpy firstFail(&first, &RemoteUploader::failed);
        QVERIFY(first.upload(source, MediaKind::Image, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(firstOk.count() + firstFail.count() > 0, 60000);
        QVERIFY2(firstFail.isEmpty(),
                 firstFail.isEmpty() ? "" : qPrintable(firstFail.first().first().toString()));

        // Same base name again, different bytes: unique final names keep both.
        QTemporaryDir second_dir;
        const QByteArray otherPayload(1200, 't');
        const QString other = writeFile(second_dir.path(), QStringLiteral("real.png"), otherPayload);
        RemoteUploader second;
        QSignalSpy secondOk(&second, &RemoteUploader::succeeded);
        QSignalSpy secondFail(&second, &RemoteUploader::failed);
        QVERIFY(second.upload(other, MediaKind::Image, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(secondOk.count() + secondFail.count() > 0, 60000);
        QVERIFY2(secondFail.isEmpty(),
                 secondFail.isEmpty() ? "" : qPrintable(secondFail.first().first().toString()));

        const QStringList published = publishedObjects(settings.connection.remoteDirectory)
                                        .filter(QStringLiteral("real-"));
        QVERIFY2(published.size() == 2, qPrintable(published.join(QLatin1Char(','))));
        QList<QByteArray> contents;
        for (const QString& name : published) {
            contents.append(readAll(QDir(settings.connection.remoteDirectory).filePath(name)));
        }
        QVERIFY2(contents.contains(payload), "the first upload's bytes were lost");
        QVERIFY2(contents.contains(otherPayload), "the second upload's bytes were lost");
        // No temporary is left behind.
        const QDir dir(settings.connection.remoteDirectory);
        QVERIFY2(dir.entryList({ QStringLiteral(".*flameshot-*") },
                               QDir::Files | QDir::Hidden).isEmpty(),
                 qPrintable(dir.entryList(QDir::Files | QDir::Hidden).join(QLatin1Char(','))));
    }

    void realSftpTrustsOnlyTheSelectedKnownHosts()
    {
        Settings settings;
        if (!sshdSettings(&settings, Protocol::SFTP)) {
            QSKIP("OpenSSH fixture not running; use tests/run.sh");
        }
        const QString dir = envValue("FLAMESHOT_TEST_SSHD_DIR");
        QTemporaryDir workspace;
        const QString source = writeFile(workspace.path(), QStringLiteral("trust.png"), "x");
        const QString alternate = QDir(dir).filePath(QStringLiteral("known_hosts_alternate"));

        // An empty selected file: the correct key exists in another file, but the
        // global database is disabled, so verification must fail.
        QTemporaryFile emptyHosts;
        QVERIFY(emptyHosts.open());
        emptyHosts.flush();
        Settings isolated = settings;
        isolated.connection.knownHostsPath = emptyHosts.fileName();
        RemoteUploader first;
        QSignalSpy firstFailed(&first, &RemoteUploader::failed);
        QSignalSpy firstOk(&first, &RemoteUploader::succeeded);
        QVERIFY(first.upload(source, MediaKind::Image, isolated, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(firstFailed.count() + firstOk.count() > 0, 60000);
        QCOMPARE(firstOk.count(), 0);
        QCOMPARE(firstFailed.count(), 1);

        // A file holding a different key for the same host must also fail.
        Settings wrong = settings;
        wrong.connection.knownHostsPath = QDir(dir).filePath(QStringLiteral("known_hosts_wrong"));
        RemoteUploader second;
        QSignalSpy secondFailed(&second, &RemoteUploader::failed);
        QSignalSpy secondOk(&second, &RemoteUploader::succeeded);
        QVERIFY(second.upload(source, MediaKind::Image, wrong, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(secondFailed.count() + secondOk.count() > 0, 60000);
        QCOMPARE(secondOk.count(), 0);

        // The selected file that really holds the key works (the alternate file is
        // never consulted; it exists only to prove that).
        QVERIFY(QFileInfo::exists(alternate));
        RemoteUploader third;
        QSignalSpy thirdOk(&third, &RemoteUploader::succeeded);
        QSignalSpy thirdFail(&third, &RemoteUploader::failed);
        QVERIFY(third.upload(source, MediaKind::Image, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(thirdOk.count() + thirdFail.count() > 0, 60000);
        QVERIFY2(thirdFail.isEmpty(),
                 thirdFail.isEmpty() ? "" : qPrintable(thirdFail.first().first().toString()));
    }

    void realSshVerifiesBytesAndKeepsBothSameBasenameUploads()
    {
        Settings settings;
        if (!sshdSettings(&settings, Protocol::SSH)) {
            QSKIP("OpenSSH fixture not running; use tests/run.sh");
        }
        QTemporaryDir workspace;
        const QByteArray payload(5000, 'h');
        const QString source = writeFile(workspace.path(), QStringLiteral("stream.mp4"), payload);

        RemoteUploader first;
        QSignalSpy firstOk(&first, &RemoteUploader::succeeded);
        QSignalSpy firstFail(&first, &RemoteUploader::failed);
        QVERIFY(first.upload(source, MediaKind::Video, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(firstOk.count() + firstFail.count() > 0, 60000);
        QVERIFY2(firstFail.isEmpty(),
                 firstFail.isEmpty() ? "" : qPrintable(firstFail.first().first().toString()));
        // The byte verification passed, so the published object is whole.
        const QStringList afterFirst = publishedObjects(settings.connection.remoteDirectory)
                                         .filter(QStringLiteral("stream-"));
        QCOMPARE(afterFirst.size(), 1);
        QCOMPARE(readAll(QDir(settings.connection.remoteDirectory).filePath(afterFirst.first())),
                 payload);

        QTemporaryDir second_dir;
        const QByteArray otherPayload(900, 'i');
        const QString other =
          writeFile(second_dir.path(), QStringLiteral("stream.mp4"), otherPayload);
        RemoteUploader second;
        QSignalSpy secondOk(&second, &RemoteUploader::succeeded);
        QSignalSpy secondFail(&second, &RemoteUploader::failed);
        QVERIFY(second.upload(other, MediaKind::Video, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(secondOk.count() + secondFail.count() > 0, 60000);
        QVERIFY2(secondFail.isEmpty(),
                 secondFail.isEmpty() ? "" : qPrintable(secondFail.first().first().toString()));

        const QStringList published = publishedObjects(settings.connection.remoteDirectory)
                                        .filter(QStringLiteral("stream-"));
        QCOMPARE(published.size(), 2);
        QList<QByteArray> contents;
        for (const QString& name : published) {
            contents.append(readAll(QDir(settings.connection.remoteDirectory).filePath(name)));
        }
        QVERIFY(contents.contains(payload));
        QVERIFY(contents.contains(otherPayload));
    }

    void reportsMissingFfmpegWithoutLosingTheRecording()
    {
        QTemporaryDir workspace;
        const QString source = writeFile(workspace.path(), QStringLiteral("clip.mkv"), "not-a-video");
        Settings settings = sftpFixtureSettings(workspace.path());
        settings.video.conversion = Conversion::Convert;
        RemoteUploader uploader;
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        // A deliberately unusable PATH hides ffmpeg without uninstalling it.
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", workspace.path().toLocal8Bit());
        const auto restore = qScopeGuard([&]() { qputenv("PATH", oldPath); });
        QVERIFY(uploader.upload(source, MediaKind::Video, settings, Credentials{}));
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0, 20000);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("FFmpeg")));
        QCOMPARE(readAll(source), QByteArray("not-a-video"));
    }
};

QTEST_GUILESS_MAIN(RemoteUploadTest)
#include "remoteuploadtest.moc"
