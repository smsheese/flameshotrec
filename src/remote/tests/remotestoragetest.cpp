// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

// Focused checks for remote storage: SigV4 canonicalisation and signing, the
// configuration model and its one-shot legacy migration, validation, transfer
// command construction (including hostile file names), FFmpeg transcode
// arguments, image re-encoding and remote file naming.
//
// Everything here is offline: no service is contacted and no real upload
// happens. The signature vectors were produced by an independent Python
// implementation of SigV4 (see tests/run.sh), not by echoing this code.

#include "remote/mediaexporter.h"
#include "remote/remotestorage.h"
#include "remote/remotestoragekeys.h"
#include "remote/remoteuploader.h"
#include "remote/s3signer.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace RemoteStorage;

namespace {

// In-memory Store: the model never needs the real configuration file.
class MemoryStore : public Store
{
public:
    QString stringValue(const QString& key) const override
    {
        return m_values.value(key).toString();
    }
    int intValue(const QString& key, int fallback) const override
    {
        const QVariant value = m_values.value(key);
        return value.isValid() ? value.toInt() : fallback;
    }
    bool boolValue(const QString& key, bool fallback) const override
    {
        const QVariant value = m_values.value(key);
        return value.isValid() ? value.toBool() : fallback;
    }
    bool contains(const QString& key) const override { return m_values.contains(key); }
    void setString(const QString& key, const QString& value) override
    {
        m_values.insert(key, value);
        m_written.insert(key);
    }
    void setInt(const QString& key, int value) override
    {
        m_values.insert(key, value);
        m_written.insert(key);
    }
    void setBool(const QString& key, bool value) override
    {
        m_values.insert(key, value);
        m_written.insert(key);
    }
    void seed(const QString& key, const QVariant& value) { m_values.insert(key, value); }
    QSet<QString> written() const { return m_written; }
    QHash<QString, QVariant> values() const { return m_values; }

private:
    QHash<QString, QVariant> m_values;
    QSet<QString> m_written;
};

QString writeFixtureFile(const QString& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = QDir(directory).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write(bytes);
    file.close();
    return path;
}

Settings sftpSettings(QTemporaryDir& workspace)
{
    Settings settings;
    settings.enabled = true;
    Connection& connection = settings.connection;
    connection.protocol = Protocol::SFTP;
    connection.host = QStringLiteral("server.example");
    connection.username = QStringLiteral("fixture");
    connection.privateKeyPath = writeFixtureFile(workspace.path(), "id_key", "key");
    connection.knownHostsPath = writeFixtureFile(workspace.path(), "known_hosts", "host");
    connection.remoteDirectory = QStringLiteral("/recordings");
    return settings;
}

} // namespace

class RemoteStorageTest : public QObject
{
    Q_OBJECT
private slots:
    // ---- SigV4 -------------------------------------------------------------
    void encodesUriPerRfc3986()
    {
        QCOMPARE(S3::uriEncode(QStringLiteral("aZ09-_.~")), QStringLiteral("aZ09-_.~"));
        QCOMPARE(S3::uriEncode(QStringLiteral("a b")), QStringLiteral("a%20b"));
        QCOMPARE(S3::uriEncode(QStringLiteral("a/b")), QStringLiteral("a%2Fb"));
        QCOMPARE(S3::uriEncode(QStringLiteral("a/b"), true), QStringLiteral("a/b"));
        QCOMPARE(S3::uriEncode(QString::fromUtf8("é")), QStringLiteral("%C3%A9"));
        // '*' and '!' are not unreserved and must be encoded.
        QCOMPARE(S3::uriEncode(QStringLiteral("a*b!c")), QStringLiteral("a%2Ab%21c"));
    }

    void buildsCanonicalUriWithoutNormalising()
    {
        QCOMPARE(S3::canonicalUri({}), QStringLiteral("/"));
        QCOMPARE(S3::canonicalUri({ QStringLiteral("bucket"), QStringLiteral("a b.png") }),
                 QStringLiteral("/bucket/a%20b.png"));
        // A hostile name is one segment; its slashes cannot escape the prefix.
        QCOMPARE(S3::canonicalUri({ QStringLiteral("b"), QStringLiteral("../x") }),
                 QStringLiteral("/b/..%2Fx"));
    }

    void implementsHmacSha256Correctly()
    {
        // RFC 4231-style vector: HMAC-SHA256("key", "The quick brown fox ...").
        QCOMPARE(QString::fromLatin1(S3::hmacSha256(
                   QByteArrayLiteral("key"),
                   QByteArrayLiteral("The quick brown fox jumps over the lazy dog")).toHex()),
                 QStringLiteral("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"));
        QCOMPARE(QString::fromLatin1(S3::sha256Hex(QByteArrayLiteral("abc"))),
                 QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }

    void matchesIndependentSignatureVector()
    {
        // Vector produced by tests/run.sh's Python implementation of SigV4 for:
        //   PUT https://s3.example.test:9000/shots/flameshot/screen shot 1.png
        //   content-type: image/png, payload "flameshot-s3-vector-fixture",
        //   region eu-west-1, 20260101T000000Z.
        S3::Request request;
        request.method = QByteArrayLiteral("PUT");
        request.host = QStringLiteral("s3.example.test:9000");
        request.pathSegments = { QStringLiteral("shots"), QStringLiteral("flameshot"),
                                 QStringLiteral("screen shot 1.png") };
        request.headers.append({ QByteArrayLiteral("content-type"),
                                 QByteArrayLiteral("image/png") });
        request.payloadHash = S3::sha256Hex(QByteArrayLiteral("flameshot-s3-vector-fixture"));

        S3::Credentials credentials;
        credentials.accessKeyId = QStringLiteral("AKIDEXAMPLE");
        credentials.secretAccessKey = QStringLiteral(
          "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");

        const QDateTime when(QDate(2026, 1, 1), QTime(0, 0, 0), QTimeZone::UTC);
        const S3::Signed result = S3::sign(request, credentials, QStringLiteral("eu-west-1"), when);

        QCOMPARE(QString::fromLatin1(result.scope), QStringLiteral("20260101/eu-west-1/s3/aws4_request"));
        QVERIFY(result.authorization.startsWith("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20260101/eu-west-1/s3/aws4_request, SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date, Signature="));
        QVERIFY2(QString::fromLatin1(result.signature) ==
                   QStringLiteral("dbf52a900cc984c2c5b7f315f40829abfefee56145b7bfb90872206573b3afd2"),
                 qPrintable(QStringLiteral("canonical request:\n%1").arg(QString::fromLatin1(result.canonicalRequest))));
    }

    void signsTheSessionTokenWhenPresent()
    {
        S3::Request request;
        request.host = QStringLiteral("s3.example.test:9000");
        request.pathSegments = { QStringLiteral("shots"), QStringLiteral("a.png") };
        request.payloadHash = S3::sha256Hex(QByteArrayLiteral("flameshot-s3-vector-fixture"));
        S3::Credentials credentials;
        credentials.accessKeyId = QStringLiteral("AKIDEXAMPLE");
        credentials.secretAccessKey = QStringLiteral("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
        credentials.sessionToken = QStringLiteral("SESSIONTOKENEXAMPLE");
        const QDateTime when(QDate(2026, 1, 1), QTime(0, 0, 0), QTimeZone::UTC);
        const S3::Signed result = S3::sign(request, credentials, QStringLiteral("eu-west-1"), when);
        QVERIFY(result.authorization.contains("x-amz-security-token"));
        QCOMPARE(QString::fromLatin1(result.signature),
                 QStringLiteral("9870702e4e9bb3a9514e299471a15842eef8b3e561ea0235bbb38a79b2baeed2"));
        bool advertised = false;
        for (const auto& header : result.headers) {
            if (header.first == QByteArrayLiteral("x-amz-security-token")) {
                advertised = header.second == QByteArrayLiteral("SESSIONTOKENEXAMPLE");
            }
        }
        QVERIFY(advertised);
    }

    void signatureDependsOnPayloadAndMethod()
    {
        S3::Credentials credentials;
        credentials.accessKeyId = QStringLiteral("AKIDEXAMPLE");
        credentials.secretAccessKey = QStringLiteral("secret");
        const QDateTime when(QDate(2026, 1, 1), QTime(0, 0, 0), QTimeZone::UTC);
        S3::Request request;
        request.host = QStringLiteral("h.example");
        request.pathSegments = { QStringLiteral("b"), QStringLiteral("k") };
        request.payloadHash = S3::sha256Hex(QByteArrayLiteral("one"));
        const S3::Signed first = S3::sign(request, credentials, QStringLiteral("r"), when);
        request.payloadHash = S3::sha256Hex(QByteArrayLiteral("two"));
        const S3::Signed second = S3::sign(request, credentials, QStringLiteral("r"), when);
        QVERIFY(first.signature != second.signature);
        request.method = QByteArrayLiteral("GET");
        const S3::Signed third = S3::sign(request, credentials, QStringLiteral("r"), when);
        QVERIFY(second.signature != third.signature);
    }

    // ---- model and migration ----------------------------------------------
    void migratesLegacyTransferOptionsOnce()
    {
        MemoryStore store;
        store.seed(QStringLiteral(LEGACY_TRANSFER_ENABLED), true);
        store.seed(QStringLiteral(LEGACY_TRANSFER_PROTOCOL), QStringLiteral("sftp"));
        store.seed(QStringLiteral(LEGACY_TRANSFER_HOST), QStringLiteral("nas.example"));
        store.seed(QStringLiteral(LEGACY_TRANSFER_PORT), 2222);
        store.seed(QStringLiteral(LEGACY_TRANSFER_USER), QStringLiteral("shots"));
        store.seed(QStringLiteral(LEGACY_TRANSFER_REMOTE_DIR), QStringLiteral("/incoming"));
        store.seed(QStringLiteral(LEGACY_TRANSFER_PRIVATE_KEY), QStringLiteral("/home/u/id"));
        store.seed(QStringLiteral(LEGACY_TRANSFER_KNOWN_HOSTS), QStringLiteral("/home/u/known"));

        QVERIFY(migrateLegacy(store));
        const Settings migrated = load(store);
        QVERIFY(migrated.enabled);
        QCOMPARE(migrated.connection.protocol, Protocol::SFTP);
        QCOMPARE(migrated.connection.host, QStringLiteral("nas.example"));
        QCOMPARE(migrated.connection.port, quint16(2222));
        QCOMPARE(migrated.connection.username, QStringLiteral("shots"));
        QCOMPARE(migrated.connection.remoteDirectory, QStringLiteral("/incoming"));
        QCOMPARE(migrated.connection.privateKeyPath, QStringLiteral("/home/u/id"));
        QCOMPARE(migrated.connection.knownHostsPath, QStringLiteral("/home/u/known"));

        // Legacy keys stay readable: nothing is dropped from the file.
        QCOMPARE(store.stringValue(QStringLiteral(LEGACY_TRANSFER_HOST)),
                 QStringLiteral("nas.example"));

        // Idempotent: a second run reports "nothing changed" and writes nothing.
        const QSet<QString> before = store.written();
        QVERIFY(!migrateLegacy(store));
        QCOMPARE(store.written(), before);
    }

    void migrationNeverOverwritesExplicitSharedValues()
    {
        MemoryStore store;
        store.seed(QStringLiteral(LEGACY_TRANSFER_HOST), QStringLiteral("legacy.example"));
        store.seed(QStringLiteral(REMOTE_STORAGE_HOST), QStringLiteral("explicit.example"));
        QVERIFY(migrateLegacy(store));
        QCOMPARE(load(store).connection.host, QStringLiteral("explicit.example"));
    }

    void migrationKeepsExplicitLocalPaths()
    {
        MemoryStore store;
        // The local save paths are a different feature; migration must not touch
        // or repurpose them.
        store.seed(QStringLiteral("savePath"), QStringLiteral("/home/u/Pictures/shots"));
        store.seed(QStringLiteral("recordingVideoPath"), QStringLiteral("/home/u/Videos/rec"));
        store.seed(QStringLiteral(LEGACY_TRANSFER_ENABLED), true);
        QVERIFY(migrateLegacy(store));
        QCOMPARE(store.stringValue(QStringLiteral("savePath")),
                 QStringLiteral("/home/u/Pictures/shots"));
        QCOMPARE(store.stringValue(QStringLiteral("recordingVideoPath")),
                 QStringLiteral("/home/u/Videos/rec"));
        for (const QString& key : store.written()) {
            QVERIFY2(!key.endsWith(QStringLiteral("Path")) || key.startsWith(QStringLiteral("remoteStorage")),
                     qPrintable(QStringLiteral("migration wrote a local path key: %1").arg(key)));
        }
    }

    void legacyFtpRequiresPlaintextAcknowledgement()
    {
        MemoryStore store;
        store.seed(QStringLiteral(LEGACY_TRANSFER_ENABLED), true);
        store.seed(QStringLiteral(LEGACY_TRANSFER_PROTOCOL), QStringLiteral("ftp"));
        store.seed(QStringLiteral(LEGACY_TRANSFER_HOST), QStringLiteral("ftp.example"));
        store.seed(QStringLiteral(LEGACY_TRANSFER_USER), QStringLiteral("u"));
        QVERIFY(migrateLegacy(store));
        QVERIFY(load(store).connection.acceptInsecureFtp);
    }

    void roundTripsAllSettings()
    {
        MemoryStore store;
        Settings settings;
        settings.enabled = true;
        settings.connection.protocol = Protocol::S3;
        settings.connection.endpoint = QStringLiteral("https://s3.example.test");
        settings.connection.region = QStringLiteral("eu-central-1");
        settings.connection.bucket = QStringLiteral("shots");
        settings.connection.keyPrefix = QStringLiteral("team/flameshot");
        settings.connection.accessKeyId = QStringLiteral("AKID");
        settings.connection.secretSource = SecretSource::Environment;
        settings.image.conversion = Conversion::Convert;
        settings.image.format = QStringLiteral("jpeg");
        settings.image.quality = 71;
        settings.image.compression = 3;
        settings.image.directory = QStringLiteral("images");
        settings.video.conversion = Conversion::Convert;
        settings.video.container = QStringLiteral("webm");
        settings.video.codec = QStringLiteral("libvpx-vp9");
        settings.video.rateControl = QStringLiteral("abr");
        settings.video.bitrateKbps = 2500;
        settings.video.audioMode = QStringLiteral("encode");
        settings.video.audioCodec = QStringLiteral("libopus");
        settings.video.audioBitrateKbps = 96;
        settings.video.directory = QStringLiteral("videos");
        save(store, settings);

        const Settings loaded = load(store);
        QVERIFY(loaded.enabled);
        QCOMPARE(loaded.connection.protocol, Protocol::S3);
        QCOMPARE(loaded.connection.endpoint, QStringLiteral("https://s3.example.test"));
        QCOMPARE(loaded.connection.region, QStringLiteral("eu-central-1"));
        QCOMPARE(loaded.connection.bucket, QStringLiteral("shots"));
        QCOMPARE(loaded.connection.keyPrefix, QStringLiteral("team/flameshot"));
        QCOMPARE(loaded.connection.secretSource, SecretSource::Environment);
        QCOMPARE(loaded.image.conversion, Conversion::Convert);
        QCOMPARE(loaded.image.format, QStringLiteral("jpeg"));
        QCOMPARE(loaded.image.quality, 71);
        QCOMPARE(loaded.image.compression, 3);
        QCOMPARE(loaded.video.container, QStringLiteral("webm"));
        QCOMPARE(loaded.video.codec, QStringLiteral("libvpx-vp9"));
        QCOMPARE(loaded.video.rateControl, QStringLiteral("abr"));
        QCOMPARE(loaded.video.bitrateKbps, 2500);
        QCOMPARE(loaded.video.audioMode, QStringLiteral("encode"));
        QCOMPARE(loaded.video.audioCodec, QStringLiteral("libopus"));
        QCOMPARE(loaded.video.audioBitrateKbps, 96);

        // No secret field is ever written.
        for (auto it = store.values().constBegin(); it != store.values().constEnd(); ++it) {
            QVERIFY2(!it.key().contains(QStringLiteral("Secret"), Qt::CaseInsensitive) ||
                       it.key() == QStringLiteral(REMOTE_STORAGE_SECRET_SOURCE),
                     qPrintable(it.key()));
        }
    }

    void defaultsPreserveOriginalMedia()
    {
        MemoryStore store;
        const Settings defaults = load(store);
        QCOMPARE(defaults.image.conversion, Conversion::Original);
        QCOMPARE(defaults.video.conversion, Conversion::Original);
        QVERIFY(!defaults.enabled);
        QCOMPARE(defaults.connection.protocol, Protocol::SFTP);
        QVERIFY(defaults.image.directory.isEmpty());
        QVERIFY(defaults.video.directory.isEmpty());
    }

    // ---- validation --------------------------------------------------------
    void requiresKnownHostsForSftpAndSsh()
    {
        for (const Protocol protocol : { Protocol::SFTP, Protocol::SSH }) {
            Settings settings;
            settings.connection.protocol = protocol;
            settings.connection.host = QStringLiteral("server.example");
            settings.connection.username = QStringLiteral("user");
            settings.connection.privateKeyPath = QStringLiteral("/nonexistent/key");
            QVERIFY(validate(settings, MediaKind::Video).contains(QStringLiteral("private key")));
        }
        QTemporaryDir workspace;
        Settings settings = sftpSettings(workspace);
        settings.connection.knownHostsPath.clear();
        QVERIFY(validate(settings, MediaKind::Video).contains(QStringLiteral("known_hosts")));
    }

    void refusesInsecureFtpUnlessAcknowledgedOrLoopback()
    {
        Settings settings;
        settings.connection.protocol = Protocol::FTP;
        settings.connection.host = QStringLiteral("ftp.example.test");
        settings.connection.username = QStringLiteral("u");
        QVERIFY(validate(settings, MediaKind::Image).contains(QStringLiteral("unencrypted")));
        settings.connection.acceptInsecureFtp = true;
        QVERIFY(validate(settings, MediaKind::Image).isEmpty());
        settings.connection.acceptInsecureFtp = false;
        settings.connection.host = QStringLiteral("127.0.0.1");
        QVERIFY(validate(settings, MediaKind::Image).isEmpty());
    }

    void refusesHostileHostAndBucket()
    {
        Settings settings;
        settings.connection.host = QStringLiteral("-oProxyCommand=touch /tmp/pwn");
        QVERIFY(!validate(settings, MediaKind::Image).isEmpty());
        settings.connection.host = QStringLiteral("server.example");
        settings.connection.protocol = Protocol::S3;
        settings.connection.endpoint = QStringLiteral("https://s3.example.test");
        settings.connection.bucket = QStringLiteral("../etc");
        QVERIFY(!validate(settings, MediaKind::Image).isEmpty());
    }

    void acceptsPlainHttpOnlyForLoopbackEndpoints()
    {
        QString scheme;
        QString host;
        QString error;
        QVERIFY(parseEndpoint(QStringLiteral("http://127.0.0.1:9000"), &scheme, &host, &error));
        QCOMPARE(scheme, QStringLiteral("http"));
        QCOMPARE(host, QStringLiteral("127.0.0.1:9000"));
        QVERIFY(!parseEndpoint(QStringLiteral("http://s3.example.test"), &scheme, &host, &error));
        QVERIFY(error.contains(QStringLiteral("loopback")));
        QVERIFY(parseEndpoint(QStringLiteral("https://s3.example.test"), &scheme, &host, &error));
        QVERIFY(!parseEndpoint(QStringLiteral("https://s3.example.test/bucket"), &scheme, &host, &error));
    }

    void rejectsImpossibleVideoCombinations()
    {
        // A loopback FTP connection is valid without extra files, so the video
        // checks are what the assertions below actually exercise.
        Settings settings;
        settings.connection.protocol = Protocol::FTP;
        settings.connection.host = QStringLiteral("127.0.0.1");
        settings.connection.username = QStringLiteral("fixture");
        settings.video.conversion = Conversion::Convert;
        settings.video.container = QStringLiteral("webm");
        settings.video.codec = QStringLiteral("libx264");
        QVERIFY(validate(settings, MediaKind::Video).contains(QStringLiteral("cannot be stored")));
        QVERIFY(!isSupportedVideoEncoder(QStringLiteral("webm"), QStringLiteral("libx264")));
        QVERIFY(isSupportedVideoEncoder(QStringLiteral("webm"), QStringLiteral("libvpx-vp9")));
        QCOMPARE(defaultVideoCodec(QStringLiteral("mp4")), QStringLiteral("libx264"));
    }

    // ---- command construction ---------------------------------------------
    void sftpPlanUploadsToATemporaryThenRenames()
    {
        QTemporaryDir workspace;
        Settings settings = sftpSettings(workspace);
        const QString artifact = writeFixtureFile(workspace.path(), "clip.mp4", "data");
        QString error;
        const QList<Command> plan = buildTransferPlan(
          settings, MediaKind::Video, artifact, QStringLiteral("clip-abcdef12.mp4"),
          QStringLiteral("video/mp4"), Credentials{}, QDateTime::currentDateTimeUtc(),
          QStringLiteral("abc123"), &error);
        QVERIFY2(plan.size() == 2, qPrintable(error));

        // No existence probe: a denied or unsupported listing cannot be mistaken
        // for "the destination is free", because the final name is unique.
        for (const Command& step : plan) {
            QVERIFY(!step.probeMustBeAbsent);
        }

        // 1. upload to the per-transfer temporary, then read its size field
        const QString putBatch = QString::fromUtf8(plan.at(0).stdinData);
        QVERIFY(putBatch.contains(
          QStringLiteral(".clip-abcdef12.mp4.flameshot-abc123.part")));
        QVERIFY(putBatch.contains(QStringLiteral("ls -l ")));
        QCOMPARE(plan.at(0).expectSizeInOutput, qint64(4));
        QCOMPARE(plan.at(0).sizeCheckKind, QByteArrayLiteral("sftp"));

        // 2. publish with rename to the unique final name
        const QString publishBatch = QString::fromUtf8(plan.at(1).stdinData);
        QVERIFY(publishBatch.startsWith(QStringLiteral("rename ")));
        QVERIFY(publishBatch.contains(QStringLiteral("clip-abcdef12.mp4")));

        // Host verification is anchored on the selected file only.
        for (const Command& step : plan) {
            QVERIFY(step.arguments.contains(QStringLiteral("-oStrictHostKeyChecking=yes")));
            QVERIFY(step.arguments.contains(QStringLiteral("-oGlobalKnownHostsFile=/dev/null")));
            QVERIFY(step.arguments.contains(QStringLiteral("none"))); // -F none
            bool knownHosts = false;
            for (const QString& argument : step.arguments) {
                if (argument.startsWith(QStringLiteral("-oUserKnownHostsFile="))) {
                    knownHosts = true;
                }
            }
            QVERIFY(knownHosts);
        }
    }

    void uniqueFinalNamesDifferButKeepTheExtension()
    {
        const QString first = uniqueRemoteFileName(QStringLiteral("shot.png"),
                                                   QStringLiteral("aaaabbbbccccdddd"));
        const QString second = uniqueRemoteFileName(QStringLiteral("shot.png"),
                                                    QStringLiteral("1111222233334444"));
        QVERIFY(first != second);
        QVERIFY2(first.startsWith(QStringLiteral("shot-")), qPrintable(first));
        QVERIFY2(first.endsWith(QStringLiteral(".png")), qPrintable(first));
        QCOMPARE(QFileInfo(first).suffix(), QStringLiteral("png"));
        // Same base name from two different directories no longer collides.
        QVERIFY(uniqueRemoteFileName(QStringLiteral("a/b/shot.png"), QStringLiteral("aaaa1111")) !=
                uniqueRemoteFileName(QStringLiteral("c/d/shot.png"), QStringLiteral("bbbb2222")));
        // No extension: still unique and readable.
        QVERIFY(uniqueRemoteFileName(QStringLiteral("capture"), QStringLiteral("abcd1234")) ==
                QStringLiteral("capture-abcd1234"));
    }

    void sftpTemporaryNameIsUniquePerTransfer()
    {
        const QString first = temporaryRemoteName(QStringLiteral("a.png"), QStringLiteral("aaaa"));
        const QString second = temporaryRemoteName(QStringLiteral("a.png"), QStringLiteral("bbbb"));
        QVERIFY(first != second);
        QVERIFY(first.startsWith(QLatin1Char('.')));
        QVERIFY(first.endsWith(QStringLiteral(".part")));
    }

    void sftpCleanupRemovesOnlyThisTransfersTemporary()
    {
        QTemporaryDir workspace;
        Settings settings = sftpSettings(workspace);
        Command cleanup;
        QVERIFY(buildCleanupCommand(settings, MediaKind::Video, QStringLiteral("clip.mp4"),
                                    QStringLiteral("abc123"), Credentials{}, &cleanup));
        const QString batch = QString::fromUtf8(cleanup.stdinData);
        QVERIFY(batch.startsWith(QStringLiteral("rm ")));
        QVERIFY(batch.contains(QStringLiteral(".clip.mp4.flameshot-abc123.part")));
        QVERIFY(!batch.contains(QLatin1String("clip.mp4\"\n"))); // never the final object
    }

    void quotesSftpBatchAgainstHostileNames()
    {
        QTemporaryDir workspace;
        Settings settings = sftpSettings(workspace);
        const QString artifact = writeFixtureFile(workspace.path(), "clip.mp4", "data");
        QString error;
        const QList<Command> plan = buildTransferPlan(
          settings, MediaKind::Video, artifact, QStringLiteral("a\"b\nc.mp4"), {},
          Credentials{}, QDateTime::currentDateTimeUtc(), QStringLiteral("id"), &error);
        QVERIFY2(!plan.isEmpty(), qPrintable(error));
        const QString batch = QString::fromUtf8(plan.at(0).stdinData);
        QVERIFY(batch.contains(QStringLiteral("\\\"")));
        // Two commands (put, ls) and the injected newline is neutralised, so the
        // hostile name cannot add a command.
        QCOMPARE(batch.count(QLatin1Char('\n')), 2);
    }

    void sshStreamsAndPublishesWithoutReplacing()
    {
        QTemporaryDir workspace;
        Settings settings = sftpSettings(workspace);
        settings.connection.protocol = Protocol::SSH;
        const QString artifact = writeFixtureFile(workspace.path(), "clip.mp4", "abcd");
        QString error;
        const QList<Command> plan = buildTransferPlan(
          settings, MediaKind::Video, artifact, QStringLiteral("clip.mp4"), {},
          Credentials{}, QDateTime::currentDateTimeUtc(), QStringLiteral("id9"), &error);
        QVERIFY2(plan.size() == 1, qPrintable(error));
        const Command& step = plan.at(0);
        QCOMPARE(step.stdinFile, artifact); // streamed, never a temporary local copy
        QCOMPARE(step.collisionExitCode, 3);
        QCOMPARE(step.interruptedExitCode, 4);
        const QString script = step.arguments.last();
        QVERIFY(script.contains(QStringLiteral("wc -c")));      // byte verification
        QVERIFY(script.contains(QStringLiteral("ln ")));        // link(2): no replace
        QVERIFY(script.contains(QStringLiteral("exit 3")));
        QVERIFY(script.contains(QStringLiteral("exit 4")));
        QVERIFY(script.contains(QStringLiteral(".flameshot-id9.part")));
        QVERIFY(script.contains(QStringLiteral("rm -f")));
        QVERIFY(step.arguments.contains(QStringLiteral("-F")));
        QVERIFY(step.arguments.contains(QStringLiteral("none")));
        QVERIFY(step.arguments.contains(QStringLiteral("-oGlobalKnownHostsFile=/dev/null")));
    }

    void sshRemoteCommandCannotBeInjected()
    {
        QTemporaryDir workspace;
        Settings settings = sftpSettings(workspace);
        settings.connection.protocol = Protocol::SSH;
        settings.connection.remoteDirectory = workspace.path();
        const QString artifact = writeFixtureFile(workspace.path(), "payload.bin", "streamed-bytes");
        const QString marker = QDir(workspace.path()).filePath(QStringLiteral("PWNED"));
        const QString hostile = QStringLiteral("x'; touch PWNED; echo '$(id)`.bin");

        QString error;
        const QList<Command> plan = buildTransferPlan(
          settings, MediaKind::Video, artifact, hostile, {}, Credentials{},
          QDateTime::currentDateTimeUtc(), QStringLiteral("inj"), &error);
        QVERIFY2(plan.size() == 1, qPrintable(error));
        const QString remoteCommand = plan.at(0).arguments.last();

        QProcess shell;
        shell.setProgram(QStringLiteral("/bin/sh"));
        shell.setArguments({ QStringLiteral("-c"), remoteCommand });
        shell.setWorkingDirectory(workspace.path());
        shell.setStandardInputFile(artifact);
        shell.start();
        QVERIFY(shell.waitForFinished(10000));
        QCOMPARE(shell.exitCode(), 0);
        QVERIFY2(!QFileInfo::exists(marker), "a hostile file name executed a command");
        const QString written = QDir(workspace.path()).filePath(hostile);
        QVERIFY2(QFileInfo::exists(written), qPrintable(written));
        QFile file(written);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("streamed-bytes"));
        QVERIFY(!QFileInfo::exists(written + QStringLiteral(".part")));
    }

    void sshRefusesToReplaceAnExistingFile()
    {
        // Same generated command, run locally, with the destination already taken:
        // it must exit 3 and leave the existing content alone.
        QTemporaryDir workspace;
        Settings settings = sftpSettings(workspace);
        settings.connection.protocol = Protocol::SSH;
        settings.connection.remoteDirectory = workspace.path();
        const QString artifact = writeFixtureFile(workspace.path(), "payload.bin", "new-bytes");
        const QString existing = writeFixtureFile(workspace.path(), "taken.bin", "original");
        QString error;
        const QList<Command> plan = buildTransferPlan(
          settings, MediaKind::Video, artifact, QStringLiteral("taken.bin"), {},
          Credentials{}, QDateTime::currentDateTimeUtc(), QStringLiteral("col"), &error);
        QVERIFY2(plan.size() == 1, qPrintable(error));

        QProcess shell;
        shell.setProgram(QStringLiteral("/bin/sh"));
        shell.setArguments({ QStringLiteral("-c"), plan.at(0).arguments.last() });
        shell.setWorkingDirectory(workspace.path());
        shell.setStandardInputFile(artifact);
        shell.start();
        QVERIFY(shell.waitForFinished(10000));
        QCOMPARE(shell.exitCode(), 3);
        QFile file(existing);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
        // The temporary file this transfer owned was cleaned up.
        const QDir dir(workspace.path());
        QVERIFY(dir.entryList({ QStringLiteral(".*flameshot-col.part") }, QDir::Files | QDir::Hidden).isEmpty());
    }

    void sshRefusesToPublishTruncatedInput()
    {
        // The size in the remote script is fixed at build time. Streaming a
        // shorter file (what an interrupted transfer looks like to the remote
        // side) must fail the byte check: nothing is published and the temporary
        // is removed.
        QTemporaryDir workspace;
        Settings settings = sftpSettings(workspace);
        settings.connection.protocol = Protocol::SSH;
        settings.connection.remoteDirectory = workspace.path();
        const QString artifact = writeFixtureFile(workspace.path(), "payload.bin", "twelve-bytes");
        QString error;
        const QList<Command> plan = buildTransferPlan(
          settings, MediaKind::Video, artifact, QStringLiteral("short.bin"), {},
          Credentials{}, QDateTime::currentDateTimeUtc(), QStringLiteral("trunc"), &error);
        QVERIFY2(plan.size() == 1, qPrintable(error));

        // Truncate the artifact after the plan was built: the script still
        // expects twelve bytes, the stream now delivers four.
        QFile shorter(artifact);
        QVERIFY(shorter.open(QIODevice::WriteOnly | QIODevice::Truncate));
        shorter.write("half");
        shorter.close();

        QProcess shell;
        shell.setProgram(QStringLiteral("/bin/sh"));
        shell.setArguments({ QStringLiteral("-c"), plan.at(0).arguments.last() });
        shell.setWorkingDirectory(workspace.path());
        shell.setStandardInputFile(artifact);
        shell.start();
        QVERIFY(shell.waitForFinished(10000));
        QCOMPARE(shell.exitCode(), 4);
        QVERIFY(!QFileInfo::exists(QDir(workspace.path()).filePath(QStringLiteral("short.bin"))));
        const QDir dir(workspace.path());
        QVERIFY(dir.entryList({ QStringLiteral(".*flameshot-trunc.part") },
                              QDir::Files | QDir::Hidden).isEmpty());
    }

    void ftpPlanUploadsToATemporaryThenRenamesAndVerifies()
    {
        QTemporaryDir workspace;
        Settings settings;
        settings.connection.protocol = Protocol::FTP;
        settings.connection.host = QStringLiteral("127.0.0.1");
        settings.connection.port = 2121;
        settings.connection.username = QStringLiteral("fixture");
        settings.connection.remoteDirectory = QStringLiteral("/incoming dir");
        const QString artifact = writeFixtureFile(workspace.path(), "shot.png", "png-bytes");
        Credentials credentials;
        credentials.password = QStringLiteral("s3cr3t-pw");
        QString error;
        const QList<Command> plan = buildTransferPlan(
          settings, MediaKind::Image, artifact, QStringLiteral("screen-abcdef12.png"),
          QStringLiteral("image/png"), credentials, QDateTime::currentDateTimeUtc(),
          QStringLiteral("f1"), &error);
        QVERIFY2(plan.size() == 4, qPrintable(error));

        // 1. STOR goes to the temporary, never to the final name.
        QVERIFY(!plan.at(0).probeMustBeAbsent);
        const QString uploadConfig = QString::fromUtf8(plan.at(0).stdinData);
        QVERIFY(uploadConfig.contains(QStringLiteral(".screen-abcdef12.png.flameshot-f1.part")));
        // The URL carries the temporary name, not the final one.
        QVERIFY(!uploadConfig.contains(QStringLiteral("screen-abcdef12.png") +
                                       QLatin1Char('"')));
        QCOMPARE(plan.at(0).uploadFile, artifact);

        // 2. the stored size is read back and must match before publishing
        QVERIFY(plan.at(1).arguments.contains(QStringLiteral("--head")));
        QCOMPARE(plan.at(1).expectSizeInOutput, qint64(9));
        QCOMPARE(plan.at(1).sizeCheckKind, QByteArrayLiteral("ftp"));

        // 3. publish by renaming the temporary; the exit status is only advisory
        //    because a follow-up stat decides whether publication really happened.
        QVERIFY(plan.at(2).advisoryExitStatus);
        const QStringList& renameArgs = plan.at(2).arguments;
        bool renamesTemp = false;
        bool renamesFinal = false;
        for (const QString& argument : renameArgs) {
            if (argument == QStringLiteral("RNFR .screen-abcdef12.png.flameshot-f1.part")) {
                renamesTemp = true;
            }
            if (argument == QStringLiteral("RNTO screen-abcdef12.png")) {
                renamesFinal = true;
            }
        }
        QVERIFY2(renamesTemp, qPrintable(renameArgs.join(QLatin1Char(' '))));
        QVERIFY(renamesFinal);

        // 4. the published object's size decides success
        QCOMPARE(plan.at(3).sizeCheckKind, QByteArrayLiteral("ftp"));
        QCOMPARE(plan.at(3).expectSizeInOutput, qint64(9));

        // The password never reaches an argument list.
        for (const Command& step : plan) {
            for (const QString& argument : step.arguments) {
                QVERIFY(!argument.contains(credentials.password));
            }
            QVERIFY(!QString::fromUtf8(step.stdinData).isEmpty());
        }
    }

    void parsesTheSftpSizeFieldNotAnyToken()
    {
        const QString path = QStringLiteral("/remote/.shot.png.flameshot-abc.part");
        // A correct record: size 1234.
        const QString good = QStringLiteral(
          "-rw-r--r--    1 fixture fixture     1234 Nov 15 10:00 ") + path;
        QCOMPARE(checkSftpListingSize(good, path, 1234), SizeCheck::Match);
        QCOMPARE(checkSftpListingSize(good, path, 1235), SizeCheck::Mismatch);

        // Misleading numbers elsewhere in the output must not satisfy the check:
        // a link count, an owner uid, a date and an echoed command that all
        // contain the expected size while the real size differs.
        const QString misleading = QStringLiteral(
          "sftp> put /tmp/x.png ") + path + QStringLiteral("\n") +
          QStringLiteral("-rw-r--r--    1 fixture fixture     4321 Nov 15 10:00 ") + path;
        QCOMPARE(checkSftpListingSize(misleading, path, 1), SizeCheck::Mismatch);
        QCOMPARE(checkSftpListingSize(misleading, path, 4321), SizeCheck::Match);
        QCOMPARE(checkSftpListingSize(misleading, path, 10), SizeCheck::Mismatch);
        QCOMPARE(checkSftpListingSize(misleading, path, 15), SizeCheck::Mismatch);

        // A record for a different file is not evidence for this path.
        const QString otherFile = QStringLiteral(
          "-rw-r--r--    1 fixture fixture     1234 Nov 15 10:00 /remote/other.png");
        QCOMPARE(checkSftpListingSize(otherFile, path, 1234), SizeCheck::Unparsable);

        // Nothing usable at all (denied listing, protocol error) is unparsable,
        // and must never be treated as a match.
        QCOMPARE(checkSftpListingSize(QStringLiteral("Permission denied"), path, 1234),
                 SizeCheck::Unparsable);
        QCOMPARE(checkSftpListingSize(QString(), path, 1234), SizeCheck::Unparsable);
    }

    void parsesTheFtpSizeReply()
    {
        // curl's FTP `--head` reports the SIZE answer as pseudo-headers; the raw
        // 213 spelling is accepted too. Anything else is "cannot verify".
        QCOMPARE(checkFtpSizeReply(QStringLiteral("Last-Modified: Thu, 01 Jan 2026 00:00:00 GMT\r\n"
                                                 "Content-Length: 1234\r\n"),
                                   1234),
                 SizeCheck::Match);
        QCOMPARE(checkFtpSizeReply(QStringLiteral("Content-Length: 1234"), 12), SizeCheck::Mismatch);
        QCOMPARE(checkFtpSizeReply(QStringLiteral("213 1234"), 1234), SizeCheck::Match);
        QCOMPARE(checkFtpSizeReply(QStringLiteral("213 1234\n"), 1234), SizeCheck::Match);
        QCOMPARE(checkFtpSizeReply(QStringLiteral("213 1234"), 99), SizeCheck::Mismatch);
        // A server that does not implement SIZE, or denies it, is unparsable:
        // the caller fails closed instead of publishing unverified bytes.
        QCOMPARE(checkFtpSizeReply(QStringLiteral("500 Unknown command"), 1234),
                 SizeCheck::Unparsable);
        QCOMPARE(checkFtpSizeReply(QStringLiteral("550 Permission denied"), 1234),
                 SizeCheck::Unparsable);
        QCOMPARE(checkFtpSizeReply(QString(), 1234), SizeCheck::Unparsable);
    }

    void s3UsesAConditionalCreateAndValidatesTheStatus()
    {
        QTemporaryDir workspace;
        Settings settings;
        settings.connection.protocol = Protocol::S3;
        settings.connection.endpoint = QStringLiteral("http://127.0.0.1:9000");
        settings.connection.region = QStringLiteral("eu-west-1");
        settings.connection.bucket = QStringLiteral("shots");
        settings.connection.keyPrefix = QStringLiteral("team//flameshot");
        settings.connection.accessKeyId = QStringLiteral("AKIDEXAMPLE");
        const QString artifact = writeFixtureFile(workspace.path(), "shot.png", "png-bytes");
        Credentials credentials;
        credentials.secretAccessKey = QStringLiteral("super-secret-key");
        QString error;
        const QList<Command> plan = buildTransferPlan(
          settings, MediaKind::Image, artifact, QStringLiteral("screen shot #1.png"),
          QStringLiteral("image/png"), credentials, QDateTime::currentDateTimeUtc(),
          QStringLiteral("s3"), &error);
        QVERIFY2(plan.size() == 1, qPrintable(error));
        const Command& step = plan.at(0);

        // No-replace: an explicit conditional create, signed as a header.
        QVERIFY(step.expectOkHttpStatus);
        QVERIFY(step.arguments.contains(QStringLiteral("--path-as-is")));
        const QString config = QString::fromUtf8(step.stdinData);
        QVERIFY(config.contains(QStringLiteral("If-None-Match: *")));
        QVERIFY(config.contains(QStringLiteral("if-none-match")));
        // Redirects are never followed.
        QVERIFY(!step.arguments.contains(QStringLiteral("-L")));
        QVERIFY(!step.arguments.contains(QStringLiteral("--location")));

        const int urlIndex = step.arguments.indexOf(QStringLiteral("--url"));
        const QString url = step.arguments.at(urlIndex + 1);
        // The repeated slash in the prefix is preserved: S3 keys are literal.
        QCOMPARE(url, QStringLiteral(
                        "http://127.0.0.1:9000/shots/team//flameshot/screen%20shot%20%231.png"));
        for (const QString& argument : step.arguments) {
            QVERIFY(!argument.contains(credentials.secretAccessKey));
        }
        QVERIFY(!config.contains(credentials.secretAccessKey));
    }

    void s3KeysAreNeverNormalised()
    {
        // Dot segments and repeated slashes are part of the key.
        QCOMPARE(objectKeySegments(QStringLiteral("a/./b//c/../d"), QStringLiteral("f.png")),
                 QList<QString>({ QStringLiteral("a"), QStringLiteral("."),
                                  QStringLiteral("b"), QString(), QStringLiteral("c"),
                                  QStringLiteral(".."), QStringLiteral("d"),
                                  QStringLiteral("f.png") }));
        QCOMPARE(objectKeySegments(QStringLiteral("/lead//x"), QStringLiteral("f")),
                 QList<QString>({ QStringLiteral("lead"), QString(), QStringLiteral("x"),
                                  QStringLiteral("f") }));
        QCOMPARE(objectKeySegments(QString(), QStringLiteral("f")),
                 QList<QString>({ QStringLiteral("f") }));
    }

    void reportsTransportGuaranteesHonestly()
    {
        // S3 and SSH can refuse atomically; SFTP and FTP must say they cannot.
        QVERIFY(supportsAtomicNoReplace(Protocol::S3));
        QVERIFY(supportsAtomicNoReplace(Protocol::SSH));
        QVERIFY(!supportsAtomicNoReplace(Protocol::SFTP));
        QVERIFY(!supportsAtomicNoReplace(Protocol::FTP));
        // The wording must stay honest: unique names avoid ordinary collisions,
        // but SFTP/FTP cannot promise an atomic no-replace publish, and cleanup
        // after a dropped connection is best effort only.
        const QString sftpText = noReplaceGuarantee(Protocol::SFTP);
        QVERIFY2(sftpText.contains(QStringLiteral("unique final name")), qPrintable(sftpText));
        QVERIFY2(sftpText.contains(QStringLiteral("best effort")), qPrintable(sftpText));
        const QString ftpText = noReplaceGuarantee(Protocol::FTP);
        QVERIFY2(ftpText.contains(QStringLiteral("no atomic rename")), qPrintable(ftpText));
        QVERIFY2(ftpText.contains(QStringLiteral("best effort")), qPrintable(ftpText));
        const QString sshText = noReplaceGuarantee(Protocol::SSH);
        QVERIFY2(sshText.contains(QStringLiteral("link(2)")), qPrintable(sshText));
        QVERIFY2(sshText.contains(QStringLiteral("best effort")), qPrintable(sshText));
        QVERIFY(noReplaceGuarantee(Protocol::S3).contains(QStringLiteral("If-None-Match")));
    }

    void buildsIpv6EndpointAuthorityWithBrackets()
    {
        QString scheme;
        QString host;
        QString error;
        QVERIFY(parseEndpoint(QStringLiteral("http://[::1]:9000"), &scheme, &host, &error));
        QCOMPARE(host, QStringLiteral("[::1]:9000"));
        QVERIFY(parseEndpoint(QStringLiteral("https://[2001:db8::1]"), &scheme, &host, &error));
        QCOMPARE(host, QStringLiteral("[2001:db8::1]"));
        QVERIFY(parseEndpoint(QStringLiteral("https://s3.example.test:8443"), &scheme, &host, &error));
        QCOMPARE(host, QStringLiteral("s3.example.test:8443"));
    }

    void signsTheIpv6AuthorityExactlyAsSent()
    {
        QTemporaryDir workspace;
        Settings settings;
        settings.connection.protocol = Protocol::S3;
        settings.connection.endpoint = QStringLiteral("http://[::1]:9000");
        settings.connection.region = QStringLiteral("eu-west-1");
        settings.connection.bucket = QStringLiteral("shots");
        settings.connection.accessKeyId = QStringLiteral("AKIDEXAMPLE");
        const QString artifact = writeFixtureFile(workspace.path(), "a.png", "png");
        Credentials credentials;
        credentials.secretAccessKey = QStringLiteral("secret");
        QString error;
        const QList<Command> plan = buildTransferPlan(
          settings, MediaKind::Image, artifact, QStringLiteral("a.png"), QStringLiteral("image/png"),
          credentials, QDateTime::currentDateTimeUtc(), QStringLiteral("v6"), &error);
        QVERIFY2(plan.size() == 1, qPrintable(error));
        const QString url = plan.at(0).arguments.at(
          plan.at(0).arguments.indexOf(QStringLiteral("--url")) + 1);
        QVERIFY(url.startsWith(QStringLiteral("http://[::1]:9000/shots/")));
    }

    void appliesCodecSpecificQualityLimits()
    {
        QCOMPARE(videoQualityRange(QStringLiteral("mp4"), QStringLiteral("libx264")).maximum, 51);
        QCOMPARE(videoQualityRange(QStringLiteral("webm"), QStringLiteral("libvpx-vp9")).maximum, 63);
        Settings settings;
        settings.connection.protocol = Protocol::FTP;
        settings.connection.host = QStringLiteral("127.0.0.1");
        settings.connection.username = QStringLiteral("u");
        settings.video.conversion = Conversion::Convert;
        settings.video.container = QStringLiteral("mp4");
        settings.video.codec = QStringLiteral("libx264");
        settings.video.rateControl = QStringLiteral("crf");
        settings.video.quality = 52;
        QVERIFY(validate(settings, MediaKind::Video).contains(QStringLiteral("between 0 and 51")));
        settings.video.quality = 51;
        QVERIFY(validate(settings, MediaKind::Video).isEmpty());
        // The same number is valid for VP9 but is not for x264.
        settings.video.container = QStringLiteral("webm");
        settings.video.codec = QStringLiteral("libvpx-vp9");
        settings.video.quality = 63;
        QVERIFY(validate(settings, MediaKind::Video).isEmpty());
        settings.video.quality = 64;
        QVERIFY(!validate(settings, MediaKind::Video).isEmpty());
    }

    void s3RequiresASecret()
    {
        QTemporaryDir workspace;
        Settings settings;
        settings.connection.protocol = Protocol::S3;
        settings.connection.endpoint = QStringLiteral("http://127.0.0.1:9000");
        settings.connection.bucket = QStringLiteral("shots");
        settings.connection.accessKeyId = QStringLiteral("AKID");
        const QString artifact = writeFixtureFile(workspace.path(), "shot.png", "png");
        RemoteUploader uploader;
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(!uploader.upload(artifact, MediaKind::Image, settings, Credentials{}));
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("secret access key")));
        QVERIFY(QFileInfo::exists(artifact));
    }

    // ---- conversion --------------------------------------------------------
    void transcodesWithRealEncoderFlags()
    {
        VideoProfile profile;
        profile.container = QStringLiteral("mp4");
        profile.codec = QStringLiteral("libx264");
        profile.rateControl = QStringLiteral("crf");
        profile.quality = 20;
        profile.audioMode = QStringLiteral("encode");
        profile.audioCodec = QStringLiteral("auto");
        profile.audioBitrateKbps = 128;
        QStringList arguments;
        QString error;
        QVERIFY(buildVideoArguments(profile, QStringLiteral("/in.mp4"), QStringLiteral("/out.mp4"),
                                    &arguments, &error));
        QVERIFY(arguments.contains(QStringLiteral("-c:v")));
        QVERIFY(arguments.contains(QStringLiteral("libx264")));
        QVERIFY(arguments.contains(QStringLiteral("-crf")));
        QVERIFY(arguments.contains(QStringLiteral("20")));
        QVERIFY(arguments.contains(QStringLiteral("aac")));
        QVERIFY(arguments.contains(QStringLiteral("+faststart")));
        QCOMPARE(arguments.last(), QStringLiteral("/out.mp4"));

        profile.container = QStringLiteral("webm");
        profile.codec = QStringLiteral("libvpx-vp9");
        profile.rateControl = QStringLiteral("crf");
        profile.audioMode = QStringLiteral("none");
        QVERIFY(buildVideoArguments(profile, QStringLiteral("/in.mp4"), QStringLiteral("/out.webm"),
                                    &arguments, &error));
        QVERIFY(arguments.contains(QStringLiteral("-an")));
        QVERIFY(arguments.contains(QStringLiteral("0"))); // vp9 constant quality target

        profile.container = QStringLiteral("webm");
        profile.codec = QStringLiteral("libx264");
        QVERIFY(!buildVideoArguments(profile, QStringLiteral("/in.mp4"),
                                     QStringLiteral("/out.webm"), &arguments, &error));
        QVERIFY(!error.isEmpty());
    }

    void reencodesImagesWithTheWriterForTheChosenFormat()
    {
        QTemporaryDir workspace;
        QImage image(8, 6, QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        const QString source = QDir(workspace.path()).filePath(QStringLiteral("shot.png"));
        QVERIFY(image.save(source, "PNG"));
        const auto originalSize = QFileInfo(source).size();

        QTemporaryDir output;
        ImageProfile profile;
        profile.conversion = Conversion::Convert;
        profile.format = QStringLiteral("jpeg");
        profile.quality = 40;
        ExportedMedia exported;
        QString error;
        QVERIFY2(exportImage(source, profile, &output, &exported, &error), qPrintable(error));
        QVERIFY(exported.converted);
        QCOMPARE(QFileInfo(exported.file).suffix(), QStringLiteral("jpg"));
        QCOMPARE(exported.remoteName, QStringLiteral("shot.jpg"));
        QCOMPARE(exported.mimeType, QStringLiteral("image/jpeg"));
        QImage roundTrip(exported.file);
        QVERIFY(!roundTrip.isNull());
        QCOMPARE(roundTrip.size(), image.size());
        // The original is untouched by the conversion.
        QCOMPARE(QFileInfo(source).size(), originalSize);
        QVERIFY(QFileInfo::exists(source));
    }

    void reportsImageWriterCapabilitiesInsteadOfGuessing()
    {
        const QList<ImageFormatInfo> formats = imageFormats();
        QVERIFY(!formats.isEmpty());
        const ImageFormatInfo png = imageFormat(QStringLiteral("png"));
        QCOMPARE(png.name, QStringLiteral("png"));
        QCOMPARE(png.mimeType, QStringLiteral("image/png"));
        // The widget enables only the controls the installed writer advertises.
        // Qt's PNG writer exposes both Quality and CompressionRatio, so the UI
        // mirrors that rather than inventing an encoder choice for a still image.
        QVERIFY(png.supportsQuality);
        QVERIFY(png.supportsCompression);
        const ImageFormatInfo jpeg = imageFormat(QStringLiteral("jpeg"));
        if (!jpeg.name.isEmpty()) {
            QVERIFY(jpeg.supportsQuality);
        }
        QVERIFY(imageFormat(QStringLiteral("definitely-not-a-writer")).name.isEmpty());
    }

    void refusesUnknownImageWriter()
    {
        QTemporaryDir workspace;
        QString source = writeFixtureFile(workspace.path(), "shot.png", "not-an-image");
        ImageProfile profile;
        profile.format = QStringLiteral("png");
        QTemporaryDir output;
        ExportedMedia exported;
        QString error;
        QVERIFY(!exportImage(source, profile, &output, &exported, &error));
        QVERIFY(!error.isEmpty());
    }

    void sanitisesRemoteFileNames()
    {
        QCOMPARE(remoteFileName(QStringLiteral("/a/b/screen shot.png")),
                 QStringLiteral("screen shot.png"));
        QCOMPARE(remoteFileName(QStringLiteral("../../etc/passwd")), QStringLiteral("passwd.bin"));
        QCOMPARE(remoteFileName(QStringLiteral("a\tb\nc.png")), QStringLiteral("a_b_c.png"));
        QCOMPARE(remoteFileName(QStringLiteral(".hidden.png")), QStringLiteral("hidden.png"));
        QCOMPARE(remoteFileName(QStringLiteral("clip.mkv"), QStringLiteral("mp4")),
                 QStringLiteral("clip.mp4"));
        const QString longName = QString(400, QLatin1Char('x')) + QStringLiteral(".png");
        QVERIFY(remoteFileName(longName).size() <= 121);
    }

    void reportsMimeTypesFromTheFileSuffix()
    {
        QCOMPARE(mimeTypeForFile(QStringLiteral("a.png")), QStringLiteral("image/png"));
        QCOMPARE(mimeTypeForFile(QStringLiteral("a.jpg")), QStringLiteral("image/jpeg"));
        QCOMPARE(mimeTypeForFile(QStringLiteral("a.mp4")), QStringLiteral("video/mp4"));
        QCOMPARE(mimeTypeForFile(QStringLiteral("a.webm")), QStringLiteral("video/webm"));
    }

    void failsWithoutTouchingTheOriginal()
    {
        QTemporaryDir workspace;
        const QString source = writeFixtureFile(workspace.path(), "shot.png", "png");
        Settings settings;
        settings.connection.protocol = Protocol::SFTP;
        settings.connection.host = QStringLiteral("server.example");
        settings.connection.username = QStringLiteral("fixture");
        settings.connection.privateKeyPath = writeFixtureFile(workspace.path(), "id_key", "k");
        // No known_hosts: the transfer must refuse before starting a process.
        RemoteUploader uploader;
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(!uploader.upload(source, MediaKind::Image, settings, Credentials{}));
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("known_hosts")));
        QVERIFY(QFileInfo::exists(source));
    }

    void refusesMissingLocalFile()
    {
        Settings settings;
        settings.connection.protocol = Protocol::FTP;
        settings.connection.host = QStringLiteral("127.0.0.1");
        settings.connection.username = QStringLiteral("u");
        RemoteUploader uploader;
        QSignalSpy failed(&uploader, &RemoteUploader::failed);
        QVERIFY(!uploader.upload(QStringLiteral("/definitely/not/here.png"), MediaKind::Image,
                                 settings, Credentials{}));
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("missing or unreadable")));
    }
};

QTEST_GUILESS_MAIN(RemoteStorageTest)
#include "remotestoragetest.moc"
