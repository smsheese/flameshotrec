// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "remote/s3signer.h"

#include <QList>
#include <QPair>
#include <QString>

class QProcessEnvironment;

namespace RemoteStorage {

enum class Protocol { FTP, SFTP, SSH, S3 };
enum class MediaKind { Image, Video };
// "Use original" keeps the saved file byte for byte. `Convert` asks for a real
// re-encode through the image writer or FFmpeg before the upload.
enum class Conversion { Original, Convert };
// Where the secrets come from. They are never persisted by this module.
enum class SecretSource { Prompt, Environment };

// Secrets resolved at upload time. Never written to the configuration file, the
// process argument list or the log.
struct Credentials
{
    QString password;        // FTP
    QString secretAccessKey; // S3
    QString sessionToken;    // S3, optional
};

// Transport settings shared by screenshots and recordings.
struct Connection
{
    Protocol protocol = Protocol::SFTP;
    QString host;
    quint16 port = 0;
    QString username;
    QString privateKeyPath; // SFTP and SSH
    QString knownHostsPath; // SFTP and SSH, required
    QString remoteDirectory; // FTP/SFTP/SSH destination directory
    bool acceptInsecureFtp = false;
    SecretSource secretSource = SecretSource::Prompt;

    // S3-compatible object storage.
    QString endpoint; // https://s3.example.com, or plain http for loopback only
    QString region = QStringLiteral("us-east-1");
    QString bucket;
    QString keyPrefix;
    QString accessKeyId;
};

// What the two media profiles have in common: whether to convert at all, and
// where the file goes. `directory` is a remote directory for FTP/SFTP/SSH and an
// object key prefix for S3; it is never a local path.
struct MediaProfile
{
    Conversion conversion = Conversion::Original;
    QString directory;
};

struct ImageProfile : MediaProfile
{
    QString format;         // empty keeps the source format
    int quality = 90;       // lossy writers only
    int compression = -1;   // -1 leaves the writer default
};

struct VideoProfile : MediaProfile
{
    QString container = QStringLiteral("mp4");
    QString codec = QStringLiteral("libx264");
    QString rateControl = QStringLiteral("crf");
    int quality = 23;
    int bitrateKbps = 4000;
    QString audioMode = QStringLiteral("copy"); // copy, encode, none
    QString audioCodec = QStringLiteral("auto");
    int audioBitrateKbps = 128;
};

struct Settings
{
    bool enabled = false;
    Connection connection;
    ImageProfile image;
    VideoProfile video;

    const MediaProfile& profile(MediaKind kind) const
    {
        return kind == MediaKind::Image ? static_cast<const MediaProfile&>(image) : video;
    }
};

// Minimal key/value store so the model can be exercised without a full
// ConfigHandler. The application uses ConfigHandlerStore; tests use a memory
// store.
class Store
{
public:
    virtual ~Store() = default;
    virtual QString stringValue(const QString& key) const = 0;
    virtual int intValue(const QString& key, int fallback) const = 0;
    virtual bool boolValue(const QString& key, bool fallback) const = 0;
    virtual bool contains(const QString& key) const = 0;
    virtual void setString(const QString& key, const QString& value) = 0;
    virtual void setInt(const QString& key, int value) = 0;
    virtual void setBool(const QString& key, bool value) = 0;
};

QString protocolName(Protocol protocol);
Protocol protocolFromName(const QString& name);
QString secretSourceName(SecretSource source);
SecretSource secretSourceFromName(const QString& name);
QString conversionName(Conversion conversion);
Conversion conversionFromName(const QString& name);

// Accepted containers and the encoders that can be muxed into them. The widget
// and the exporter use the same table so an impossible combination cannot be
// configured from the UI only.
QStringList videoContainers();
QStringList videoCodecs(const QString& container);
QStringList videoRateControls();
QStringList videoAudioModes();
QStringList videoAudioCodecs(const QString& container);

// Codec-specific constant-quality range. x264 accepts 0-51, libvpx 0-63; the
// UI and validate() both use this so an encoder cannot be handed a value it
// rejects.
struct QualityRange
{
    int minimum = 0;
    int maximum = 0;
    int recommended = 0;
};
QualityRange videoQualityRange(const QString& container, const QString& codec);

// Encoders that FFmpeg can be asked for; anything else is rejected before a
// process is started.
bool isSupportedVideoEncoder(const QString& container, const QString& codec);
QString defaultVideoCodec(const QString& container);

Settings load(const Store& store);
void save(Store& store, const Settings& settings);

// Copies the legacy `recordingTransfer*` options into the shared keys exactly
// once. Idempotent: the migration version marker stops a second run, and an
// explicit value already present in the shared key is never overwritten.
// Returns true when the stored settings changed.
bool migrateLegacy(Store& store);

// Validates everything an upload needs except the secrets. Returns an empty
// string when the settings are usable, otherwise a message safe to display.
QString validate(const Settings& settings, MediaKind kind);

// Rejects hostile input and returns a plain host name without brackets or port.
QString normalizedHost(const QString& host);

// True for loopback hosts, the only place an unencrypted FTP transfer or a
// plain-http S3 endpoint is accepted.
bool isLoopbackHost(const QString& host);

// Reads the secret for this connection from the environment. `found` reports
// whether the secret was actually present.
Credentials credentialsFromEnvironment(const Connection& connection, bool* found);

// Builds credentials from a prompted or otherwise supplied secret.
Credentials credentialsFromSecret(const Connection& connection, const QString& secret);

// Splits an S3 endpoint into scheme and the authority exactly as it must appear
// both in the request URL and in the signed Host header: an IPv6 literal keeps
// its brackets ("[::1]:9000"). Loopback endpoints may use plain http;
// everything else must be https.
bool parseEndpoint(const QString& endpoint, QString* scheme, QString* host, QString* error);

// Object key segments for an S3 upload, preserving the configured prefix
// literally: repeated slashes and dot segments are kept, because S3 keys are
// arbitrary strings and normalising them would address a different object.
QList<QString> objectKeySegments(const QString& prefix, const QString& remoteName);

// Whether a protocol can publish without replacing an existing object.
// S3 uses a conditional create (If-None-Match: *), SSH publishes with link(2)
// which fails on an existing target. SFTP and FTP have no such primitive: they
// check first and are honest about the remaining race window.
bool supportsAtomicNoReplace(Protocol protocol);

}
