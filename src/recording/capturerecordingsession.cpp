// SPDX-License-Identifier: GPL-3.0-or-later
#include "capturerecordingsession.h"
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QFileInfo>

#include <cerrno>
#include <cstring>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#endif

namespace Recording {
namespace {

// Result of publishing one file under one name without ever replacing a file
// that already holds that name.
struct NoReplace
{
    enum Status
    {
        Published,
        Exists, // the name is taken; nothing was written
        Failed  // the publish could not be attempted or completed
    };
    Status status{Failed};
    int error{0}; // errno of the failing system call, 0 when unknown
};

QString systemMessage(int error)
{
    return error == 0 ? QString()
                      : QString::fromLocal8Bit(std::strerror(error));
}

// Publishes `from` as `to` so that an existing `to` is never replaced.
//
// QFile::rename() is documented to refuse an existing target, and Qt 6.8 does
// implement that on Linux through renameat2(RENAME_NOREPLACE) -- checked on
// this host, where renaming onto an existing file returns false and leaves both
// files in place. It is still not a guarantee to build on: Qt falls back to
// plain ::rename() (which *replaces* the target) where the kernel or the
// filesystem does not implement the flag, and an existence check made earlier
// cannot close that window, because the target can appear in between.
//
// So the primitive is done here explicitly: renameat2() with RENAME_NOREPLACE
// where the kernel supports it, link()+unlink() where it does not (link() fails
// with EEXIST instead of replacing), and never plain rename(). A caller that
// gets `Exists` picks another name and tries again.
NoReplace publishNoReplace(const QString& from, const QString& to)
{
    NoReplace result;
#ifdef Q_OS_UNIX
    const QByteArray source = QFile::encodeName(from);
    const QByteArray target = QFile::encodeName(to);
    if (source.isEmpty() || target.isEmpty()) {
        result.error = EINVAL;
        return result;
    }
#ifdef Q_OS_LINUX
#ifdef SYS_renameat2
    // RENAME_NOREPLACE is 1; the flag is not exposed by glibc everywhere, so it
    // is spelled out here instead of relying on a header.
    constexpr unsigned int kRenameNoReplace = 1U << 0;
    if (::syscall(SYS_renameat2,
                  AT_FDCWD,
                  source.constData(),
                  AT_FDCWD,
                  target.constData(),
                  kRenameNoReplace) == 0) {
        result.status = NoReplace::Published;
        return result;
    }
    result.error = errno;
    if (result.error == EEXIST) {
        result.status = NoReplace::Exists;
        return result;
    }
    // ENOSYS: kernel without renameat2. EINVAL/EOPNOTSUPP: this filesystem does
    // not implement the flag. Both fall through to link()+unlink(), which gives
    // the same no-replace guarantee.
    const bool unsupported = result.error == ENOSYS || result.error == EINVAL ||
                             result.error == EOPNOTSUPP ||
                             result.error == ENOTSUP;
    if (!unsupported) {
        result.status = NoReplace::Failed;
        return result;
    }
#endif
#endif
    if (::link(source.constData(), target.constData()) == 0) {
        if (::unlink(source.constData()) != 0) {
            // The recording is published; only the staging copy is left behind.
            // That is leaked space, not lost data, so it is not a failure.
            qWarning("could not remove '%s' after publishing it: %s",
                     source.constData(),
                     std::strerror(errno));
        }
        result.status = NoReplace::Published;
        return result;
    }
    result.error = errno;
    result.status = result.error == EEXIST ? NoReplace::Exists : NoReplace::Failed;
#else
    // No POSIX primitive to build on: refuse instead of guessing.
    result.error = ENOTSUP;
    Q_UNUSED(from)
    Q_UNUSED(to)
#endif
    return result;
}

// One candidate name for publishing into `directory`: the requested base name
// first, then `<base>_1`, `<base>_2`, ... in the same folder.
QString candidateName(const QString& directory, const QString& base,
                      const QString& suffix, int index)
{
    if (index <= 0) {
        return QDir(directory).filePath(suffix.isEmpty()
                                          ? base
                                          : base + QLatin1Char('.') + suffix);
    }
    const QString numbered = QStringLiteral("%1_%2").arg(base).arg(index);
    return QDir(directory).filePath(suffix.isEmpty()
                                      ? numbered
                                      : numbered + QLatin1Char('.') + suffix);
}

// How many numbered names are tried before giving up. Each attempt is an atomic
// no-replace publish, so a name that appears between two attempts is simply the
// next candidate, never an overwrite.
constexpr int kMaxNameAttempts = 1000;

// Publishes `from` into `directory` under `base`/`suffix`, never replacing an
// existing file, and returns the path it was published at (empty on failure,
// with `error` set).
QString publishInto(const QString& from, const QString& directory,
                    const QString& base, const QString& suffix, QString* error)
{
    if (!QDir().mkpath(directory)) {
        if (error != nullptr) {
            *error = QObject::tr("the folder %1 could not be created")
                       .arg(directory);
        }
        return {};
    }
    NoReplace result = publishNoReplace(
      from, candidateName(directory, base, suffix, 0));
    if (result.status == NoReplace::Published) {
        return candidateName(directory, base, suffix, 0);
    }
    if (result.status == NoReplace::Failed && result.error != EXDEV) {
        if (error != nullptr) {
            *error = systemMessage(result.error);
        }
        return {};
    }
    if (result.status == NoReplace::Exists) {
        // The requested name is already taken by a file that appeared before (or
        // during) this publish: pick the first free numbered name, each attempt
        // being an atomic no-replace publish itself.
        for (int index = 1; index < kMaxNameAttempts; ++index) {
            const QString candidate = candidateName(directory, base, suffix, index);
            result = publishNoReplace(from, candidate);
            if (result.status == NoReplace::Published) {
                return candidate;
            }
            if (result.status == NoReplace::Failed) {
                if (error != nullptr) {
                    *error = systemMessage(result.error);
                }
                return {};
            }
        }
        if (error != nullptr) {
            *error = QObject::tr("every name up to %1_%2 is taken")
                       .arg(base)
                       .arg(kMaxNameAttempts - 1);
        }
        return {};
    }
    // EXDEV: the staging directory is a different filesystem from the user's
    // folder. Copy once into a private partial *inside the destination folder*
    // and publish that copy with the same no-replace primitive, so the visible
    // file appears atomically, a half-written file can never be mistaken for a
    // recording, and an existing file is still never replaced.
    const QString partial =
      QDir(directory).filePath(QStringLiteral(".%1.part-%2")
                                 .arg(base.isEmpty() ? QStringLiteral("recording")
                                                     : base,
                                      QUuid::createUuid()
                                        .toString(QUuid::WithoutBraces)
                                        .left(8)));
    if (!QFile::copy(from, partial)) {
        QFile::remove(partial);
        if (error != nullptr) {
            *error = QObject::tr("the recording could not be copied into %1")
                       .arg(directory);
        }
        return {};
    }
    QString published;
    QString reason;
    for (int index = 0; index < kMaxNameAttempts && published.isEmpty(); ++index) {
        const QString candidate = candidateName(directory, base, suffix, index);
        result = publishNoReplace(partial, candidate);
        if (result.status == NoReplace::Published) {
            published = candidate;
        } else if (result.status == NoReplace::Failed) {
            reason = systemMessage(result.error);
            break;
        }
    }
    if (published.isEmpty()) {
        QFile::remove(partial);
        if (error != nullptr) {
            *error = reason.isEmpty()
                       ? QObject::tr("every name up to %1_%2 is taken")
                           .arg(base)
                           .arg(kMaxNameAttempts - 1)
                       : reason;
        }
        return {};
    }
    // The recording is published; remove the copy that was staged for it.
    QFile::remove(from);
    return published;
}
// Audio matters only when a source was requested; the container codec is then
// validated before the video encoder starts.
QString audioStartupError(const AudioRecordingOptions& options,
                          const AudioCapabilities& capabilities,
                          const QString& container)
{
    if (!capabilities.pulseInput) {
        return capabilities.error.isEmpty()
                 ? QObject::tr("This FFmpeg build cannot capture PulseAudio "
                               "sources.")
                 : capabilities.error;
    }
    const QString codec = Audio::audioCodecForContainer(container);
    if (codec == QLatin1String("aac") && !capabilities.aacEncoder) {
        return QObject::tr(
          "This FFmpeg build has no AAC encoder, which %1 audio requires. "
          "Choose another container or install an FFmpeg build with AAC.")
          .arg(container);
    }
    if (codec == QLatin1String("libopus") && !capabilities.opusEncoder) {
        return QObject::tr(
          "This FFmpeg build has no Opus encoder, which %1 audio requires. "
          "Install FFmpeg with libopus or choose MKV or MP4.")
          .arg(container);
    }
    if (codec.isEmpty()) {
        return QObject::tr("The %1 container cannot store audio.").arg(container);
    }
    if (options.bitrateKbps < 32 || options.bitrateKbps > 512) {
        return QObject::tr("The audio bitrate must be between 32 and 512 kbit/s.");
    }
    return {};
}
} // namespace

CaptureRecordingSession::CaptureRecordingSession(QObject* parent)
  : QObject(parent), m_backend(this)
{
    connect(&m_backend, &X11RecordingBackend::stateChanged, this,
            [this](X11RecordingBackend::State state) {
        if (m_running && state != X11RecordingBackend::State::Recording) {
            m_elapsed += m_segment.elapsed();
            m_running = false;
        } else if (!m_running && state == X11RecordingBackend::State::Recording) {
            m_segment.start();
            m_running = true;
        }
        // Audio follows the video clock exactly: discarded while paused, and
        // closed when the encoder stops accepting frames.
        const bool stopAudioAfterNotify =
          state == X11RecordingBackend::State::Finalizing;
        if (state == X11RecordingBackend::State::Recording)
            m_audio.setCapturing(true);
        else if (state == X11RecordingBackend::State::Paused)
            m_audio.setCapturing(false);
        if (!m_cancelled) emit stateChanged(state);
        if (stopAudioAfterNotify) {
            // Stopping the capture processes can block, so the overlay is told
            // about Finalizing first and the shutdown runs from the event loop
            // instead of inside this state change (review F5).
            QMetaObject::invokeMethod(
              &m_audio, &AudioRecorder::stop, Qt::QueuedConnection);
        }
    });
    connect(&m_backend, &X11RecordingBackend::recordingWarning,
            this, &CaptureRecordingSession::warning);
    connect(&m_audio, &AudioRecorder::failed, this,
            [this](const QString& message) {
        if (m_cancelled || m_failureReported) return;
        if (m_startingAudio) {
            // The recorder can fail inside start(); report it after start()
            // returned, so no receiver deletes the session while startAudio is
            // still on the stack (review F9).
            m_pendingAudioFailure = message;
            return;
        }
        handleAudioFailure(message);
    });
    connect(&m_backend, &X11RecordingBackend::recordingFailed, this,
            [this](const QString& message) {
        if (m_cancelled || m_failureReported) return;
        m_failureReported = true;
        m_audio.stop();
        m_muxer.cancel();
        emit failed(message + keepTemporaryFiles());
    });
    connect(&m_muxer, &AudioMuxer::succeeded, this,
            [this](const QString& path) {
        if (m_cancelled) return;
        publish(path);
    });
    connect(&m_muxer, &AudioMuxer::failed, this, [this](const QString& message) {
        if (m_cancelled || m_failureReported) return;
        m_failureReported = true;
        emit failed(message + keepTemporaryFiles());
    });
    connect(&m_backend, &X11RecordingBackend::recordingFinished, this,
            [this](const QString& temporaryPath) {
        if (m_cancelled) return;
        if (!m_audioStarted) {
            publish(temporaryPath);
            return;
        }
        muxAudio(temporaryPath);
    });
}

CaptureRecordingSession::~CaptureRecordingSession()
{
    cancel();
}

bool CaptureRecordingSession::start(const QString& ffmpeg, const QRect& region,
                                     const QString& destination,
                                     const RecordingOptions& options)
{
    return start(ffmpeg, region, destination, QString(), options);
}

bool CaptureRecordingSession::start(const QString& ffmpeg,
                                     const QRect& region,
                                     const QString& destination,
                                     const QString& stagingFile,
                                     const RecordingOptions& options)
{
    if (m_temporary) return false;
    const QFileInfo target(destination);
    // An existing file at `destination` is not a reason to refuse the recording:
    // `destination` is the name the user would *like*, and the configured
    // pattern repeats, so the second recording after the first save would never
    // start at all (it then also could not be copied). "Never clobber" is
    // enforced where it can actually be guaranteed: publishing picks a free name
    // and publishes with an atomic no-replace rename, so a file that appears
    // between this point and the publish is never replaced either.
    m_destination = target.absoluteFilePath();
    m_stagingFile = stagingFile;
    m_ffmpeg = ffmpeg;
    m_container = options.container.toLower();
    m_audioOptions = options.audio;
    m_audioRequested = audioRequested(options.audio);
    m_startupError.clear();
    m_failureReported = false;
    // The overlay shows "Preparing recording…" until the encoder accepts its
    // first frame, so the UI never claims to record while the audio sources are
    // still being resolved (review F3).
    emit stateChanged(X11RecordingBackend::State::Preparing);
    const QString requestedDirectory = target.absolutePath();
    if (!stagingFile.isEmpty()) {
        // Every recording is encoded in the private staging directory, whatever
        // the action will be. The configured video folder is only written once
        // the user actually saves, opens or uploads the file at publish time, so
        // a copy-only recording cannot create it, write into it or fail on it --
        // not even for the seconds the encoder runs.
        m_encodedInStaging = true;
        m_encoderDirectory = QFileInfo(stagingFile).absolutePath();
        if (!QDir().mkpath(m_encoderDirectory)) {
            m_startupError =
              tr("Could not create the private recording staging folder %1.")
                .arg(m_encoderDirectory);
            return false;
        }
    } else {
        // Compatibility path for a caller that does not stage at all: encode
        // next to the destination and create that folder, as before.
        m_encodedInStaging = false;
        if (!QDir().mkpath(requestedDirectory)) {
            m_startupError = tr("Could not create the video save folder %1.")
                               .arg(requestedDirectory);
            return false;
        }
        m_encoderDirectory = requestedDirectory;
    }
    m_temporary = std::make_unique<QTemporaryDir>(
      m_encoderDirectory + QStringLiteral("/.flameshot-recording-XXXXXX"));
    if (!m_temporary->isValid()) {
        m_startupError = tr("Could not create a temporary folder in %1.")
                           .arg(m_encoderDirectory);
        return false;
    }
    const QString capturePath =
      m_temporary->filePath(QStringLiteral("capture.") + options.container);
    if (m_audioRequested && !startAudio(ffmpeg, options)) {
        m_temporary.reset();
        return false;
    }
    if (!m_backend.start(ffmpeg, region, capturePath, options)) {
        m_audio.stop();
        m_audioStarted = false;
        m_temporary.reset();
        if (m_startupError.isEmpty() && !m_backend.lastError().isEmpty()) {
            m_startupError = m_backend.lastError();
        }
        return false;
    }
    return true;
}

void CaptureRecordingSession::handleAudioFailure(const QString& message)
{
    if (m_cancelled || m_failureReported) return;
    m_failureReported = true;
    const auto current = state();
    if (current == X11RecordingBackend::State::Finalizing) {
        // The encoder is already closing the container. Killing it here would
        // leave an unplayable file, so report the audio problem and let the
        // video be finalized and published without sound (review F6).
        m_audioStarted = false;
        m_audio.stop();
        emit warning(tr("Audio stopped before the recording was complete (%1). "
                        "The video is saved without sound.")
                       .arg(message));
        return;
    }
    m_audio.stop();
    // A requested source must never disappear silently: stop the video and
    // report why.
    if (current != X11RecordingBackend::State::Finished &&
        current != X11RecordingBackend::State::Idle &&
        current != X11RecordingBackend::State::Failed) {
        m_backend.abort(message);
    }
    emit failed(message + keepTemporaryFiles());
}

bool CaptureRecordingSession::startAudio(const QString& ffmpeg,
                                         const RecordingOptions& options)
{
    const QString startupError =
      audioStartupError(options.audio, Audio::probeCapabilities(ffmpeg),
                        options.container);
    if (!startupError.isEmpty()) {
        m_startupError = startupError;
        return false;
    }
    QList<AudioSourceSpec> specs;
    QString error;
    if (!Audio::resolveRequestedSources(options.audio, &specs, &error)) {
        m_startupError = error;
        return false;
    }
    const int sampleRate = Audio::sampleRateForQuality(options.audio.quality);
    m_startingAudio = true;
    const bool started =
      m_audio.start(ffmpeg, specs, sampleRate, m_temporary->path(), &error);
    m_startingAudio = false;
    if (!started) {
        m_startupError = error;
        return false;
    }
    m_audioStarted = true;
    if (!m_pendingAudioFailure.isEmpty()) {
        const QString pending = m_pendingAudioFailure;
        m_pendingAudioFailure.clear();
        handleAudioFailure(pending);
    }
    return true;
}

bool CaptureRecordingSession::togglePause()
{
    if (m_finishing || m_cancelled) return false;
    return state() == X11RecordingBackend::State::Recording ? m_backend.pause()
         : state() == X11RecordingBackend::State::Paused ? m_backend.resume() : false;
}

void CaptureRecordingSession::finish(Action action)
{
    if (m_finishing || m_cancelled) return;
    m_finishing = true;
    m_action = action;
    m_backend.stop();
}

void CaptureRecordingSession::cancel()
{
    if (m_cancelled) return;
    m_cancelled = true;
    m_muxer.cancel();
    m_audio.stop();
    disconnect(&m_backend, nullptr, this, nullptr);
    if (state() != X11RecordingBackend::State::Finished &&
        state() != X11RecordingBackend::State::Idle &&
        state() != X11RecordingBackend::State::Failed)
        m_backend.abort(tr("Recording cancelled."));
}

qint64 CaptureRecordingSession::activeMilliseconds() const
{
    return m_elapsed + (m_running ? m_segment.elapsed() : 0);
}

void CaptureRecordingSession::muxAudio(const QString& videoPath)
{
    if (!m_temporary) {
        publish(videoPath);
        return;
    }
    AudioMuxer::Request request;
    request.ffmpeg = m_ffmpeg;
    request.videoPath = videoPath;
    request.container = m_container;
    request.codec = Audio::audioCodecForContainer(m_container);
    request.bitrateKbps = m_audioOptions.bitrateKbps;
    request.outputPath =
      m_temporary->filePath(QStringLiteral("mixed.") + m_container);
    for (int index = 0; index < m_audio.sourceCount(); ++index) {
        if (m_audio.capturedBytes(index) <= 0) continue;
        AudioMuxer::Input input;
        input.path = m_audio.filePath(index);
        input.sampleRate = m_audio.sampleRate();
        input.channels = m_audio.channels();
        request.inputs.append(input);
    }
    if (request.inputs.isEmpty()) {
        // Explicit, never silent: the request could not be honoured because no
        // audio actually arrived from the selected sources.
        emit warning(tr("No audio was captured from the selected sources, so "
                        "the recording was saved without sound."));
        publish(videoPath);
        return;
    }
    QString error;
    if (!AudioMuxer::validate(request, &error)) {
        m_failureReported = true;
        emit failed(error + keepTemporaryFiles());
        return;
    }
    // A failed start() already reported the reason through AudioMuxer::failed.
    m_muxer.start(request);
}

void CaptureRecordingSession::publish(const QString& path)
{
    if (qEnvironmentVariableIsSet("FLAMESHOT_DEBUG_COPY")) {
        qInfo("session publish: action %d, cancelled %d, from '%s', staged name "
              "'%s'",
              int(m_action),
              m_cancelled ? 1 : 0,
              qUtf8Printable(path),
              qUtf8Printable(m_stagingFile));
    }
    if (m_cancelled) return;
    const QString published =
      m_action == Action::Copy ? moveToStaging(path) : moveToDestination(path);
    if (qEnvironmentVariableIsSet("FLAMESHOT_DEBUG_COPY")) {
        qInfo("session publish: result '%s'", qUtf8Printable(published));
    }
    if (published.isEmpty()) {
        if (m_temporary) m_temporary->setAutoRemove(false);
        const QString reason =
          m_publishError.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m_publishError);
        if (m_action == Action::Copy) {
            emit failed(tr("Could not put the recording on the clipboard%1. The "
                           "completed file is available at %2.")
                          .arg(reason, path));
        } else {
            emit failed(tr("Could not save the recording to %1%2. The completed "
                           "file is available at %3.")
                          .arg(m_destination, reason, path));
        }
        return;
    }
    emit saved(published, m_action);
}

QString CaptureRecordingSession::moveToDestination(const QString& path)
{
    // The folder is created only now, at publish time. A recording that is
    // copied instead of saved therefore never creates it, and a folder that
    // turns out to be unusable delays the failure to the moment the user asked
    // for a save instead of blocking a recording that may be copied.
    const QFileInfo target(m_destination);
    const QString directory = target.absolutePath();
    const QString base = target.completeBaseName().isEmpty()
                           ? QStringLiteral("recording")
                           : target.completeBaseName();
    const QString suffix = target.suffix();
    QString error;
    // No existence scan: the name is chosen and published in one atomic
    // no-replace step, so a file that appears between choosing the name and
    // publishing it is never overwritten -- the next numbered name is used
    // instead (see publishNoReplace()).
    const QString published = publishInto(path, directory, base, suffix, &error);
    if (published.isEmpty()) {
        m_publishError = error.isEmpty() ? tr("the file could not be published")
                                         : error;
    }
    return published;
}

QString CaptureRecordingSession::moveToStaging(const QString& path)
{
    if (m_stagingFile.isEmpty()) return {};
    const QString staged = QFileInfo(m_stagingFile).absoluteFilePath();
    if (QFileInfo(path).absoluteFilePath() == staged) {
        // Already in place (a caller that named the encoder's own file).
        return staged;
    }
    const QFileInfo target(staged);
    if (!QDir().mkpath(target.absolutePath())) {
        m_publishError = tr("the staging folder %1 could not be created")
                           .arg(target.absolutePath());
        return {};
    }
    // The caller hands out a fresh, unique name for the clipboard, and that name
    // must be the one the clipboard offers, so no numbered alternative is
    // invented here. The publish is still an atomic no-replace rename: a file
    // that appeared under that name is never replaced, and the failure is
    // reported instead of silently handing out a different file.
    const NoReplace result = publishNoReplace(path, staged);
    if (result.status == NoReplace::Published) return staged;
    m_publishError = result.status == NoReplace::Exists
                       ? tr("the staging file %1 already exists, so it was not "
                            "replaced")
                           .arg(target.fileName())
                       : (result.error == EXDEV
                            ? tr("the staging file could not be moved onto the "
                                 "recording filesystem")
                            : systemMessage(result.error));
    if (m_publishError.isEmpty())
        m_publishError = tr("the file could not be published");
    return {};
}

QString CaptureRecordingSession::keepTemporaryFiles()
{
    if (!m_temporary || !m_temporary->isValid()) return {};
    const QDir dir(m_temporary->path());
    const auto files = dir.entryInfoList(QDir::Files, QDir::Name);
    QFileInfo chosen;
    for (const auto& file : files) {
        if (file.size() <= 0) continue;
        // Raw PCM is an internal intermediate; never advertise it as the
        // recording the user can play (review F10).
        if (file.suffix().compare(QLatin1String("pcm"), Qt::CaseInsensitive) == 0)
            continue;
        if (file.suffix().compare(m_container, Qt::CaseInsensitive) == 0) {
            chosen = file;
            break;
        }
        if (!chosen.exists()) chosen = file;
    }
    if (!chosen.exists()) return {};
    m_temporary->setAutoRemove(false);
    return tr("\nThe partial recording was kept at %1.")
      .arg(chosen.absoluteFilePath());
}
}
