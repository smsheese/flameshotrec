// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "x11recordingbackend.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class QFile;
class QProcess;

namespace Recording {

// A recordable PulseAudio/PipeWire source as reported by the sound server.
struct AudioSourceInfo
{
    QString name;
    QString description;
    bool monitor{false};
};

// A resolved capture input. Both values are plain FFmpeg input options, so the
// same recorder accepts real sound-server sources and synthetic FFmpeg sources
// in tests without a system audio device.
struct AudioSourceSpec
{
    QString label; // "system" or "microphone"; only used for diagnostics
    QString inputFormat{QStringLiteral("pulse")};
    QString inputUrl;
    bool monitor{false};
    // Extra FFmpeg input options placed before -i. Production leaves this
    // empty; tests use it to pace synthetic sources in real time (-re).
    QStringList extraInputArgs;
};

// What the configured FFmpeg build can do for audio capture and muxing.
struct AudioCapabilities
{
    bool pulseInput{false};
    bool aacEncoder{false};
    bool opusEncoder{false};
    QString error;
    bool usable(const QString& container) const;
};

namespace Audio {

// pactl ships with PulseAudio and talks to the PipeWire pulse server as well.
QString pactlExecutable();

// Pure parsers; they never touch a sound server. Tests use synthetic output.
QList<AudioSourceInfo> parseSourceList(const QString& text);
QString parseDefaultSink(const QString& infoText);
QString parseDefaultSource(const QString& infoText);
QString monitorForSink(const QString& sink);

// Live queries. An empty result is accompanied by a non-empty error string.
QList<AudioSourceInfo> listSources(QString* error);
QString defaultMonitorSource(QString* error);
QString defaultInputSource(QString* error);

// Runs FFmpeg's own capability listing instead of guessing from the build name.
AudioCapabilities probeCapabilities(const QString& ffmpeg);

int sampleRateForQuality(const QString& quality);
QStringList qualityKeys();
QString qualityDisplayName(const QString& quality);

// "aac" for mp4/mkv, "libopus" for webm, empty when audio cannot be muxed.
QString audioCodecForContainer(const QString& container);

// Maps the requested options onto concrete capture specs. Empty source names
// follow the default output monitor and the default input. An explicitly named
// source that the sound server does not report is an error, never a silent
// fallback.
bool resolveSources(const AudioRecordingOptions& options,
                    const QList<AudioSourceInfo>& available,
                    const QString& defaultMonitor,
                    const QString& defaultInput,
                    QList<AudioSourceSpec>* specs,
                    QString* error);

// Convenience wrapper for callers that have no enumeration at hand: it asks
// the sound server only when a source must follow the default output monitor
// or the default input. Explicitly configured names are left to FFmpeg, whose
// error then names the missing source.
bool resolveRequestedSources(const AudioRecordingOptions& options,
                             QList<AudioSourceSpec>* specs,
                             QString* error);

} // namespace Audio

inline bool audioRequested(const AudioRecordingOptions& options)
{
    return options.systemEnabled || options.microphoneEnabled;
}

// Captures every requested source into its own raw 16-bit stereo PCM file.
// Audio that arrives while capture is not active (pause, or the interval
// before the encoder accepts its first frame) is discarded, so the stored
// duration always matches active video time.
class AudioRecorder final : public QObject
{
    Q_OBJECT
public:
    explicit AudioRecorder(QObject* parent = nullptr);
    ~AudioRecorder() override;

    bool start(const QString& ffmpeg,
               const QList<AudioSourceSpec>& specs,
               int sampleRate,
               const QString& directory,
               QString* error);
    void setCapturing(bool capturing);
    bool isCapturing() const { return m_capturing; }
    // Stops capture, keeping every byte captured while active. May be called
    // once; later calls do nothing.
    void stop();

    int sampleRate() const { return m_sampleRate; }
    int channels() const { return 2; }
    int sourceCount() const { return int(m_channels.size()); }
    qint64 capturedBytes(int index) const;
    qint64 capturedMilliseconds(int index) const;
    qint64 capturedMilliseconds() const;
    QString filePath(int index) const;
    QString label(int index) const;
    bool isActive() const { return !m_channels.empty(); }

signals:
    void failed(const QString& message);

private:
    struct Channel
    {
        AudioSourceSpec spec;
        QString path;
        std::unique_ptr<QProcess> process;
        std::unique_ptr<QFile> file;
        QByteArray errorOutput;
        qint64 bytes{0};
    };
    void readChannel(int index);
    void discardPending();
    void handleChannelFinished(int index, int exitCode);
    void emitFailureOnce(const QString& message);
    void closeChannels();

    // std::vector, not QList: a channel owns a QProcess and a QFile.
    std::vector<Channel> m_channels;
    QString m_ffmpeg;
    QString m_directory;
    int m_sampleRate{48000};
    bool m_capturing{false};
    bool m_stopping{false};
    bool m_failed{false};
};

} // namespace Recording
