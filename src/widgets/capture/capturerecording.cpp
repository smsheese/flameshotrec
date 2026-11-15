// SPDX-License-Identifier: GPL-3.0-or-later
#include "capturewidget.h"
#include "core/flameshot.h"
#include "core/flameshotdaemon.h"
#include "recording/annotationpolicy_confighandler.h"
#include "recording/ffmpegdiscovery.h"
#include "recording/videotransfer.h"
#include "recording/x11input.h"
#include "tools/launcher/applauncherwidget.h"
#include "utils/capturegeometry.h"
#include "utils/filenamehandler.h"
#include "utils/screenshotsaver.h"
#include "widgets/capture/notifierbox.h"
#include "widgets/capture/overlaymessage.h"
#include "widgets/capture/recordingclipboard.h"
#include "widgets/capture/recordingclipboarddaemon.h"
#include "widgets/capture/x11livedesktop.h"
#include "widgets/panel/utilitypanel.h"
#include "widgets/panel/sidepanelwidget.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QPainter>
#include <QProgressDialog>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QToolTip>
#include <algorithm>

namespace {
// Width of the boundary drawn immediately outside the recorded region. Keeping
// it outside means neither it nor the 1px selection border nor the handles are
// part of the encoded video.
constexpr int kRecordingBoundaryWidth = 2;

// Repaint period while an annotation is fading. Outside the fade window nothing
// on the overlay changes, so the timer is not running at all then (review R3).
constexpr int kAnnotationFadeIntervalMs = 16;

// How often the desktop under a live effect annotation (invert, pixelate) is
// compared with the last sample while it is on screen. The tick only repaints
// when the content changed, so a static screen still costs no repaint (review
// R3) while a changing one is followed.
constexpr int kLiveEffectPollMs = 100;

// The policy tool behind an overlay tool. `Recording::captureToolType()` is the
// inverse mapping; both live in the configuration module's header so the two
// sides cannot drift.

// Tools that draw on the live overlay. Their painting is captured by the
// recording, so they are the only annotation tools enabled while a session is
// running. Screenshot-only actions (export, pin) stay disabled; invert and
// pixelate are annotation tools here because while recording they read the live
// desktop instead of the capture-time screenshot.
bool annotationTool(CaptureTool::Type type)
{
    switch (type) {
        case CaptureTool::TYPE_PENCIL:
        case CaptureTool::TYPE_DRAWER:
        case CaptureTool::TYPE_ARROW:
        case CaptureTool::TYPE_RECTANGLE:
        case CaptureTool::TYPE_CIRCLE:
        case CaptureTool::TYPE_MARKER:
        case CaptureTool::TYPE_SELECTION:
        case CaptureTool::TYPE_TEXT:
        case CaptureTool::TYPE_CIRCLECOUNT:
        case CaptureTool::TYPE_INVERT:
        case CaptureTool::TYPE_PIXELATE:
            return true;
        default:
            return false;
    }
}

void notifyVideo(const QString& title, const QString& message, bool error = false)
{
    auto* box = new QMessageBox(error ? QMessageBox::Critical : QMessageBox::Information,
                               title, message, QMessageBox::Ok);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
}

void uploadSavedCapture(const QString& path)
{
    VideoTransferOptions options = VideoTransfer::configuredOptions();
    if (!options.enabled) {
        notifyVideo(QObject::tr("Capture saved"), QObject::tr("Saved to %1. Configure FTP/SFTP in Configuration > Video before uploading.").arg(path));
        return;
    }
    auto* progress = new QProgressDialog(QObject::tr("Uploading saved capture…"), QString(), 0, 0);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setCancelButton(nullptr);
    progress->setMinimumDuration(0);
    progress->show();
    if (options.protocol == VideoTransferOptions::Protocol::FTP) {
        bool accepted = false;
        options.password = QInputDialog::getText(progress, QObject::tr("FTP upload"), QObject::tr("Password:"),
                                                QLineEdit::Password, {}, &accepted);
        if (!accepted) { progress->close(); return; }
    }
    auto* transfer = new VideoTransfer(progress);
    QObject::connect(transfer, &VideoTransfer::failed, progress, [progress, path](const QString& message) {
        notifyVideo(QObject::tr("Upload failed"), QObject::tr("The capture is saved at %1.\n%2").arg(path, message), true);
        progress->close();
    });
    QObject::connect(transfer, &VideoTransfer::succeeded, progress, [progress, path]() {
        notifyVideo(QObject::tr("Capture uploaded"), QObject::tr("Local file: %1").arg(path));
        progress->close();
    });
    transfer->upload(path, options);
}
}

void CaptureWidget::startRecording()
{
    if (m_videoSession) { m_videoSession->togglePause(); return; }
    if (QGuiApplication::platformName() != QLatin1String("xcb") || !Recording::hasX11Compositor()) {
        QMessageBox::warning(this, tr("Recording unavailable"),
                             tr("Recording currently requires an X11 session with desktop compositing enabled."));
        return;
    }
    if (m_selection->geometry().isEmpty() || !m_selection->isVisible()) return;
    if (m_buttonHandler->buttonsAreInside()) {
        QMessageBox::warning(this, tr("Recording unavailable"),
                             tr("Leave enough space outside the selected area for the capture tools."));
        return;
    }
    const QString ffmpeg = Recording::findFFmpeg(m_config.recordingFfmpegPath());
    if (ffmpeg.isEmpty()) {
        QMessageBox::warning(this, tr("Recording unavailable"), tr("FFmpeg was not found. Set its executable in Configuration > Video."));
        return;
    }
    m_videoRegion = m_selection->geometry();
    const QRect globalRegion(mapToGlobal(m_videoRegion.topLeft()),
                             m_videoRegion.size());
    QScreen* screen = QGuiApplication::screenAt(globalRegion.center());
    if (!screen || !screen->geometry().contains(globalRegion)) {
        QMessageBox::warning(this, tr("Recording unavailable"), tr("Select a recording area within one monitor."));
        return;
    }
    // FFmpeg's X11 offsets are physical pixels. A mixed-scale desktop cannot
    // be mapped by multiplying a single Qt desktop rectangle.
    const qreal scale = screen->devicePixelRatio();
    for (QScreen* candidate : QGuiApplication::screens()) {
        if (!qFuzzyCompare(candidate->devicePixelRatio(), scale)) {
            QMessageBox::warning(this, tr("Recording unavailable"), tr("Recording across a desktop with different monitor scale factors is not supported yet."));
            return;
        }
    }
    // Where a *save* would put the recording. Nothing is created here: the
    // folder is made when the file is actually published, so a recording that
    // is copied instead never touches (or fails on) the configured folder.
    QString directory = m_config.recordingVideoPath();
    if (directory.startsWith(QLatin1String("~/"))) directory = QDir::home().filePath(directory.mid(2));
    if (directory.isEmpty()) directory = QDir::home().filePath(QStringLiteral("Pictures"));
    QString name = FileNameHandler().parseFilename(m_config.recordingFilenamePattern());
    name = QFileInfo(name).fileName();
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) name = QStringLiteral("recording");
    const QString suffix = m_config.recordingContainer();
    const QString destination = QDir(directory).filePath(name + QLatin1Char('.') + suffix);
    // The clipboard gets its own file in a private staging directory. Handing
    // out a fresh name means a copy can never overwrite a file an earlier paste
    // is still reading. The directory is created securely or refused, never
    // adopted from whatever was already at that path.
    QString stagingError;
    Recording::Clipboard::ensureStagingDirectory(&stagingError);
    const QString stagingFile = Recording::Clipboard::stagingFile(suffix);
    if (stagingFile.isEmpty()) {
        const QString stagingLocation =
          Recording::Clipboard::activeStagingDirectory();
        QMessageBox::warning(this,
                             tr("Recording unavailable"),
                             tr("Could not create the private recording staging "
                                "folder %1: %2")
                               .arg(stagingLocation.isEmpty()
                                      ? Recording::Clipboard::stagingDirectory()
                                      : stagingLocation,
                                    stagingError));
        return;
    }
    // A copied recording has to stay on the clipboard after this process exits,
    // so the production hand-off to the clipboard daemon is installed once per
    // recording (before any Copy can finalize). Without it the URI is lost when
    // the overlay closes.
    Recording::Clipboard::installDaemonHandOff();
    // Retention runs over the module-owned staging directories and nothing else.
    // The usual path is passed first because a sweep of it also collects the
    // marked siblings that had to be used when that name was taken; the directory
    // this process actually uses is passed second, so it is swept even then. A
    // directory this module cannot prove it created -- the configured video
    // folder, a legacy staging folder, a user's own directory -- is refused by
    // both calls, so a name prefix is never an ownership claim.
    Recording::Clipboard::sweepStaging(Recording::Clipboard::stagingDirectory());
    Recording::Clipboard::sweepStaging(
      Recording::Clipboard::activeStagingDirectory());

    auto* session = new Recording::CaptureRecordingSession(this);
    m_videoSession = session;
    m_videoFinalizing = false;
    // The lifetime policy for this recording: one snapshot read here, never in
    // a paint or timer callback. Every per-tool hold/fade and the counter group
    // mode come from it; tools left on "Default" inherit the global marking
    // hold/fade, so a user who never opened the section keeps today's timing.
    m_videoPolicy = Recording::loadAnnotationPolicyFromConfig();
    m_videoCounterGroupStamp = -1;
    // Live desktop content for invert and pixelate: only a compositing manager
    // keeps the windows below the overlay readable. Without one the effects are
    // unavailable for this recording instead of painting a frozen desktop, and
    // the user is told before the session starts.
    m_videoLiveSampling = LiveDesktop::compositingActive();
    m_videoLifetimes.clear();
    clearRecordingSamples();
    if (m_videoLiveSampling) {
        if (m_videoSampler.isNull()) {
            auto* sampler = new LiveDesktop::Sampler(this);
            m_videoSampler = sampler;
            connect(sampler,
                    &LiveDesktop::Sampler::sampled,
                    this,
                    [this](quint64 id, const LiveDesktop::Sample& sample) {
                        EffectSample effect;
                        effect.image = sample.image;
                        effect.valid = sample.usable();
                        effect.failure = sample.failure;
                        // The sampler reports the *physical* rectangle it
                        // rebuilt; handleLiveEffectSample() validates it against
                        // the annotation's current rectangle before the overlay
                        // coordinates are attached. Dropping it here made every
                        // asynchronous result look like it belonged to another
                        // rectangle, so the effect never painted at all.
                        effect.rect = sample.rect;
                        effect.covered = sample.covered;
                        handleLiveEffectSample(id, effect);
                    });
        }
    } else {
        m_videoSampler.clear();
        // Reported after the recording, not from a dialog while the encoder is
        // already running: a window over the recorded region would be encoded
        // into the video. The disabled annotation buttons say the same thing at
        // the point of use (see updateRecordingUi()).
        recordVideoWarning(
          tr("Invert and pixelate were unavailable for this recording: they "
             "need a compositing desktop to read what is below the overlay."));
    }
    const qint64 startedAt = session->activeMilliseconds();
    for (auto& tool : m_captureToolObjects.captureToolObjects())
        if (tool && annotationTool(tool->type()))
            m_videoLifetimes.insert(tool, lifetimeFor(tool->type(), startedAt));
    m_recordingSession = true;
    m_panel->hide();
    if (m_sidePanel) m_sidePanel->hide();
    if (m_magnifier) m_magnifier->hide();
    m_xywhDisplay = false;
    OverlayMessage::pop();
    // Nothing this widget pops up may survive into the recording: its own
    // tooltip used to carry the encoder status, and a tooltip is a separate
    // window that the encoder captures wherever the pointer happens to be.
    setToolTip(QString());
    QToolTip::hideText();
    m_videoWarnings.clear();
    m_videoWarningLog.invalidate();
    // The selection keeps its geometry but stops painting its border and
    // handles: they would otherwise be encoded into the video.
    m_selection->setIgnoreMouse(true);
    m_selection->setRecordingFrame(true);
    for (QWidget* child : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        child->installEventFilter(this);
    updateAnnotationRefresh();
    connect(session, &Recording::CaptureRecordingSession::stateChanged, this,
            [this]() { updateRecordingUi(); update(); });
    // Encoder and audio status is a diagnostic, not something to draw: the
    // overlay covers the recorded region, so a tooltip or banner here is encoded
    // into the video. recordVideoWarning() rate-limits it to the console and the
    // finished recording reports it once, outside the recorded area.
    connect(session, &Recording::CaptureRecordingSession::warning, this,
            [this](const QString& warning) { recordVideoWarning(warning); });
    connect(session, &Recording::CaptureRecordingSession::failed, this,
            [](const QString& message) {
                if (qEnvironmentVariableIsSet("FLAMESHOT_DEBUG_COPY")) {
                    qInfo("session failed: %s", qUtf8Printable(message));
                }
            });
    connect(session, &Recording::CaptureRecordingSession::saved, this,
            [this](const QString& path, Recording::CaptureRecordingSession::Action action) {
        m_videoRefresh.stop();
        // Whatever went wrong while recording (an encoder backlog, audio that
        // stopped) is reported now, when the recorded area is closed and the
        // message cannot become part of the video.
        const QString summary = videoWarningSummary();
        const QString details =
          summary.isEmpty() ? path : path + QStringLiteral("\n\n") + summary;
        if (action == Recording::CaptureRecordingSession::Action::Open) {
            auto* launcher = new AppLauncherWidget(path);
            launcher->setAttribute(Qt::WA_DeleteOnClose);
            // Same balance as the toolbar launcher: the window that outlives the
            // capture has to hold one external-widget reference (review R4).
            QObject::connect(launcher, &QObject::destroyed,
                             Flameshot::instance(), []() {
                                 Flameshot::instance()->removeExternalWindow();
                             });
            Flameshot::instance()->addExternalWindow();
            launcher->show();
        } else if (action == Recording::CaptureRecordingSession::Action::Upload) {
            uploadSavedCapture(path);
        } else if (action == Recording::CaptureRecordingSession::Action::Copy) {
            // The file, not a screenshot: a recording copied to the clipboard is
            // pasted as a video file, and the staging file stays where it is
            // until the paste target (or something else) takes the data. The
            // clipboard is set before the overlay closes, so the ownership
            // outlives this widget;
            // Recording::Clipboard::copyFileUri() also hands the path to the
            // clipboard daemon, so it outlives this *process* on X11.
            if (Recording::Clipboard::copyFileUri(path)) {
                notifyVideo(tr("Recording copied"),
                            tr("The video file is on the clipboard: %1").arg(details));
            } else {
                notifyVideo(tr("Copy failed"),
                            tr("The recording could not be put on the clipboard. It is available at %1.").arg(path),
                            true);
            }
        } else {
            notifyVideo(tr("Recording saved"), details);
        }
        close();
    });
    auto starting = std::make_shared<bool>(true);
    auto startupError = std::make_shared<QString>();
    connect(session, &Recording::CaptureRecordingSession::failed, this,
            [this, session, starting, startupError](const QString& message) {
        if (*starting) { *startupError = message; return; }
        if (m_videoSession != session) return;
        m_videoRefresh.stop();
        m_videoSession = nullptr;
        m_recordingSession = false;
        m_videoLifetimes.clear();
        clearRecordingSamples();
        m_selection->setRecordingFrame(false);
        session->deleteLater();
        m_videoFinalizing = false;
        resetRecordingInput();
        updateRecordingUi();
        updateSelectionState();
        update();
        notifyVideo(tr("Recording failed"), message, true);
    });
    updateRecordingUi();
    repaint();
    // Let the compositor expose the live selection before capture begins.
    QTimer::singleShot(100, this, [this, session, starting, startupError, ffmpeg, globalRegion, scale, destination, stagingFile]() {
        if (m_videoSession != session) return;
        Recording::RecordingOptions options;
        options.frameRate = m_config.recordingFrameRate();
        options.codec = m_config.recordingCodec();
        options.container = m_config.recordingContainer();
        options.rateControl = m_config.recordingRateControl();
        options.bitrateKbps = m_config.recordingBitrateKbps();
        options.quality = m_config.recordingQuality();
        options.audio.systemEnabled = m_config.recordingAudioSystemEnabled();
        options.audio.microphoneEnabled = m_config.recordingAudioMicEnabled();
        options.audio.systemSource = m_config.recordingAudioSystemSource();
        options.audio.microphoneSource = m_config.recordingAudioMicSource();
        options.audio.quality = m_config.recordingAudioQuality();
        options.audio.bitrateKbps = m_config.recordingAudioBitrateKbps();
        // Physical pixels for FFmpeg's x11grab; the overlay works in logical
        // coordinates, so negative origins and scale factors are converted here.
        const QRect physical = CaptureGeometry::physicalRect(globalRegion, scale);
        if (!session->start(ffmpeg, physical, destination, stagingFile, options)) {
            m_videoSession = nullptr;
            m_recordingSession = false;
            m_videoRefresh.stop();
            m_videoLifetimes.clear();
            clearRecordingSamples();
            m_selection->setRecordingFrame(false);
            session->deleteLater();
            resetRecordingInput();
            updateRecordingUi();
            updateSelectionState();
            update();
            // Prefer the audio/video probe cause when the session recorded one.
            const QString reason =
              !session->lastError().isEmpty()
                ? session->lastError()
                : (startupError->isEmpty()
                     ? tr("Could not start FFmpeg with the selected video settings.")
                     : *startupError);
            notifyVideo(tr("Recording failed"), reason, true);
        }
        *starting = false;
    });
}

void CaptureWidget::finishRecording(Recording::CaptureRecordingSession::Action action)
{
    if (!m_videoSession || m_videoFinalizing) return;
    if (m_videoSession->state() != Recording::X11RecordingBackend::State::Recording &&
        m_videoSession->state() != Recording::X11RecordingBackend::State::Paused) return;
    commitCurrentTool();
    m_videoFinalizing = true;
    m_videoSession->finish(action);
    updateRecordingUi();
}

bool CaptureWidget::handleRecordingAction(CaptureTool::Type type)
{
    using Action = Recording::CaptureRecordingSession::Action;
    switch (type) {
        case CaptureTool::TYPE_EXIT:
        case CaptureTool::TYPE_CANCEL:
            m_videoSession->cancel(); close(); return true;
        case CaptureTool::TYPE_RECORD: m_videoSession->togglePause(); return true;
        case CaptureTool::TYPE_SAVE: finishRecording(Action::Save); return true;
        case CaptureTool::TYPE_OPEN_APP: finishRecording(Action::Open); return true;
        case CaptureTool::TYPE_RECORD_UPLOAD: finishRecording(Action::Upload); return true;
        // Copy used to be swallowed here, which left the toolbar button and its
        // shortcut dead during a recording. It now finalizes the recording into
        // the private staging directory and puts the file URI on the clipboard.
        case CaptureTool::TYPE_COPY: finishRecording(Action::Copy); return true;
        case CaptureTool::TYPE_MOVESELECTION:
            if (m_activeButton) uncheckActiveTool();
            // Leaving the counter tool (or any drawing tool) ends the group, so
            // the counter bubbles start their hold here instead of staying
            // pinned forever.
            enforceCounterGroup();
            updateRecordingInput();
            return true;
#ifdef ENABLE_IMGUR
        case CaptureTool::TYPE_IMAGEUPLOADER: finishRecording(Action::Upload); return true;
#endif
        default:
            if (m_videoFinalizing) return true;
            // Invert and pixelate read the desktop below the overlay, which
            // needs a compositing manager. Without one they are refused here as
            // well as disabled in the toolbar, so a shortcut cannot reach them
            // either, and the user gets the reason instead of a frozen patch.
            if (liveEffectTool(type) && !m_videoLiveSampling) {
                reportLiveEffectFailure(
                  tr("this desktop does not composite the windows below the "
                     "overlay"));
                return true;
            }
            // Swallow screenshot-only actions instead of enabling them: they
            // would export, pin or transform a screenshot that is not being
            // taken. Drawing and its editing controls are handled normally.
            return !(annotationTool(type) || type == CaptureTool::TYPE_UNDO ||
                     type == CaptureTool::TYPE_REDO || type == CaptureTool::TYPE_SIZEINCREASE ||
                     type == CaptureTool::TYPE_SIZEDECREASE);
    }
}

void CaptureWidget::updateRecordingUi()
{
    const bool active = !m_videoSession.isNull();
    const auto state = active ? m_videoSession->state() : Recording::X11RecordingBackend::State::Idle;
    // Between the user pressing Record and the encoder accepting its first
    // frame the session is still Idle from the widget's point of view; never
    // present that as "recording" (review F3).
    const bool preparing = active && (state == Recording::X11RecordingBackend::State::Idle ||
                                      state == Recording::X11RecordingBackend::State::Preparing);
    for (auto* button : findChildren<CaptureToolButton*>()) {
        const auto type = button->tool()->type();
        if (active) {
            // Copy is an action on the recording now, so its button has to be
            // usable: the handler finalizes the session and puts the file URI on
            // the clipboard. Leaving it out here made the toolbar button (and
            // the shortcut that goes through it) dead while recording.
            const bool action = type == CaptureTool::TYPE_RECORD || type == CaptureTool::TYPE_SAVE ||
                                type == CaptureTool::TYPE_COPY ||
                                type == CaptureTool::TYPE_EXIT || type == CaptureTool::TYPE_OPEN_APP ||
                                type == CaptureTool::TYPE_RECORD_UPLOAD || type == CaptureTool::TYPE_MOVESELECTION;
#ifdef ENABLE_IMGUR
            const bool upload = type == CaptureTool::TYPE_IMAGEUPLOADER;
#else
            const bool upload = false;
#endif
            button->setEnabled(action || upload || annotationTool(type) || type == CaptureTool::TYPE_UNDO ||
                               type == CaptureTool::TYPE_REDO || type == CaptureTool::TYPE_SIZEINCREASE ||
                               type == CaptureTool::TYPE_SIZEDECREASE);
            // A live effect needs the desktop below the overlay. Saying so on the
            // button is better than a patch that silently shows the frozen
            // capture-time screenshot.
            if (liveEffectTool(type) && !m_videoLiveSampling) {
                button->setEnabled(false);
                button->setToolTip(tr("Needs a compositing desktop to read what is below the overlay"));
            }
            if (m_videoFinalizing && type != CaptureTool::TYPE_EXIT) button->setEnabled(false);
            if (type == CaptureTool::TYPE_MOVESELECTION) button->setToolTip(tr("Interact with the desktop"));
            if (type == CaptureTool::TYPE_RECORD) {
                QPixmap icon(32, 32); icon.fill(Qt::transparent);
                QPainter p(&icon); p.setPen(Qt::NoPen); p.setBrush(Qt::white);
                if (state == Recording::X11RecordingBackend::State::Paused) {
                    p.drawPolygon(QPolygon({QPoint(10, 6), QPoint(26, 16), QPoint(10, 26)}));
                    button->setToolTip(tr("Resume recording"));
                } else {
                    p.drawRect(8, 6, 5, 20); p.drawRect(19, 6, 5, 20);
                    button->setToolTip(preparing ? tr("Preparing recording…") : tr("Pause recording"));
                }
                button->setIcon(QIcon(icon));
                button->setEnabled(state == Recording::X11RecordingBackend::State::Recording || state == Recording::X11RecordingBackend::State::Paused);
            }
        } else {
            button->setEnabled(type != CaptureTool::TYPE_RECORD_UPLOAD || m_config.recordingTransferEnabled());
            if (type == CaptureTool::TYPE_RECORD) { button->setIcon(button->icon()); button->setToolTip(button->tool()->description()); }
        }
    }
    updateSelectionState();
    updateRecordingInput();
}

void CaptureWidget::paintRecording(QPainter& painter)
{
    // Outside the recorded region the overlay is fully transparent: the
    // desktop stays live and the other monitors are neither dimmed nor
    // repainted, so they keep their normal appearance during a recording.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // The boundary is drawn in the strip just outside the recorded region, so
    // it stays visible on screen but is never part of the encoded frames.
    const QRegion boundary =
      CaptureGeometry::boundaryRing(m_videoRegion, kRecordingBoundaryWidth);
    if (!boundary.isEmpty()) {
        painter.save();
        painter.setClipRegion(boundary);
        painter.fillRect(boundary.boundingRect(), m_contrastUiColor);
        painter.restore();
    }

    painter.save();
    painter.setClipRect(m_videoRegion);
    const qint64 now = m_videoSession->activeMilliseconds();
    bool stamped = false;
    for (auto tool : m_captureToolObjects.captureToolObjects()) {
        if (!tool || !annotationTool(tool->type())) continue;
        // An annotation without a slot was just added (or restored by undo):
        // start its lifetime now instead of letting it expire on the first
        // frame, which made strokes vanish as soon as they were released.
        if (!m_videoLifetimes.contains(tool.data())) {
            m_videoLifetimes.insert(tool.data(), lifetimeFor(tool->type(), now));
            stamped = true;
        }
        const AnnotationLifetime lifetime = m_videoLifetimes.value(tool.data());
        const qint64 age = now - lifetime.startMs;
        // A held counter group keeps full strength regardless of its age.
        const qreal opacity =
          lifetime.pinned
            ? 1.0
            : CaptureGeometry::annotationOpacity(
                age, lifetime.holdMs, lifetime.fadeMs);
        if (qEnvironmentVariableIsSet("FLAMESHOT_DEBUG_ANNOTATIONS")) {
            qInfo("annotation: type %d lifetime %s age %lld hold %d fade %d "
                  "opacity %.2f valid %d edit %d",
                  int(tool->type()),
                  m_videoLifetimes.contains(tool.data()) ? "yes" : "no",
                  static_cast<long long>(age),
                  lifetime.holdMs,
                  lifetime.fadeMs,
                  opacity,
                  tool->isValid() ? 1 : 0,
                  tool->editMode() ? 1 : 0);
        }
        if (opacity <= 0.0) continue;
        painter.setOpacity(opacity);
        painter.setRenderHint(QPainter::Antialiasing);
        if (liveEffectTool(tool->type())) {
            // Invert and pixelate read the desktop through the window stack on
            // every frame, never a frozen screenshot and never their own patch
            // (see liveDesktopSample()).
            paintLiveEffect(painter, tool.data());
        } else {
            // Painted straight into the overlay: a per-annotation tile sized
            // from boundingRect() clipped the text tool, whose painted area is
            // only computed inside process().
            tool->process(painter, m_context.origScreenshot);
        }
        painter.setOpacity(1.0);
    }
    if (m_activeTool && m_mouseIsClicked && annotationTool(m_activeTool->type())) {
        // The in-progress annotation is sampled like a committed one, so the
        // preview of an effect follows the desktop while it is dragged.
        if (liveEffectTool(m_activeTool->type())) {
            paintLiveEffect(painter, m_activeTool.data());
        } else {
            m_activeTool->process(painter, m_context.origScreenshot);
        }
    }
    painter.restore();
    if (stamped) {
        updateAnnotationRefresh();
    }
}

void CaptureWidget::refreshRecordingAnnotations()
{
    if (!m_videoSession) return;
    const qint64 now = m_videoSession->activeMilliseconds();
    bool removed = false;
    bool fullRepaint = false;
    QRegion dirty;
    for (int i = m_captureToolObjects.size() - 1; i >= 0; --i) {
        auto tool = m_captureToolObjects.at(i);
        if (!tool) continue;
        if (!m_videoLifetimes.contains(tool.data()))
            m_videoLifetimes.insert(tool.data(), lifetimeFor(tool->type(), now));
        const AnnotationLifetime lifetime = m_videoLifetimes.value(tool.data());
        const QRect bounds = tool->boundingRect()
                               .normalized()
                               .adjusted(-3, -3, 3, 3)
                               .intersected(m_videoRegion);
        if (lifetime.pinned) {
            // Held for as long as the group is active: nothing expires, nothing
            // fades, and the group's clock starts when the tool is switched
            // (enforceCounterGroup()).
            continue;
        }
        const qint64 age = now - lifetime.startMs;
        if (CaptureGeometry::annotationExpired(
              age, lifetime.holdMs, lifetime.fadeMs)) {
            m_videoLifetimes.remove(tool.data());
            m_videoEffectSamples.remove(tool.data());
            m_videoSampleRequests.remove(tool.data());
            m_captureToolObjects.removeAt(i);
            // The stale annotation object has to go with the list entry: it is
            // a child of this widget, so dropping only the entry leaked one
            // object per annotation for the rest of the recording (review R5).
            // The undo snapshots below are cleared in the same pass, so no
            // snapshot keeps a reference to it (they are QPointer, and would
            // become null rather than dangle).
            tool->deleteLater();
            removed = true;
            if (!bounds.isEmpty()) dirty += bounds;
        } else if (age > lifetime.holdMs) {
            // Only annotations inside their fade window are changing; during the
            // hold the overlay has nothing to repaint (review R3).
            if (!bounds.isEmpty()) {
                dirty += bounds;
            } else {
                // A tool that has not painted yet (text computes its area inside
                // process()) cannot be located: repaint everything instead of
                // leaving it stuck at full opacity.
                fullRepaint = true;
            }
        }
        if (liveEffectTool(tool->type())) {
            // A live effect that is on screen -- held *or* fading -- has to
            // follow the desktop. Ask the background sampler for the
            // annotation's own rectangle (plus the pixelate fringe) instead of
            // reading X here: the walk happens on the sampler thread, so the
            // overlay keeps responding while a busy stack is sampled. The result
            // only repaints when the pixels really changed, so a static screen
            // still costs no repaint (review R3), and a fading effect never
            // freezes on the last sample it happened to get.
            const QRect effectRect = tool->boundingRect()
                                       .normalized()
                                       .adjusted(-1, -1, 1, 1)
                                       .intersected(m_videoRegion);
            if (!effectRect.isEmpty() &&
                now - m_videoSampleRequests.value(tool.data(), -kLiveEffectPollMs) >=
                  kLiveEffectPollMs) {
                m_videoSampleRequests.insert(tool.data(), now);
                requestLiveEffectSample(tool.data(), effectRect);
            }
        }
    }
    if (removed) {
        m_undoStack.clear();
        m_captureToolObjectsBackup.clear();
        m_panel->setActiveLayer(-1);
        updateLayersPanel();
    }
    // Forget annotations that left the overlay (removed or undone) so the
    // timeline and the live samples do not grow for the length of a
    // recording.
    if (!m_videoLifetimes.isEmpty()) {
        QSet<CaptureTool*> live;
        const auto objects = m_captureToolObjects.captureToolObjects();
        for (const auto& tool : objects) {
            if (tool) {
                live.insert(tool.data());
            }
        }
        const auto stampedTools = m_videoLifetimes.keys();
        for (CaptureTool* tool : stampedTools) {
            if (!live.contains(tool)) {
                m_videoLifetimes.remove(tool);
                m_videoEffectSamples.remove(tool);
                m_videoSampleRequests.remove(tool);
                if (m_videoSampler) {
                    m_videoSampler->forget(effectSampleId(tool));
                }
                m_videoSampleOwners.remove(effectSampleId(tool));
            }
        }
    } else {
        m_videoEffectSamples.clear();
        m_videoSampleOwners.clear();
    }
    if (removed || fullRepaint) {
        // The undo stack was reset, so anything could have disappeared.
        update();
    } else if (!dirty.isEmpty()) {
        // Repaint the fading or changing annotations only: never the whole
        // desktop.
        update(dirty);
    }
    updateAnnotationRefresh();
}

void CaptureWidget::updateAnnotationRefresh()
{
    if (m_videoSession.isNull() || m_videoLifetimes.isEmpty()) {
        m_videoRefresh.stop();
        return;
    }
    const qint64 now = m_videoSession->activeMilliseconds();
    bool fading = false;
    bool following = false;
    qint64 nextWake = -1;
    for (auto it = m_videoLifetimes.cbegin(); it != m_videoLifetimes.cend();
         ++it) {
        if (it.value().pinned) {
            continue;
        }
        const qint64 age = now - it.value().startMs;
        if (age >= it.value().holdMs) {
            fading = true;
            break;
        }
        if (m_videoLiveSampling && it.key() &&
            liveEffectTool(it.key()->type())) {
            following = true;
        }
        const qint64 until = it.value().holdMs - age;
        if (nextWake < 0 || until < nextWake) {
            nextWake = until;
        }
    }
    qint64 interval = 0;
    if (fading) {
        interval = kAnnotationFadeIntervalMs;
    } else if (following) {
        // Poll the desktop under the effect annotations instead of repainting:
        // the poll tick only repaints when the content changed.
        interval = kLiveEffectPollMs;
    } else if (nextWake >= 0) {
        // Sleep through the whole hold and wake when the fade begins.
        interval = nextWake;
    }
    if (interval <= 0) {
        m_videoRefresh.stop();
        return;
    }
    const int bounded = int(qBound<qint64>(qint64(1), interval, qint64(60000)));
    if (m_videoRefresh.interval() != bounded) {
        m_videoRefresh.setInterval(bounded);
    }
    if (!m_videoRefresh.isActive()) {
        m_videoRefresh.start();
    }
}

void CaptureWidget::ensureLiveCanvas()
{
    if (!m_videoLiveCanvas.isNull() && m_videoLiveCanvas.size() == size()) {
        return;
    }
    // One desktop-sized carrier per recording, created on first use: the tools
    // address their source in overlay coordinates, so the live pixels have to
    // be uploaded into a pixmap of that geometry.
    m_videoLiveCanvas = QPixmap(size());
    // Transparent, not black: anything a sample does not cover has to show the
    // desktop, never a black rectangle left over from an earlier patch.
    m_videoLiveCanvas.fill(Qt::transparent);
}

bool CaptureWidget::liveEffectTool(CaptureTool::Type type)
{
    return type == CaptureTool::TYPE_INVERT || type == CaptureTool::TYPE_PIXELATE;
}

bool CaptureWidget::paintEffectFromSample(QPainter& painter,
                                          CaptureTool* tool,
                                          const QRect& sampleRect,
                                          const EffectSample& sample)
{
    Q_UNUSED(sampleRect)
    if (tool == nullptr || !sample.valid || sample.covered.isEmpty() ||
        sample.image.isNull()) {
        return false;
    }
    ensureLiveCanvas();
    if (m_videoLiveCanvas.isNull()) {
        return false;
    }
    // Source composition, not SourceOver: the canvas is reused, and an old patch
    // must not shine through a new sample.
    QPainter canvas(&m_videoLiveCanvas);
    canvas.setCompositionMode(QPainter::CompositionMode_Source);
    canvas.drawImage(sample.covered.topLeft(), sample.image);
    canvas.end();
    // The tool paints from the canvas itself, so InvertTool and PixelateTool
    // keep their single implementation (the secure fringe included).
    tool->process(painter, m_videoLiveCanvas);
    return true;
}

void CaptureWidget::paintLiveEffect(QPainter& painter, CaptureTool* tool)
{
    if (!tool) return;
    const QRect bounds =
      tool->boundingRect().normalized().intersected(m_videoRegion);
    if (bounds.isEmpty()) return;
    if (!m_videoLiveSampling) {
        reportLiveEffectFailure(
          tr("this desktop does not composite the windows below the overlay"));
        return;
    }
    // One pixel of context around the annotation: the pixelate effect samples
    // the fringe just outside its selection, and that fringe has to be live
    // content too.
    const QRect sampleRect =
      bounds.adjusted(-1, -1, 1, 1).intersected(m_videoRegion);
    // Paint only from a sample the background sampler already produced. A paint
    // event must never block on an X round trip: the walk can take up to the
    // sampling budget, and while this thread is blocked the overlay cannot
    // process its own input -- a drag that is being drawn would lose points, and
    // on a busy server the effect would stutter. If nothing covers this
    // rectangle yet, one is requested and this frame paints no patch; the next
    // tick has it.
    const EffectSample sample = m_videoEffectSamples.value(tool);
    if (!sample.valid || sample.rect != sampleRect) {
        requestLiveEffectSample(tool, sampleRect);
        return;
    }
    if (paintEffectFromSample(painter, tool, sampleRect, sample)) {
        return;
    }
    // Never a silent substitute: the patch is left out (the recording shows the
    // real desktop) and the reason is shown to the user once.
    reportLiveEffectFailure(
      sample.failure.isEmpty()
        ? tr("the desktop below the effect could not be read")
        : sample.failure);
}

quint64 CaptureWidget::effectSampleId(CaptureTool* tool)
{
    // Stable for the lifetime of the annotation object and never zero.
    return quint64(reinterpret_cast<quintptr>(tool)) | 1U;
}

void CaptureWidget::requestLiveEffectSample(CaptureTool* tool,
                                            const QRect& overlayRect)
{
    if (tool == nullptr || m_videoSampler.isNull() || overlayRect.isEmpty()) {
        return;
    }
    const quint64 id = effectSampleId(tool);
    m_videoSampleOwners.insert(id, tool);
    m_videoSampleRects.insert(id, overlayRect);
    const qreal scale = devicePixelRatioF();
    const QRect global(mapToGlobal(overlayRect.topLeft()), overlayRect.size());
    m_videoSampler->request(
      id, winId(), CaptureGeometry::physicalRect(global, scale));
}

void CaptureWidget::applyLiveEffectSample(CaptureTool* tool,
                                          const QRect& overlayRect,
                                          const EffectSample& sample)
{
    Q_UNUSED(overlayRect)
    if (tool == nullptr) {
        return;
    }
    const EffectSample previous = m_videoEffectSamples.value(tool);
    if (!sample.valid) {
        // Invalidate instead of freezing: the next paint finds no usable sample,
        // abstains, and the user is told why. Keeping the last good patch would
        // show content that is no longer on the desktop.
        if (m_videoEffectSamples.contains(tool)) {
            m_videoEffectSamples.remove(tool);
            const QRect bounds = tool->boundingRect()
                                   .normalized()
                                   .adjusted(-2, -2, 2, 2)
                                   .intersected(m_videoRegion);
            if (!bounds.isEmpty()) {
                update(bounds);
            }
        }
        reportLiveEffectFailure(sample.failure);
        return;
    }
    if (previous.valid && previous.rect == sample.rect &&
        previous.image == sample.image) {
        // Unchanged content: no repaint (a static screen costs nothing).
        return;
    }
    m_videoEffectSamples.insert(tool, sample);
    const QRect bounds = tool->boundingRect()
                           .normalized()
                           .adjusted(-2, -2, 2, 2)
                           .intersected(m_videoRegion);
    if (!bounds.isEmpty()) {
        update(bounds);
    }
}

void CaptureWidget::handleLiveEffectSample(quint64 id, const EffectSample& sample)
{
    const QPointer<CaptureTool> owner = m_videoSampleOwners.value(id);
    CaptureTool* tool = owner.data();
    if (tool == nullptr) {
        m_videoSampleOwners.remove(id);
        m_videoSampleRects.remove(id);
        if (m_videoSampler) {
            m_videoSampler->forget(id);
        }
        return;
    }
    const QRect requested = m_videoSampleRects.value(id);
    if (requested.isEmpty()) {
        return;
    }
    // The sample may have been produced for an older rectangle (the annotation
    // moved while the walk was running). Its pixels are only usable when they
    // still describe this annotation's rectangle, so that is checked instead of
    // assumed: a sample that no longer matches is dropped and the next request
    // brings the right one.
    const qreal scale = devicePixelRatioF();
    const QRect current(mapToGlobal(requested.topLeft()), requested.size());
    if (CaptureGeometry::physicalRect(current, scale) != sample.rect) {
        return;
    }
    EffectSample effect = sample;
    effect.rect = requested;
    effect.covered = requested;
    effect.valid = sample.valid;
    applyLiveEffectSample(tool, requested, effect);
}

void CaptureWidget::reportLiveEffectFailure(const QString& reason)
{
    if (reason.isEmpty() || m_videoEffectFailures.contains(reason)) {
        return;
    }
    // One report per reason per recording (or per capture): a failure that
    // repeats on every paint must not repeat the message, and the reason stays
    // suppressed for the rest of this session.
    m_videoEffectFailures.insert(reason);
    qWarning() << "live effect unavailable:" << reason;
    if (m_videoSession) {
        // While a recording runs, anything this widget shows over the recorded
        // region is encoded into the video: a notifier box is as much a status
        // burn-in as a tooltip is. The reason therefore goes to the same
        // rate-limited log and to the summary the user reads after the
        // recording, and nothing is drawn now.
        recordVideoWarning(tr("Live effect: %1").arg(reason));
        return;
    }
    // Outside a recording the overlay is not being captured, so the notifier is
    // the right place -- deferred, because this runs from a paint and showing a
    // child widget from inside paintEvent is not something to do inline.
    QTimer::singleShot(0, this, [this, reason]() {
        if (m_notifierBox != nullptr) {
            m_notifierBox->showMessage(tr("Live effect: %1").arg(reason));
        }
    });
}

void CaptureWidget::recordVideoWarning(const QString& message)
{
    if (message.trimmed().isEmpty()) {
        return;
    }
    // Distinct messages only, and a handful of them: the report is a summary the
    // user reads after the recording, not a log file.
    constexpr int kMaxRecordedWarnings = 4;
    if (!m_videoWarnings.contains(message) &&
        m_videoWarnings.size() < kMaxRecordedWarnings) {
        m_videoWarnings.append(message);
    }
    // At most one console line per second: a burst of encoder backpressure must
    // not turn into log spam, and the summary above is kept either way.
    if (m_videoWarningLog.isValid() && m_videoWarningLog.elapsed() < 1000) {
        return;
    }
    m_videoWarningLog.start();
    qInfo("recording: %s", qUtf8Printable(message));
}

QString CaptureWidget::videoWarningSummary() const
{
    if (m_videoWarnings.isEmpty()) {
        return {};
    }
    return tr("While recording:\n%1").arg(m_videoWarnings.join(QLatin1Char('\n')));
}

void CaptureWidget::clearRecordingSamples()
{
    if (m_videoSampler) {
        for (auto it = m_videoSampleOwners.cbegin();
             it != m_videoSampleOwners.cend();
             ++it) {
            m_videoSampler->forget(it.key());
        }
    }
    m_videoSampleOwners.clear();
    m_videoSampleRects.clear();
    m_videoSampleRequests.clear();
    m_videoEffectSamples.clear();
    m_videoEffectFailures.clear();
    m_videoLiveCanvas = QPixmap();
}

CaptureWidget::AnnotationLifetime CaptureWidget::lifetimeFor(
  CaptureTool::Type type,
  qint64 ownStampMs) const
{
    AnnotationLifetime lifetime;
    lifetime.startMs = ownStampMs;
    if (!isPolicyTool(type)) {
        // Not a tool the policy knows (the policy covers every annotation tool,
        // so this is a defensive path): inherit the global marking hold/fade.
        const Recording::AnnotationTiming inherited =
          m_videoPolicy.effectiveTiming(Recording::AnnotationTool::Pen);
        lifetime.holdMs = inherited.holdMs;
        lifetime.fadeMs = inherited.fadeMs;
        return lifetime;
    }
    const Recording::AnnotationTool tool = policyToolFor(type);
    const Recording::AnnotationTiming timing =
      m_videoPolicy.effectiveTiming(tool);
    lifetime.holdMs = timing.holdMs;
    lifetime.fadeMs = timing.fadeMs;
    if (tool == Recording::AnnotationTool::Counter) {
        // Group rule, owned by the policy: in GroupFromFirstBubble mode every
        // bubble ages from the first one, so the group fades as one. In the
        // default mode each bubble keeps its own stamp and the group is held
        // while the counter tool is selected (see enforceCounterGroup()).
        lifetime.startMs = Recording::AnnotationPolicy::stampFor(
          tool,
          m_videoPolicy.counterMode(),
          ownStampMs,
          m_videoCounterGroupStamp);
        if (m_videoPolicy.counterMode() ==
              Recording::AnnotationCounterMode::HoldUntilToolChange &&
            activeButtonToolType() == CaptureTool::TYPE_CIRCLECOUNT) {
            lifetime.pinned = true;
        }
    }
    return lifetime;
}

void CaptureWidget::setAnnotationPolicy(const Recording::AnnotationPolicy& policy)
{
    // Applies to annotations stamped from now on. Annotations already on the
    // timeline keep the values they were given, so a policy change can never
    // make one disappear early.
    m_videoPolicy = policy;
}

Recording::AnnotationTool CaptureWidget::policyToolFor(CaptureTool::Type type)
{
    for (const Recording::AnnotationTool tool :
         Recording::AnnotationPolicy::supportedTools()) {
        if (Recording::captureToolType(tool) == type) {
            return tool;
        }
    }
    return Recording::AnnotationTool::Pen;
}

bool CaptureWidget::isPolicyTool(CaptureTool::Type type)
{
    for (const Recording::AnnotationTool tool :
         Recording::AnnotationPolicy::supportedTools()) {
        if (Recording::captureToolType(tool) == type) {
            return true;
        }
    }
    return false;
}

void CaptureWidget::stampRecordingAnnotation(CaptureTool* tool)
{
    if (!m_videoSession || !tool) return;
    const qint64 now = m_videoSession->activeMilliseconds();
    const Recording::AnnotationTool type = policyToolFor(tool->type());
    if (type == Recording::AnnotationTool::Counter &&
        m_videoCounterGroupStamp < 0) {
        // First bubble of a new group: remember the anchor the group rule uses
        // in GroupFromFirstBubble mode.
        m_videoCounterGroupStamp = now;
    }
    m_videoLifetimes.insert(tool, lifetimeFor(tool->type(), now));
}

void CaptureWidget::enforceCounterGroup()
{
    if (!m_videoSession) return;
    const bool counterActive =
      activeButtonToolType() == CaptureTool::TYPE_CIRCLECOUNT;
    // Leaving the counter tool ends the anchor of the current group in *both*
    // counter modes. Bubbles that are already stamped keep their own start
    // time, so an older group keeps ageing from when it was drawn while the
    // next group starts its own clock; without this a group that is started
    // after coming back to the tool inherited the previous group's stamp and
    // could expire the moment it was drawn.
    if (!counterActive) {
        m_videoCounterGroupStamp = -1;
    }
    if (m_videoPolicy.counterMode() !=
        Recording::AnnotationCounterMode::HoldUntilToolChange) {
        // Every bubble of a group ages from the group's first one in this mode;
        // switching tools changes nothing else.
        return;
    }
    const qint64 now = m_videoSession->activeMilliseconds();
    const Recording::AnnotationTiming groupEnd =
      m_videoPolicy.effectiveTiming(Recording::AnnotationTool::Counter);
    bool changed = false;
    for (auto it = m_videoLifetimes.begin(); it != m_videoLifetimes.end();
         ++it) {
        if (!it.key() || it.key()->type() != CaptureTool::TYPE_CIRCLECOUNT) {
            continue;
        }
        if (counterActive) {
            if (!it.value().pinned) {
                // Still counting: never let a bubble expire mid walk-through.
                it.value().pinned = true;
                changed = true;
            }
        } else if (it.value().pinned) {
            // The user left the counter tool: the group is re-stamped here and
            // ages with the policy's group-end timing (zero hold, so it fades
            // over the configured fade time right away).
            it.value().pinned = false;
            it.value().startMs = now;
            it.value().holdMs = groupEnd.holdMs;
            it.value().fadeMs = groupEnd.fadeMs;
            changed = true;
        }
    }
    if (changed) {
        updateAnnotationRefresh();
        update();
    }
}

QList<QRect> CaptureWidget::recordingOverlayRects() const
{
    QList<QRect> overlays;
    // Toolbar buttons, the settings panel, the color picker and the text input
    // are direct children in overlay coordinates.
    const auto children =
      findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* child : children) {
        // The selection widget spans the recorded region. It is not a control,
        // and including it would swallow the pointer events that must reach the
        // applications being recorded.
        if (child == m_selection || child == m_magnifier) {
            continue;
        }
        if (child->isVisibleTo(this)) {
            overlays.append(child->geometry());
        }
    }
    return overlays;
}

void CaptureWidget::updateRecordingInput()
{
    if (!m_videoSession) {
        resetRecordingInput();
        return;
    }
    // Drawing intercepts pointer events inside the recorded region. While
    // interacting, only the overlay controls take input and everything else --
    // including apps on the other monitors -- keeps working normally.
    const bool drawing = annotationTool(activeButtonToolType());
    const QRegion input = CaptureGeometry::overlayInputRegion(
      rect(), m_videoRegion, recordingOverlayRects(), drawing);
    if (input.isEmpty()) {
        // Never leave the overlay without any interactive surface.
        resetRecordingInput();
        return;
    }
    if (Recording::setX11InputRegion(winId(), input, devicePixelRatioF())) {
        m_recordingInputShaped = true;
    }
}

void CaptureWidget::resetRecordingInput()
{
    if (!m_recordingInputShaped) return;
    Recording::resetX11InputRegion(winId());
    m_recordingInputShaped = false;
}


void CaptureWidget::saveImageForTransfer()
{
    commitCurrentTool();
    const QString directory = m_config.savePath();
    if (!QDir().mkpath(directory)) {
        QMessageBox::warning(this, tr("Save failed"), tr("Could not create the image save folder."));
        return;
    }
    const QString path = FileNameHandler().properScreenshotPath(directory, m_config.saveAsFileExtension());
    if (!saveToFilesystem(pixmap(), path)) return;
    m_exportedKeepOpen = true;
    uploadSavedCapture(path);
    close();
}
