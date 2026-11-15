#pragma once
#include <QProcess>
#include <QElapsedTimer>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QSize>
#include <QMutex>

#include <cstdint>

namespace Recording {
bool hasX11Compositor();

constexpr int kDefaultFrameRate = 30;
constexpr int kDefaultVideoBitrateKbps = 4000;
constexpr int kDefaultQuality = 23;

// Optional audio capture. Both sources are off by default so callers that do
// not configure audio keep producing silent video, exactly as before. These
// defaults live with the recording options (not in the configuration) so that
// the recording module, the video settings tab and the tests share one
// definition; the configuration and CaptureRecordingSession only override it.
struct AudioRecordingOptions
{
    bool systemEnabled{false};
    bool microphoneEnabled{false};
    // Empty follows the default output monitor or the default input device.
    QString systemSource;
    QString microphoneSource;
    // "low" (32 kHz), "medium" (44.1 kHz) or "high" (48 kHz).
    QString quality{QStringLiteral("high")};
    // 32..512 kbit/s; the muxer refuses values outside the supported range.
    static constexpr int kMinBitrateKbps = 32;
    static constexpr int kMaxBitrateKbps = 512;
    static constexpr int kDefaultBitrateKbps = 128;
    int bitrateKbps{kDefaultBitrateKbps};
};
struct RecordingOptions {
    // Order chosen so the aggregate needs no padding between members.
    AudioRecordingOptions audio;
    int frameRate{kDefaultFrameRate};
    int bitrateKbps{kDefaultVideoBitrateKbps};
    int quality{kDefaultQuality};
    QString codec{QStringLiteral("libx264")};
    QString container{QStringLiteral("mp4")};
    QString rateControl{QStringLiteral("crf")};
};

class X11RecordingBackend final : public QObject {
    Q_OBJECT
public:
    Q_DISABLE_COPY_MOVE(X11RecordingBackend)
    enum class State : std::uint8_t {
        Idle,
        Preparing,
        Recording,
        Paused,
        Finalizing,
        Finished,
        Failed
    };
    Q_ENUM(State)
    explicit X11RecordingBackend(QObject *parent = nullptr);
    ~X11RecordingBackend() override;
    bool start(const QString &ffmpeg, const QRect &region, const QString &output,
               const QString &display = {});
    bool start(const QString &ffmpeg, const QRect &region, const QString &output,
               const RecordingOptions &options, const QString &display = {});
    // Encoder-only mode for adapters that supply already cropped BGRA frames.
    bool startEncoder(const QString& ffmpeg, const QSize& frameSize,
                      const QString& output, const RecordingOptions& options = {});
    // May be called from a capture worker thread. Frames are queued to this
    // object's thread and must match startEncoder()'s dimensions.
    void submitFrame(const QByteArray& bgra, int width, int height, int stride,
                     qint64 timestampNs = -1);
    void stop();
    // Kill the input/encoder and signal failure when an upstream frame source
    // fails; any partial output is left in place for possible recovery.
    void abort(const QString& reason);
    // Pause discards complete captured frames before they reach the encoder.
    bool pause();
    bool resume();
    [[nodiscard]] State state() const { return m_state; }
    // Why the last start() was refused, when the refusal has a specific cause
    // (no X display, invalid settings, unusable output path). Empty when the
    // encoder itself refused and reported through recordingFailed().
    QString lastError() const { return m_lastError; }
    QProcess *process() { return &m_process; }

signals:
    void stateChanged(Recording::X11RecordingBackend::State state);
    void recordingFailed(const QString &message);
    void recordingFinished(const QString &outputPath);
    void recordingWarning(const QString &message);

private:
    void setState(State state);
    void handleProcessFinished(int exitCode, QProcess::ExitStatus status);
    void handleCaptureFrames();
    void drainCaptureFrames();
    void writeScheduledFrame();
    void reportBacklog(qint64 backlogMs);
    void scheduleNextFrame();
    [[nodiscard]] bool validateOptions(const RecordingOptions& options,
                                       const QString& output) const;
    bool launchEncoder(const QString& ffmpeg, int width, int height,
                       const QString& output, const RecordingOptions& options);
    void acceptLatestFrame(const QByteArray& frame);
    void deliverExternalFrame();
    void fail(const QString &message);
    QProcess m_process;
    QProcess m_captureProcess;
    State m_state{State::Idle};
    QString m_lastError;
    QString m_outputPath;
    QByteArray m_errorOutput;
    bool m_stopping{false};
    bool m_paused{false};
    bool m_resumePending{false};
    qsizetype m_frameBytes{0};
    qsizetype m_discardBytes{0};
    QByteArray m_pendingFrames;
    QByteArray m_latestFrame;
    bool m_captureEnded{false};
    QTimer m_frameTimer;
    QElapsedTimer m_frameClock;
    int m_frameRate{kDefaultFrameRate};
    qint64 m_scheduledFrameCount{0};
    qint64 m_completedActiveNs{0};
    bool m_activeClockRunning{false};
    bool m_captureStarted{false};
    QElapsedTimer m_backpressureClock;
    int m_consecutiveLateFrames{0};
    // Deepest write buffer seen during the current backlog episode, and when the
    // backlog was last reported. A backlog is reported only when it stays deep
    // (see kBacklogWarnMs) and at most once per interval.
    qsizetype m_backlogPeakBytes{0};
    qint64 m_lastBacklogWarningMs{-1};
    bool m_externalFrameInput{false};
    int m_externalFrameWidth{0};
    int m_externalFrameHeight{0};
    QMutex m_externalFrameMutex;
    QByteArray m_queuedExternalFrame;
    bool m_externalFrameDeliveryQueued{false};
};
}
