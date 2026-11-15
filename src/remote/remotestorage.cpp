// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/remotestorage.h"
#include "remote/remotestoragekeys.h"

#include <QFileInfo>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <algorithm>

namespace RemoteStorage {
namespace {

constexpr int kMigrationVersion = 1;

QString trimmed(const QString& value)
{
    return value.trimmed();
}

bool isOptionSafe(const QString& value)
{
    // Hosts, users and remote paths end up inside process arguments or a
    // remote command, so a leading '-' (an option in curl/ssh/sftp) and any
    // control character are refused outright.
    if (value.isEmpty() || value.startsWith(QLatin1Char('-'))) {
        return false;
    }
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f) {
            return false;
        }
    }
    return true;
}

QStringList splitLines(const QString& value)
{
    QStringList parts;
    for (const QString& part : value.split(QLatin1Char('/'))) {
        if (!part.isEmpty()) {
            parts.append(part);
        }
    }
    return parts;
}

void migrateValue(Store& store,
                  const QString& legacyKey,
                  const QString& targetKey,
                  const QString& fallback = {})
{
    if (store.contains(targetKey)) {
        return; // an explicit shared value always wins
    }
    const QString legacy = store.stringValue(legacyKey);
    if (legacy.isEmpty() && fallback.isEmpty()) {
        return;
    }
    store.setString(targetKey, legacy.isEmpty() ? fallback : legacy);
}

} // namespace

QString protocolName(Protocol protocol)
{
    switch (protocol) {
        case Protocol::FTP:
            return QStringLiteral("ftp");
        case Protocol::SFTP:
            return QStringLiteral("sftp");
        case Protocol::SSH:
            return QStringLiteral("ssh");
        case Protocol::S3:
            return QStringLiteral("s3");
    }
    return QStringLiteral("sftp");
}

Protocol protocolFromName(const QString& name)
{
    const QString key = name.trimmed().toLower();
    if (key == QLatin1String("ftp")) {
        return Protocol::FTP;
    }
    if (key == QLatin1String("ssh")) {
        return Protocol::SSH;
    }
    if (key == QLatin1String("s3")) {
        return Protocol::S3;
    }
    // Anything unknown (including the legacy default) stays on SFTP.
    return Protocol::SFTP;
}

QString secretSourceName(SecretSource source)
{
    return source == SecretSource::Environment ? QStringLiteral("environment")
                                               : QStringLiteral("prompt");
}

SecretSource secretSourceFromName(const QString& name)
{
    return name.trimmed().toLower() == QLatin1String("environment")
             ? SecretSource::Environment
             : SecretSource::Prompt;
}

QString conversionName(Conversion conversion)
{
    return conversion == Conversion::Convert ? QStringLiteral("convert")
                                             : QStringLiteral("original");
}

Conversion conversionFromName(const QString& name)
{
    return name.trimmed().toLower() == QLatin1String("convert")
             ? Conversion::Convert
             : Conversion::Original;
}

QStringList videoContainers()
{
    return { QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm") };
}

QStringList videoCodecs(const QString& container)
{
    const QString key = container.trimmed().toLower();
    if (key == QLatin1String("webm")) {
        return { QStringLiteral("libvpx-vp9") };
    }
    if (key == QLatin1String("mkv")) {
        return { QStringLiteral("libx264"), QStringLiteral("libvpx-vp9") };
    }
    if (key == QLatin1String("mp4")) {
        return { QStringLiteral("libx264") };
    }
    return {};
}

QStringList videoRateControls()
{
    return { QStringLiteral("crf"), QStringLiteral("abr") };
}

QStringList videoAudioModes()
{
    return { QStringLiteral("copy"), QStringLiteral("encode"), QStringLiteral("none") };
}

QStringList videoAudioCodecs(const QString& container)
{
    const QString key = container.trimmed().toLower();
    if (key == QLatin1String("webm")) {
        return { QStringLiteral("auto"), QStringLiteral("libopus") };
    }
    if (key == QLatin1String("mp4") || key == QLatin1String("mkv")) {
        return { QStringLiteral("auto"), QStringLiteral("aac"), QStringLiteral("libopus") };
    }
    return { QStringLiteral("auto") };
}

bool isSupportedVideoEncoder(const QString& container, const QString& codec)
{
    return videoCodecs(container).contains(codec.trimmed());
}

QString defaultVideoCodec(const QString& container)
{
    const QStringList codecs = videoCodecs(container);
    return codecs.isEmpty() ? QString() : codecs.first();
}

QString normalizedHost(const QString& host)
{
    QString value = trimmed(host);
    if (value.startsWith(QLatin1Char('[')) && value.endsWith(QLatin1Char(']'))) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

bool isLoopbackHost(const QString& host)
{
    const QString value = normalizedHost(host).toLower();
    if (value.isEmpty()) {
        return false;
    }
    if (value == QLatin1String("localhost") || value.endsWith(QLatin1String(".localhost"))) {
        return true;
    }
    QHostAddress dummy;
    if (!dummy.setAddress(value)) {
        return false;
    }
    return dummy.isLoopback();
}

bool parseEndpoint(const QString& endpoint,
                   QString* scheme,
                   QString* host,
                   QString* error)
{
    if (scheme) {
        scheme->clear();
    }
    if (host) {
        host->clear();
    }
    const QString value = trimmed(endpoint);
    if (value.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Enter an S3 endpoint, for example "
                                    "https://s3.us-east-1.amazonaws.com.");
        }
        return false;
    }
    const QUrl url(value);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        if (error) {
            *error = QStringLiteral("The S3 endpoint is not a valid URL.");
        }
        return false;
    }
    if (!url.query().isEmpty() || !url.fragment().isEmpty() || !url.path().isEmpty() ||
        url.path() == QLatin1String("/")) {
        if (!url.path().isEmpty() && url.path() != QLatin1String("/")) {
            if (error) {
                *error = QStringLiteral("The S3 endpoint must not contain a path; "
                                        "use the bucket field instead.");
            }
            return false;
        }
    }
    const QString endpointScheme = url.scheme().toLower();
    if (endpointScheme != QLatin1String("https") && endpointScheme != QLatin1String("http")) {
        if (error) {
            *error = QStringLiteral("The S3 endpoint must use https.");
        }
        return false;
    }
    if (endpointScheme == QLatin1String("http") && !isLoopbackHost(url.host())) {
        if (error) {
            *error = QStringLiteral("Plain http is only accepted for a loopback "
                                    "endpoint; use https for a remote host.");
        }
        return false;
    }
    if (scheme) {
        *scheme = endpointScheme;
    }
    if (host) {
        // An IPv6 literal must keep its brackets: this exact string is both the
        // request authority and the signed Host header, so a bare "::1:9000"
        // would be malformed and would sign a different Host than is sent.
        QString authority = url.host();
        if (authority.contains(QLatin1Char(':'))) {
            authority = QLatin1Char('[') + authority + QLatin1Char(']');
        }
        if (url.port() > 0) {
            authority += QLatin1Char(':') + QString::number(url.port());
        }
        *host = authority;
    }
    return true;
}

QList<QString> objectKeySegments(const QString& prefix, const QString& remoteName)
{
    // Keep every segment, including empty ones from repeated slashes, and never
    // resolve "." or "..": an S3 key is an opaque string, not a file path.
    QString normalised = prefix;
    while (normalised.startsWith(QLatin1Char('/'))) {
        normalised.remove(0, 1);
    }
    QList<QString> segments;
    if (!normalised.isEmpty()) {
        for (const QString& part : normalised.split(QLatin1Char('/'), Qt::KeepEmptyParts)) {
            segments.append(part);
        }
    }
    segments.append(remoteName);
    return segments;
}

bool supportsAtomicNoReplace(Protocol protocol)
{
    return protocol == Protocol::S3 || protocol == Protocol::SSH;
}

QualityRange videoQualityRange(const QString& container, const QString& codec)
{
    Q_UNUSED(container)
    const QString encoder = codec.trimmed().toLower();
    if (encoder.startsWith(QLatin1String("libvpx"))) {
        return { 0, 63, 23 };
    }
    if (encoder == QLatin1String("libx264") || encoder == QLatin1String("libx265")) {
        return { 0, 51, 23 };
    }
    return { 0, 63, 23 };
}

Settings load(const Store& store)
{
    Settings settings;
    settings.enabled = store.boolValue(QStringLiteral(REMOTE_STORAGE_ENABLED), false);

    Connection& connection = settings.connection;
    connection.protocol = protocolFromName(store.stringValue(QStringLiteral(REMOTE_STORAGE_PROTOCOL)));
    connection.host = store.stringValue(QStringLiteral(REMOTE_STORAGE_HOST));
    connection.port = static_cast<quint16>(
      std::clamp(store.intValue(QStringLiteral(REMOTE_STORAGE_PORT), 0), 0, 65535));
    connection.username = store.stringValue(QStringLiteral(REMOTE_STORAGE_USER));
    connection.privateKeyPath = store.stringValue(QStringLiteral(REMOTE_STORAGE_PRIVATE_KEY));
    connection.knownHostsPath = store.stringValue(QStringLiteral(REMOTE_STORAGE_KNOWN_HOSTS));
    connection.remoteDirectory = store.stringValue(QStringLiteral(REMOTE_STORAGE_REMOTE_DIR));
    if (connection.remoteDirectory.isEmpty()) {
        connection.remoteDirectory = QStringLiteral("/");
    }
    connection.acceptInsecureFtp =
      store.boolValue(QStringLiteral(REMOTE_STORAGE_ACCEPT_INSECURE_FTP), false);
    connection.secretSource =
      secretSourceFromName(store.stringValue(QStringLiteral(REMOTE_STORAGE_SECRET_SOURCE)));
    connection.endpoint = store.stringValue(QStringLiteral(REMOTE_STORAGE_S3_ENDPOINT));
    connection.region = store.stringValue(QStringLiteral(REMOTE_STORAGE_S3_REGION));
    if (connection.region.isEmpty()) {
        connection.region = QStringLiteral("us-east-1");
    }
    connection.bucket = store.stringValue(QStringLiteral(REMOTE_STORAGE_S3_BUCKET));
    connection.keyPrefix = store.stringValue(QStringLiteral(REMOTE_STORAGE_S3_KEY_PREFIX));
    connection.accessKeyId = store.stringValue(QStringLiteral(REMOTE_STORAGE_S3_ACCESS_KEY));

    ImageProfile& image = settings.image;
    image.conversion = conversionFromName(store.stringValue(QStringLiteral(REMOTE_STORAGE_IMAGE_MODE)));
    image.directory = store.stringValue(QStringLiteral(REMOTE_STORAGE_IMAGE_DIR));
    image.format = store.stringValue(QStringLiteral(REMOTE_STORAGE_IMAGE_FORMAT));
    image.quality = std::clamp(store.intValue(QStringLiteral(REMOTE_STORAGE_IMAGE_QUALITY), 90), 0, 100);
    image.compression =
      std::clamp(store.intValue(QStringLiteral(REMOTE_STORAGE_IMAGE_COMPRESSION), -1), -1, 9);

    VideoProfile& video = settings.video;
    video.conversion = conversionFromName(store.stringValue(QStringLiteral(REMOTE_STORAGE_VIDEO_MODE)));
    video.directory = store.stringValue(QStringLiteral(REMOTE_STORAGE_VIDEO_DIR));
    video.container = store.stringValue(QStringLiteral(REMOTE_STORAGE_VIDEO_CONTAINER));
    if (!videoContainers().contains(video.container)) {
        video.container = QStringLiteral("mp4");
    }
    video.codec = store.stringValue(QStringLiteral(REMOTE_STORAGE_VIDEO_CODEC));
    if (!isSupportedVideoEncoder(video.container, video.codec)) {
        video.codec = defaultVideoCodec(video.container);
    }
    video.rateControl = store.stringValue(QStringLiteral(REMOTE_STORAGE_VIDEO_RATE_CONTROL));
    if (!videoRateControls().contains(video.rateControl)) {
        video.rateControl = QStringLiteral("crf");
    }
    video.quality = std::clamp(store.intValue(QStringLiteral(REMOTE_STORAGE_VIDEO_QUALITY), 23), 0, 63);
    video.bitrateKbps = std::clamp(
      store.intValue(QStringLiteral(REMOTE_STORAGE_VIDEO_BITRATE), 4000), 100, 100000);
    video.audioMode = store.stringValue(QStringLiteral(REMOTE_STORAGE_VIDEO_AUDIO_MODE));
    if (!videoAudioModes().contains(video.audioMode)) {
        video.audioMode = QStringLiteral("copy");
    }
    video.audioCodec = store.stringValue(QStringLiteral(REMOTE_STORAGE_VIDEO_AUDIO_CODEC));
    if (!videoAudioCodecs(video.container).contains(video.audioCodec)) {
        video.audioCodec = QStringLiteral("auto");
    }
    video.audioBitrateKbps = std::clamp(
      store.intValue(QStringLiteral(REMOTE_STORAGE_VIDEO_AUDIO_BITRATE), 128), 32, 512);

    return settings;
}

void save(Store& store, const Settings& settings)
{
    const Connection& connection = settings.connection;
    store.setBool(QStringLiteral(REMOTE_STORAGE_ENABLED), settings.enabled);
    store.setString(QStringLiteral(REMOTE_STORAGE_PROTOCOL), protocolName(connection.protocol));
    store.setString(QStringLiteral(REMOTE_STORAGE_HOST), connection.host);
    store.setInt(QStringLiteral(REMOTE_STORAGE_PORT), connection.port);
    store.setString(QStringLiteral(REMOTE_STORAGE_USER), connection.username);
    store.setString(QStringLiteral(REMOTE_STORAGE_PRIVATE_KEY), connection.privateKeyPath);
    store.setString(QStringLiteral(REMOTE_STORAGE_KNOWN_HOSTS), connection.knownHostsPath);
    store.setString(QStringLiteral(REMOTE_STORAGE_REMOTE_DIR), connection.remoteDirectory);
    store.setBool(QStringLiteral(REMOTE_STORAGE_ACCEPT_INSECURE_FTP), connection.acceptInsecureFtp);
    store.setString(QStringLiteral(REMOTE_STORAGE_SECRET_SOURCE),
                    secretSourceName(connection.secretSource));
    store.setString(QStringLiteral(REMOTE_STORAGE_S3_ENDPOINT), connection.endpoint);
    store.setString(QStringLiteral(REMOTE_STORAGE_S3_REGION), connection.region);
    store.setString(QStringLiteral(REMOTE_STORAGE_S3_BUCKET), connection.bucket);
    store.setString(QStringLiteral(REMOTE_STORAGE_S3_KEY_PREFIX), connection.keyPrefix);
    store.setString(QStringLiteral(REMOTE_STORAGE_S3_ACCESS_KEY), connection.accessKeyId);

    store.setString(QStringLiteral(REMOTE_STORAGE_IMAGE_MODE), conversionName(settings.image.conversion));
    store.setString(QStringLiteral(REMOTE_STORAGE_IMAGE_DIR), settings.image.directory);
    store.setString(QStringLiteral(REMOTE_STORAGE_IMAGE_FORMAT), settings.image.format);
    store.setInt(QStringLiteral(REMOTE_STORAGE_IMAGE_QUALITY), settings.image.quality);
    store.setInt(QStringLiteral(REMOTE_STORAGE_IMAGE_COMPRESSION), settings.image.compression);

    store.setString(QStringLiteral(REMOTE_STORAGE_VIDEO_MODE), conversionName(settings.video.conversion));
    store.setString(QStringLiteral(REMOTE_STORAGE_VIDEO_DIR), settings.video.directory);
    store.setString(QStringLiteral(REMOTE_STORAGE_VIDEO_CONTAINER), settings.video.container);
    store.setString(QStringLiteral(REMOTE_STORAGE_VIDEO_CODEC), settings.video.codec);
    store.setString(QStringLiteral(REMOTE_STORAGE_VIDEO_RATE_CONTROL), settings.video.rateControl);
    store.setInt(QStringLiteral(REMOTE_STORAGE_VIDEO_QUALITY), settings.video.quality);
    store.setInt(QStringLiteral(REMOTE_STORAGE_VIDEO_BITRATE), settings.video.bitrateKbps);
    store.setString(QStringLiteral(REMOTE_STORAGE_VIDEO_AUDIO_MODE), settings.video.audioMode);
    store.setString(QStringLiteral(REMOTE_STORAGE_VIDEO_AUDIO_CODEC), settings.video.audioCodec);
    store.setInt(QStringLiteral(REMOTE_STORAGE_VIDEO_AUDIO_BITRATE), settings.video.audioBitrateKbps);
}

bool migrateLegacy(Store& store)
{
    const int version = store.intValue(QStringLiteral(REMOTE_STORAGE_MIGRATION_VERSION), 0);
    if (version >= kMigrationVersion) {
        return false;
    }

    // Legacy options carry the connection only; the media destinations keep
    // their defaults so an explicit local path is never repurposed as a remote
    // directory.
    migrateValue(store, QStringLiteral(LEGACY_TRANSFER_ENABLED),
                 QStringLiteral(REMOTE_STORAGE_ENABLED));
    migrateValue(store, QStringLiteral(LEGACY_TRANSFER_PROTOCOL),
                 QStringLiteral(REMOTE_STORAGE_PROTOCOL), QStringLiteral("sftp"));
    migrateValue(store, QStringLiteral(LEGACY_TRANSFER_HOST),
                 QStringLiteral(REMOTE_STORAGE_HOST));
    migrateValue(store, QStringLiteral(LEGACY_TRANSFER_USER),
                 QStringLiteral(REMOTE_STORAGE_USER));
    migrateValue(store, QStringLiteral(LEGACY_TRANSFER_REMOTE_DIR),
                 QStringLiteral(REMOTE_STORAGE_REMOTE_DIR), QStringLiteral("/"));
    migrateValue(store, QStringLiteral(LEGACY_TRANSFER_PRIVATE_KEY),
                 QStringLiteral(REMOTE_STORAGE_PRIVATE_KEY));
    migrateValue(store, QStringLiteral(LEGACY_TRANSFER_KNOWN_HOSTS),
                 QStringLiteral(REMOTE_STORAGE_KNOWN_HOSTS));

    const int legacyPort = store.intValue(QStringLiteral(LEGACY_TRANSFER_PORT), 0);
    if (legacyPort > 0 && !store.contains(QStringLiteral(REMOTE_STORAGE_PORT))) {
        store.setInt(QStringLiteral(REMOTE_STORAGE_PORT), legacyPort);
    }

    // FTP is unencrypted, so an existing FTP configuration has to acknowledge
    // that explicitly before uploads start working again.
    const bool legacyFtp = protocolFromName(store.stringValue(QStringLiteral(REMOTE_STORAGE_PROTOCOL))) ==
                           Protocol::FTP;
    if (legacyFtp && !store.contains(QStringLiteral(REMOTE_STORAGE_ACCEPT_INSECURE_FTP))) {
        store.setBool(QStringLiteral(REMOTE_STORAGE_ACCEPT_INSECURE_FTP), true);
    }

    store.setInt(QStringLiteral(REMOTE_STORAGE_MIGRATION_VERSION), kMigrationVersion);
    return true;
}

QString validate(const Settings& settings, MediaKind kind)
{
    const Connection& connection = settings.connection;

    if (connection.protocol == Protocol::S3) {
        QString scheme;
        QString host;
        QString error;
        if (!parseEndpoint(connection.endpoint, &scheme, &host, &error)) {
            return error;
        }
        if (!isOptionSafe(connection.bucket) || connection.bucket.contains(QLatin1Char('/'))) {
            return QStringLiteral("Enter a valid S3 bucket name.");
        }
        if (connection.region.isEmpty()) {
            return QStringLiteral("Enter the S3 region.");
        }
        if (connection.accessKeyId.trimmed().isEmpty()) {
            return QStringLiteral("Enter the S3 access key ID. The secret access key is "
                                  "requested at upload time and never stored.");
        }
        if (connection.keyPrefix.contains(QLatin1Char('\n')) ||
            connection.keyPrefix.contains(QLatin1Char('\r'))) {
            return QStringLiteral("The S3 key prefix cannot contain line breaks.");
        }
    } else {
        const QString host = normalizedHost(connection.host);
        // An IPv6 literal starts with ':' after the brackets are stripped, so
        // the first character may be one; everything else stays as before and
        // QUrl still rejects junk.
        static const QRegularExpression hostPattern(
          QStringLiteral("^[A-Za-z0-9:][A-Za-z0-9.:-]*$"));
        QUrl hostCheck;
        hostCheck.setScheme(QStringLiteral("sftp"));
        hostCheck.setHost(host);
        if (host.isEmpty() || !hostPattern.match(host).hasMatch() || !hostCheck.isValid() ||
            hostCheck.host().isEmpty()) {
            return QStringLiteral("Enter a valid server host.");
        }
        if (connection.protocol == Protocol::FTP) {
            if (connection.username.trimmed().isEmpty()) {
                return QStringLiteral("Enter an FTP username.");
            }
            if (!isLoopbackHost(host) && !connection.acceptInsecureFtp) {
                return QStringLiteral("FTP sends the password and the file unencrypted. "
                                      "Enable the plaintext FTP acknowledgement in "
                                      "Configuration > Remote Storage to use it.");
            }
        } else {
            static const QRegularExpression userPattern(
              QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.-]*$"));
            if (!userPattern.match(connection.username).hasMatch()) {
                return QStringLiteral("Enter a valid SSH user name.");
            }
            if (connection.privateKeyPath.trimmed().isEmpty()) {
                return QStringLiteral("Choose the private key file for %1.")
                  .arg(connection.protocol == Protocol::SFTP ? QStringLiteral("SFTP")
                                                             : QStringLiteral("SSH"));
            }
            if (!QFileInfo(connection.privateKeyPath).isReadable()) {
                return QStringLiteral("The private key file is not readable.");
            }
            if (connection.knownHostsPath.trimmed().isEmpty()) {
                return QStringLiteral("Choose a known_hosts file; host verification is "
                                      "always strict.");
            }
            if (!QFileInfo(connection.knownHostsPath).isReadable()) {
                return QStringLiteral("The known_hosts file is not readable.");
            }
        }
        if (connection.remoteDirectory.contains(QLatin1Char('\n')) ||
            connection.remoteDirectory.contains(QLatin1Char('\r'))) {
            return QStringLiteral("The remote directory cannot contain line breaks.");
        }
    }

    if (kind == MediaKind::Image) {
        if (settings.image.conversion == Conversion::Convert &&
            settings.image.format.trimmed().isEmpty()) {
            return QStringLiteral("Choose the image format used for the upload.");
        }
        if (settings.image.compression < -1 || settings.image.compression > 9) {
            return QStringLiteral("The image compression level must be between -1 and 9.");
        }
        return {};
    }

    const VideoProfile& video = settings.video;
    if (video.conversion == Conversion::Original) {
        return {};
    }
    if (!videoContainers().contains(video.container)) {
        return QStringLiteral("Choose a supported video container.");
    }
    if (!isSupportedVideoEncoder(video.container, video.codec)) {
        return QStringLiteral("The %1 encoder cannot be stored in a %2 file.")
          .arg(video.codec, video.container);
    }
    if (!videoRateControls().contains(video.rateControl)) {
        return QStringLiteral("Choose a supported video rate control.");
    }
    const QualityRange range = videoQualityRange(video.container, video.codec);
    if (video.rateControl == QLatin1String("crf") &&
        (video.quality < range.minimum || video.quality > range.maximum)) {
        return QStringLiteral("The %1 encoder accepts a quality between %2 and %3.")
          .arg(video.codec)
          .arg(range.minimum)
          .arg(range.maximum);
    }
    if (video.audioMode == QLatin1String("encode") &&
        !videoAudioCodecs(video.container).contains(video.audioCodec)) {
        return QStringLiteral("The selected audio encoder cannot be stored in a %1 file.")
          .arg(video.container);
    }
    if (video.audioMode != QLatin1String("copy") &&
        video.audioMode != QLatin1String("encode") &&
        video.audioMode != QLatin1String("none")) {
        return QStringLiteral("Choose how the audio track is handled.");
    }
    return {};
}

Credentials credentialsFromEnvironment(const Connection& connection, bool* found)
{
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    Credentials credentials;
    bool present = false;
    if (connection.protocol == Protocol::S3) {
        const QString secret = environment.value(
          QStringLiteral(REMOTE_STORAGE_ENV_S3_SECRET_KEY));
        if (!secret.isEmpty()) {
            credentials.secretAccessKey = secret;
            credentials.sessionToken = environment.value(
              QStringLiteral(REMOTE_STORAGE_ENV_S3_SESSION_TOKEN));
            present = true;
        }
    } else if (connection.protocol == Protocol::FTP) {
        const QString password = environment.value(
          QStringLiteral(REMOTE_STORAGE_ENV_FTP_PASSWORD));
        if (!password.isEmpty()) {
            credentials.password = password;
            present = true;
        }
    }
    if (found) {
        *found = present;
    }
    return credentials;
}

Credentials credentialsFromSecret(const Connection& connection, const QString& secret)
{
    Credentials credentials;
    const QString value = secret.trimmed();
    if (connection.protocol == Protocol::S3) {
        credentials.secretAccessKey = value;
        credentials.sessionToken = QProcessEnvironment::systemEnvironment().value(
          QStringLiteral(REMOTE_STORAGE_ENV_S3_SESSION_TOKEN));
    } else {
        credentials.password = secret;
    }
    return credentials;
}

}
