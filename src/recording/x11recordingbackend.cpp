#include "x11recordingbackend.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <X11/Xlib.h>

namespace {
// The QProcess write buffer is the pipeline's credit, so its depth says whether
// the encoder is really behind. Measured on this host (1920x1080, libx264 CRF
// 23, 30 and 60 fps): a healthy recording keeps at most two frames buffered; a
// 150 ms GUI-thread stall peaks at two frames and drains within tens of
// milliseconds while the delivered frame count and duration stay exact; ten
// competing CPU-bound processes change neither. A backlog that merely touches
// one frame therefore says nothing about the encoder and must not reach the
// user, which is what the old `> m_frameBytes` test did on the first spike of
// any catch-up burst.
constexpr qsizetype kBacklogFrames = 3;
// Sustained beyond kBacklogFrames before it is worth telling the user.
constexpr qint64 kBacklogWarnMs = 250;
// Sustained beyond kBacklogFrames before the recording is failed, as before:
// a backlog that cannot drain in a second is not catching up any more.
constexpr qint64 kBacklogFailMs = 1000;
// A degrading encoder stays visible without flooding the console or the user.
constexpr qint64 kBacklogWarnIntervalMs = 5000;

bool recordingDebugEnabled()
{
    static const bool enabled =
      qEnvironmentVariableIsSet("FLAMESHOT_DEBUG_RECORDING");
    return enabled;
}
}

namespace Recording {
bool hasX11Compositor()
{
    Display* display = XOpenDisplay(nullptr);
    if (!display) return false;
    const QByteArray selection = QByteArrayLiteral("_NET_WM_CM_S") +
                                 QByteArray::number(DefaultScreen(display));
    const Atom atom = XInternAtom(display, selection.constData(), False);
    const bool available = XGetSelectionOwner(display, atom) != None;
    XCloseDisplay(display);
    return available;
}

X11RecordingBackend::X11RecordingBackend(QObject *parent)
  : QObject(parent)
{
    m_frameTimer.setTimerType(Qt::PreciseTimer);
    m_frameTimer.setSingleShot(true);
    connect(&m_frameTimer, &QTimer::timeout,
            this, &X11RecordingBackend::writeScheduledFrame);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this]() {
        m_errorOutput += m_process.readAllStandardError();
        constexpr qsizetype maxErrorBytes = 16 * 1024;
        if (m_errorOutput.size() > maxErrorBytes)
            m_errorOutput.remove(0, m_errorOutput.size() - maxErrorBytes);
    });
    connect(&m_captureProcess, &QProcess::readyReadStandardOutput,
            this, &X11RecordingBackend::handleCaptureFrames);
    connect(&m_captureProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                m_captureEnded = true;
                if (!m_stopping && m_state != State::Failed)
                    fail(QStringLiteral("Screen capture stopped unexpectedly (%1): %2")
                           .arg(code).arg(QString::fromLocal8Bit(m_errorOutput).trimmed()));
                else
                    handleCaptureFrames();
                Q_UNUSED(status);
            });
    connect(&m_captureProcess, &QProcess::readyReadStandardError, this, [this]() {
        m_errorOutput += m_captureProcess.readAllStandardError();
        constexpr qsizetype maxErrorBytes = 16 * 1024;
        if (m_errorOutput.size() > maxErrorBytes)
            m_errorOutput.remove(0, m_errorOutput.size() - maxErrorBytes);
    });
    connect(&m_process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            &X11RecordingBackend::handleProcessFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail(m_process.errorString());
    });
    connect(&m_captureProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    fail(m_captureProcess.errorString());
            });
}

X11RecordingBackend::~X11RecordingBackend()
{
    disconnect(&m_captureProcess, nullptr, this, nullptr);
    disconnect(&m_process, nullptr, this, nullptr);
    m_captureProcess.kill();
    m_process.kill();
    if (m_captureProcess.state() != QProcess::NotRunning) m_captureProcess.waitForFinished(1000);
    if (m_process.state() != QProcess::NotRunning) m_process.waitForFinished(1000);
}

void X11RecordingBackend::setState(State state)
{
    if (m_state == state) return;
    m_state = state;
    emit stateChanged(state);
}

bool X11RecordingBackend::start(const QString &ffmpeg,
                                const QRect &region,
                                const QString &output,
                                const QString &display)
{
    return start(ffmpeg, region, output, RecordingOptions{}, display);
}

bool X11RecordingBackend::start(const QString &ffmpeg,
                                const QRect &region,
                                const QString &output,
                                const RecordingOptions &options,
                                const QString &display)
{
    m_lastError.clear();
    if (m_process.state() != QProcess::NotRunning || region.width() <= 0 ||
        m_captureProcess.state() != QProcess::NotRunning || region.height() <= 0 || output.trimmed().isEmpty() ||
        !QFileInfo(ffmpeg).isExecutable()) {
        m_lastError = QObject::tr("The recording engine is busy, the capture area "
                                  "is empty, or FFmpeg is not executable.");
        return false;
    }
    const QString codec = options.codec.toLower();
    const QString container = options.container.toLower();
    const QString rateControl = options.rateControl.toLower();
    if (options.frameRate < 1 || options.frameRate > 120 ||
        (codec != QStringLiteral("libx264") && codec != QStringLiteral("libvpx-vp9")) ||
        (container != QStringLiteral("mp4") && container != QStringLiteral("mkv") &&
         container != QStringLiteral("webm")) ||
        (rateControl != QStringLiteral("crf") && rateControl != QStringLiteral("abr")) ||
        options.quality < 0 || options.quality >
          (codec == QStringLiteral("libx264") ? 51 : 63) || options.bitrateKbps < 100 ||
        options.bitrateKbps > 100000 ||
        (container == QStringLiteral("webm") && codec != QStringLiteral("libvpx-vp9")) ||
        (container == QStringLiteral("mp4") && codec == QStringLiteral("libvpx-vp9"))) {
        m_lastError = QObject::tr(
          "The video settings are not supported by this recorder.");
        return false;
    }
    const QFileInfo outputInfo(output);
    const QString expectedSuffix = container == QStringLiteral("matroska")
                                     ? QStringLiteral("mkv") : container;
    if (outputInfo.suffix().compare(expectedSuffix, Qt::CaseInsensitive) != 0) {
        m_lastError = QObject::tr("The file extension does not match the "
                                  "selected container (%1).")
                        .arg(expectedSuffix);
        return false;
    }
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        m_lastError = QObject::tr("Could not create %1.")
                        .arg(outputInfo.absolutePath());
        return false;
    }

    const QString source = display.isEmpty()
                             ? QString::fromLocal8Bit(qgetenv("DISPLAY"))
                             : display;
    if (source.isEmpty()) {
        // A bare refusal made this failure undiagnosable (review F13).
        m_lastError = QObject::tr(
          "Recording needs an X11 display, but DISPLAY is not set.");
        return false;
    }

    if (outputInfo.exists()) { // Never clobber a user's recording.
        m_lastError = QObject::tr("%1 already exists.")
                        .arg(outputInfo.absoluteFilePath());
        return false;
    }
    m_outputPath = outputInfo.absoluteFilePath();
    m_errorOutput.clear();
    m_stopping = false;
    const QString input = QStringLiteral("%1+%2,%3")
                            .arg(source)
                            .arg(region.x())
                            .arg(region.y());
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    const int width = region.width();
    const int height = region.height();
    m_frameBytes = qsizetype(width) * height * 4;
    m_pendingFrames.clear();
    m_latestFrame.clear();
    m_frameRate = options.frameRate;
    m_scheduledFrameCount = 0;
    m_completedActiveNs = 0;
    m_activeClockRunning = false;
    m_captureStarted = false;
    m_backpressureClock.invalidate();
    m_backlogPeakBytes = 0;
    m_lastBacklogWarningMs = -kBacklogWarnIntervalMs;
    m_consecutiveLateFrames = 0;
    m_captureEnded = false;
    m_paused = false;
    m_resumePending = false;
    m_externalFrameInput = false;
    if (!launchEncoder(ffmpeg, width, height, m_outputPath, options)) {
        m_captureProcess.kill();
        setState(State::Failed);
        emit recordingFailed(m_process.errorString());
        return false;
    }
    m_captureProcess.start(ffmpeg,
                           { QStringLiteral("-f"), QStringLiteral("x11grab"),
                             QStringLiteral("-framerate"), QString::number(options.frameRate),
                             QStringLiteral("-video_size"),
                             QStringLiteral("%1x%2").arg(width).arg(height),
                             QStringLiteral("-i"), input,
                             QStringLiteral("-f"), QStringLiteral("rawvideo"),
                             QStringLiteral("-pix_fmt"), QStringLiteral("bgr0"),
                             QStringLiteral("pipe:1") });
    if (!m_captureProcess.waitForStarted(3000)) {
        m_process.kill();
        setState(State::Failed);
        emit recordingFailed(m_captureProcess.errorString());
        return false;
    }
    // Do not claim recording has started until a complete captured frame is
    // available. Process startup can take a noticeable amount of time.
    setState(State::Preparing);
    return true;
}

bool X11RecordingBackend::validateOptions(const RecordingOptions& options,
                                          const QString& output) const
{
    const QString codec = options.codec.toLower();
    const QString container = options.container.toLower();
    const QString rateControl = options.rateControl.toLower();
    const QFileInfo outputInfo(output);
    return options.frameRate >= 1 && options.frameRate <= 120 &&
      (codec == QStringLiteral("libx264") || codec == QStringLiteral("libvpx-vp9")) &&
      (container == QStringLiteral("mp4") || container == QStringLiteral("mkv") ||
       container == QStringLiteral("webm")) &&
      (rateControl == QStringLiteral("crf") || rateControl == QStringLiteral("abr")) &&
      options.quality >= 0 &&
      options.quality <= (codec == QStringLiteral("libx264") ? 51 : 63) &&
      options.bitrateKbps >= 100 && options.bitrateKbps <= 100000 &&
      !(container == QStringLiteral("webm") && codec != QStringLiteral("libvpx-vp9")) &&
      !(container == QStringLiteral("mp4") && codec == QStringLiteral("libvpx-vp9")) &&
      outputInfo.suffix().compare(container == QStringLiteral("mkv")
                                    ? QStringLiteral("mkv") : container,
                                  Qt::CaseInsensitive) == 0;
}

bool X11RecordingBackend::launchEncoder(const QString& ffmpeg, int width,
                                        int height, const QString& output,
                                        const RecordingOptions& options)
{
    const QString codec = options.codec.toLower();
    const QString container = options.container.toLower();
    QStringList args{ QStringLiteral("-f"), QStringLiteral("rawvideo"),
                      QStringLiteral("-pixel_format"), QStringLiteral("bgr0"),
                      QStringLiteral("-video_size"), QStringLiteral("%1x%2").arg(width).arg(height),
                      QStringLiteral("-framerate"), QString::number(options.frameRate),
                      QStringLiteral("-i"), QStringLiteral("pipe:0"),
                      QStringLiteral("-an"), QStringLiteral("-c:v"), codec,
                      QStringLiteral("-vf"), QStringLiteral("pad=ceil(iw/2)*2:ceil(ih/2)*2"),
                      QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p") };
    if (options.rateControl.compare(QStringLiteral("crf"), Qt::CaseInsensitive) == 0) {
        args << QStringLiteral("-crf") << QString::number(options.quality);
        if (codec == QStringLiteral("libvpx-vp9"))
            args << QStringLiteral("-b:v") << QStringLiteral("0");
    } else {
        args << QStringLiteral("-b:v") << QStringLiteral("%1k").arg(options.bitrateKbps);
    }
    args << QStringLiteral("-f")
         << (container == QStringLiteral("mkv") ? QStringLiteral("matroska") : container)
         << QStringLiteral("-n") << output;
    m_process.start(ffmpeg, args);
    return m_process.waitForStarted(3000);
}

bool X11RecordingBackend::startEncoder(const QString& ffmpeg,
                                       const QSize& frameSize,
                                       const QString& output,
                                       const RecordingOptions& options)
{
    if (m_process.state() != QProcess::NotRunning ||
        m_captureProcess.state() != QProcess::NotRunning ||
        frameSize.width() <= 0 || frameSize.height() <= 0 ||
        output.trimmed().isEmpty() || !QFileInfo(ffmpeg).isExecutable() ||
        !validateOptions(options, output)) return false;
    const QFileInfo outputInfo(output);
    if (outputInfo.exists() || !QDir().mkpath(outputInfo.absolutePath())) return false;
    m_outputPath = outputInfo.absoluteFilePath();
    m_errorOutput.clear();
    m_stopping = false;
    m_paused = false;
    m_resumePending = false;
    m_externalFrameInput = true;
    m_externalFrameWidth = frameSize.width();
    m_externalFrameHeight = frameSize.height();
    m_frameBytes = qsizetype(frameSize.width()) * frameSize.height() * 4;
    m_pendingFrames.clear();
    m_latestFrame.clear();
    m_discardBytes = 0;
    m_frameRate = options.frameRate;
    m_scheduledFrameCount = 0;
    m_completedActiveNs = 0;
    m_activeClockRunning = false;
    m_captureStarted = false;
    m_backpressureClock.invalidate();
    m_backlogPeakBytes = 0;
    m_lastBacklogWarningMs = -kBacklogWarnIntervalMs;
    m_consecutiveLateFrames = 0;
    if (!launchEncoder(ffmpeg, frameSize.width(), frameSize.height(),
                       m_outputPath, options)) {
        setState(State::Failed);
        emit recordingFailed(m_process.errorString());
        return false;
    }
    setState(State::Preparing);
    return true;
}

void X11RecordingBackend::submitFrame(const QByteArray& bgra, int width,
                                      int height, int stride, qint64 timestampNs)
{
    Q_UNUSED(timestampNs)
    if (width <= 0 || height <= 0 || stride < width * 4 ||
        bgra.size() < qsizetype(stride) * height) return;
    QByteArray frame(qsizetype(width) * height * 4, Qt::Uninitialized);
    for (int y = 0; y < height; ++y) {
        const char* source = bgra.constData() + qsizetype(y) * stride;
        char* target = frame.data() + qsizetype(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            target[x * 4] = source[x * 4];
            target[x * 4 + 1] = source[x * 4 + 1];
            target[x * 4 + 2] = source[x * 4 + 2];
            target[x * 4 + 3] = 0;
        }
    }
    if (QThread::currentThread() == thread()) {
        if (!m_externalFrameInput ||
            (m_state != State::Preparing && m_state != State::Recording &&
             m_state != State::Paused) || width != m_externalFrameWidth ||
            height != m_externalFrameHeight || m_paused) return;
        acceptLatestFrame(frame);
        return;
    }
    bool queueDelivery = false;
    {
        QMutexLocker lock(&m_externalFrameMutex);
        m_queuedExternalFrame = std::move(frame);
        if (!m_externalFrameDeliveryQueued) {
            m_externalFrameDeliveryQueued = true;
            queueDelivery = true;
        }
    }
    if (queueDelivery)
        QMetaObject::invokeMethod(this, &X11RecordingBackend::deliverExternalFrame,
                                  Qt::QueuedConnection);
}

void X11RecordingBackend::deliverExternalFrame()
{
    QByteArray frame;
    {
        QMutexLocker lock(&m_externalFrameMutex);
        frame = std::move(m_queuedExternalFrame);
        m_queuedExternalFrame.clear();
        m_externalFrameDeliveryQueued = false;
    }
    if (!frame.isEmpty() && m_externalFrameInput && !m_paused)
        acceptLatestFrame(frame);
}

void X11RecordingBackend::acceptLatestFrame(const QByteArray& frame)
{
    if (m_stopping || m_state == State::Failed || m_state == State::Finished) return;
    m_latestFrame = frame;
    if (m_state == State::Preparing || (m_state == State::Paused && m_resumePending)) {
        m_resumePending = false;
        m_captureStarted = true;
        m_frameClock.start();
        m_activeClockRunning = true;
        setState(State::Recording);
        writeScheduledFrame();
    }
}

void X11RecordingBackend::stop()
{
    if (m_process.state() != QProcess::Running || m_stopping) return;
    m_stopping = true;
    m_frameTimer.stop();
    if (m_activeClockRunning) {
        m_completedActiveNs += m_frameClock.nsecsElapsed();
        m_activeClockRunning = false;
    }
    setState(State::Finalizing);
    if (!m_externalFrameInput) {
        m_captureProcess.kill();
        m_captureProcess.waitForFinished(1000);
        handleCaptureFrames();
    }
    // Drain frames owed up to Stop before EOF, without blocking the GUI or
    // dropping timestamps when the encoder was briefly busy.
    writeScheduledFrame();
}

void X11RecordingBackend::abort(const QString& reason)
{
    fail(reason.isEmpty() ? tr("The screen capture source failed.") : reason);
}

bool X11RecordingBackend::pause()
{
    if (m_state != State::Recording) return false;
    m_frameTimer.stop();
    m_paused = true;
    m_resumePending = false;
    if (m_activeClockRunning) {
        m_completedActiveNs += m_frameClock.nsecsElapsed();
        m_activeClockRunning = false;
    }
    setState(State::Paused);
    writeScheduledFrame();
    return true;
}

bool X11RecordingBackend::resume()
{
    if (m_state != State::Paused) return false;
    // Account for every byte emitted during Pause, including a partial frame
    // and bytes still buffered by QProcess. Discard through the next frame
    // boundary so the encoder always receives complete aligned frames.
    const qsizetype unreadBytes = m_externalFrameInput
                                   ? 0 : m_captureProcess.readAllStandardOutput().size();
    const qsizetype pausedBytes = m_externalFrameInput ? 0
      : m_pendingFrames.size() + unreadBytes;
    m_discardBytes = (m_frameBytes - (pausedBytes % m_frameBytes)) % m_frameBytes;
    m_pendingFrames.clear();
    m_paused = false;
    m_resumePending = true;
    return true;
}

void X11RecordingBackend::handleCaptureFrames()
{
    if (m_frameBytes <= 0) { m_captureProcess.readAllStandardOutput(); return; }
    m_pendingFrames += m_captureProcess.readAllStandardOutput();
    drainCaptureFrames();
}

void X11RecordingBackend::drainCaptureFrames()
{
    if (m_frameBytes <= 0) return;
    if (m_discardBytes > 0) {
        const qsizetype discard = qMin(m_discardBytes, m_pendingFrames.size());
        m_pendingFrames.remove(0, discard);
        m_discardBytes -= discard;
        if (m_discardBytes > 0) return;
    }
    const qsizetype completeBytes = (m_pendingFrames.size() / m_frameBytes) * m_frameBytes;
    if (completeBytes == 0) return;
    if (m_paused) {
        m_pendingFrames.remove(0, completeBytes);
        return;
    }
    // Keep only the most recent complete capture frame. Output cadence is
    // controlled separately so dropping stale capture frames cannot shorten
    // the recorded duration.
    const QByteArray latest = m_pendingFrames.mid(completeBytes - m_frameBytes, m_frameBytes);
    m_pendingFrames.remove(0, completeBytes);
    acceptLatestFrame(latest);
}

void X11RecordingBackend::writeScheduledFrame()
{
    if (m_state != State::Recording && m_state != State::Paused && m_state != State::Finalizing) return;
    const qint64 activeNs = m_completedActiveNs + (m_activeClockRunning ? m_frameClock.nsecsElapsed() : 0);
    const qint64 expectedFrames = m_captureStarted ? (activeNs * m_frameRate / 1000000000LL) + 1 : 0;
    if (m_scheduledFrameCount >= expectedFrames || m_latestFrame.size() != m_frameBytes) {
        if (m_state == State::Finalizing) m_process.closeWriteChannel();
        else scheduleNextFrame();
        return;
    }
    // Never advance the video timeline without delivering its frame. Brief
    // catch-up bursts are retried with bounded buffering, including during
    // Pause/Stop; a backlog that stays deep reports failure and retains partial
    // output. The threshold is a *backlog* (several frames), not one frame: the
    // burst a single GUI stall produces is normal, drains on its own, and used
    // to be reported as an encoder problem on the first frame over the line.
    const qsizetype backlogLimit = kBacklogFrames * m_frameBytes;
    if (m_process.bytesToWrite() > backlogLimit) {
        if (!m_backpressureClock.isValid()) {
            m_backpressureClock.start();
            m_backlogPeakBytes = m_process.bytesToWrite();
        }
        m_backlogPeakBytes = qMax(m_backlogPeakBytes, m_process.bytesToWrite());
        const qint64 backlogMs = m_backpressureClock.elapsed();
        if (backlogMs >= kBacklogFailMs) {
            if (recordingDebugEnabled()) {
                qInfo("encoder: backlog of %lld bytes did not drain for %lld ms "
                      "(%lld frames owed); failing",
                      static_cast<long long>(m_backlogPeakBytes),
                      static_cast<long long>(backlogMs),
                      static_cast<long long>(expectedFrames - m_scheduledFrameCount));
            }
            fail(tr("Recording stopped because the encoder could not keep up with the selected frame rate."));
        } else {
            if (backlogMs >= kBacklogWarnMs) reportBacklog(backlogMs);
            m_frameTimer.start(2);
        }
        return;
    }
    if (m_backpressureClock.isValid()) {
        // The episode drained. Debug builds see every one of them with its
        // depth and duration, which is what separated "the encoder is behind"
        // from "the GUI thread stopped for a moment" during the investigation.
        if (recordingDebugEnabled()) {
            qInfo("encoder: backlog episode drained after %lld ms (peak %lld "
                  "bytes, limit %lld)",
                  static_cast<long long>(m_backpressureClock.elapsed()),
                  static_cast<long long>(m_backlogPeakBytes),
                  static_cast<long long>(backlogLimit));
        }
        m_backpressureClock.invalidate();
        m_backlogPeakBytes = 0;
    }
    if (m_process.write(m_latestFrame) < 0) {
        fail(m_process.errorString());
        return;
    }
    ++m_scheduledFrameCount;
    if (m_state == State::Recording) scheduleNextFrame();
    else m_frameTimer.start(0);
}

void X11RecordingBackend::reportBacklog(qint64 backlogMs)
{
    // Rate limited per recording, not latched: an encoder that keeps falling
    // behind stays visible, but a repeated burst cannot flood the console or the
    // report the user reads after the recording.
    const qint64 now = m_frameClock.isValid() ? m_frameClock.nsecsElapsed() / 1000000 : 0;
    if (m_lastBacklogWarningMs >= 0 && now - m_lastBacklogWarningMs < kBacklogWarnIntervalMs)
        return;
    m_lastBacklogWarningMs = now;
    qInfo("encoder: backlog above %lld bytes for %lld ms; the selected frame "
          "rate is not being met",
          static_cast<long long>(kBacklogFrames * m_frameBytes),
          static_cast<long long>(backlogMs));
    emit recordingWarning(tr("The encoder is catching up with the selected frame rate."));
}

void X11RecordingBackend::scheduleNextFrame()
{
    if (m_state != State::Recording || !m_activeClockRunning) return;
    const qint64 deadlineNs = (m_scheduledFrameCount * 1000000000LL) / m_frameRate;
    const qint64 activeNs = m_completedActiveNs + m_frameClock.nsecsElapsed();
    const qint64 remainingNs = deadlineNs - activeNs;
    const int delayMs = remainingNs <= 0 ? 0 : int((remainingNs + 999999) / 1000000);
    m_frameTimer.start(delayMs);
}

void X11RecordingBackend::fail(const QString &message)
{
    if (m_state == State::Failed || m_state == State::Finished) return;
    m_frameTimer.stop();
    m_captureProcess.kill();
    m_process.kill();
    setState(State::Failed);
    emit recordingFailed(message);
}

void X11RecordingBackend::handleProcessFinished(int exitCode,
                                                QProcess::ExitStatus status)
{
    if (m_state == State::Failed) return;
    m_frameTimer.stop();
    const QFileInfo output(m_outputPath);
    if (m_stopping && m_scheduledFrameCount > 0 && status == QProcess::NormalExit && exitCode == 0 &&
        output.exists() && output.size() > 0) {
        setState(State::Finished);
        emit recordingFinished(m_outputPath);
        return;
    }
    m_captureProcess.kill();
    setState(State::Failed);
    QString error = QString::fromLocal8Bit(m_errorOutput).trimmed();
    if (error.isEmpty()) error = m_process.errorString();
    emit recordingFailed(error);
}
}
