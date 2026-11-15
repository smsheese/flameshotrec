// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

// Based on Lightscreen areadialog.h, Copyright 2017  Christian Kaiser
// <info@ckaiser.com.ar> released under the GNU GPL2
// <https://www.gnu.org/licenses/gpl-2.0.txt>

// Based on KDE's KSnapshot regiongrabber.cpp, revision 796531, Copyright 2007
// Luca Gugelmann <lucag@student.ethz.ch> released under the GNU LGPL
// <http://www.gnu.org/licenses/old-licenses/library.txt>

#pragma once

#include "tools/capturecontext.h"
#include "tools/capturetool.h"
#include "utils/confighandler.h"
#include "widgets/capture/buttonhandler.h"
#include "widgets/capture/capturetoolbutton.h"
#include "widgets/capture/capturetoolobjects.h"
#include "widgets/capture/magnifierwidget.h"
#include "widgets/capture/selectionwidget.h"

#include <QImage>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QUndoStack>
#include <QWidget>
#ifdef ENABLE_RECORDING
#include "recording/annotationpolicy.h"
#include "recording/capturerecordingsession.h"
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QStringList>
#endif

class QLabel;
class QPaintEvent;
class QResizeEvent;
class QMouseEvent;
class QShortcut;
class QNetworkAccessManager;
class QNetworkReply;
class ColorPicker;
class NotifierBox;
class HoverEventFilter;
#if !defined(DISABLE_UPDATE_CHECKER)
class UpdateNotificationWidget;
#endif
class UtilityPanel;
class SidePanelWidget;

#ifdef ENABLE_RECORDING
namespace LiveDesktop {
class Sampler;
}
#endif

class CaptureWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CaptureWidget(const CaptureRequest& req,
                           bool fullScreen = true,
                           QWidget* parent = nullptr);
    ~CaptureWidget();

    QPixmap pixmap();
    void setCaptureToolObjects(const CaptureToolObjects& captureToolObjects);
#if !defined(DISABLE_UPDATE_CHECKER)
    void showAppUpdateNotification(const QString& appLatestVersion,
                                   const QString& appLatestUrl);
#endif

public slots:
    bool commitCurrentTool();
    void deleteToolWidgetOrClose();

signals:
    void colorChanged(const QColor& c);
    void toolSizeChanged(int size);

private slots:
    void undo();
    void redo();
    void cancel();
    void togglePanel();
    void childEnter();
    void childLeave();

    void deleteCurrentTool();

    void setState(CaptureToolButton* b);
    void handleToolSignal(CaptureTool::Request r);
    void handleButtonLeftClick(CaptureToolButton* b);
    void handleButtonRightClick(CaptureToolButton* b);
    void setDrawColor(const QColor& c);
    void onToolSizeChanged(int size);
    void onToolSizeSettled(int size);
    void updateActiveLayer(int layer);
    void onMoveCaptureToolUp(int captureToolIndex);
    void onMoveCaptureToolDown(int captureToolIndex);
    void selectAll();
    void xywhTick();
    void onDisplayGridChanged(bool display);
    void onGridSizeChanged(int size);

    void startColorGrab();

public:
    void removeToolObject(int index = -1);
    void showxywh();

#ifdef ENABLE_RECORDING
    // The annotation lifetime policy the next recording applies. Production
    // loads it from the configuration once per recording
    // (Recording::loadAnnotationPolicyFromConfig()); the settled snapshot is
    // kept here so the timeline never re-reads configuration per frame.
    void setAnnotationPolicy(const Recording::AnnotationPolicy& policy);
    Recording::AnnotationPolicy annotationPolicy() const
    {
        return m_videoPolicy;
    }
    // Tells the user, once per distinct reason, that a live effect cannot show
    // what is really on the desktop. Never silent: the patch is cleared instead.
    // Production calls this from paintLiveEffect() and from the live-effect
    // toolbar action; it is public because the integration harness has to
    // exercise the failure path on a desktop whose compositor works, which is
    // the only way to reach it there (no mock compositor, same widget).
    void reportLiveEffectFailure(const QString& reason);
#endif

protected:
    void paintEvent(QPaintEvent* paintEvent) override;
    void showEvent(QShowEvent* showEvent) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* mouseEvent) override;
    void mouseMoveEvent(QMouseEvent* mouseEvent) override;
    void mouseReleaseEvent(QMouseEvent* mouseEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* keyEvent) override;
    void keyReleaseEvent(QKeyEvent* keyEvent) override;
    void wheelEvent(QWheelEvent* wheelEvent) override;
    void resizeEvent(QResizeEvent* resizeEvent) override;
    void moveEvent(QMoveEvent* moveEvent) override;
    void changeEvent(QEvent* changeEvent) override;
    void closeEvent(QCloseEvent* event) override;

private:
#ifdef ENABLE_RECORDING
    // One annotation's slot on the recording timeline.
    struct AnnotationLifetime
    {
        qint64 startMs{0};
        int holdMs{0};
        int fadeMs{0};
        // Held instead of counting down: a circle-count group stays complete
        // while the user is still counting (see enforceCounterGroup()).
        bool pinned{false};
    };
    void startRecording();
    void finishRecording(Recording::CaptureRecordingSession::Action action);
    void saveImageForTransfer();
    void updateRecordingUi();
    void paintRecording(QPainter& painter);
    void refreshRecordingAnnotations();
    void updateRecordingInput();
    void resetRecordingInput();
    void updateAnnotationRefresh();
    QList<QRect> recordingOverlayRects() const;
    bool handleRecordingAction(CaptureTool::Type type);
    AnnotationLifetime lifetimeFor(CaptureTool::Type type, qint64 now) const;
    // The policy module's tool behind an overlay tool (the inverse of
    // Recording::captureToolType()). One mapping for the whole overlay, so the
    // timeline and the per-tool size cannot disagree.
    static Recording::AnnotationTool policyToolFor(CaptureTool::Type type);
    static bool isPolicyTool(CaptureTool::Type type);
    void stampRecordingAnnotation(CaptureTool* tool);
    // Keeps a circle-count group whole while its tool is active and starts the
    // group's hold when the user switches tools (the documented default).
    void enforceCounterGroup();
    static bool liveEffectTool(CaptureTool::Type type);
    // One live desktop sample of an effect annotation, in overlay coordinates.
    // `valid` is false (with a reason) when the desktop below the effect cannot
    // be rebuilt faithfully: an effect must abstain rather than paint a frozen
    // frame, the overlay's own pixels or the capture-time screenshot.
    struct EffectSample
    {
        QImage image;
        QRect rect;    // overlay rectangle that was asked for
        QRect covered; // overlay rectangle really rebuilt
        bool valid{false};
        QString failure;
        // When the pixels were taken. A sample older than one poll interval is
        // refreshed before it is painted again, so an effect can never freeze on
        // an old frame even if a background request is slow or dropped.
        qint64 stampMs{-1};
    };
    // Uploads `sample` into the shared canvas and lets the tool paint itself from
    // it. Returns false when the sample cannot serve `sampleRect`.
    bool paintEffectFromSample(QPainter& painter,
                               CaptureTool* tool,
                               const QRect& sampleRect,
                               const EffectSample& sample);
    // Paints one invert/pixelate annotation from live desktop content.
    void paintLiveEffect(QPainter& painter, CaptureTool* tool);
    // Asks the background sampler for a fresh sample of `overlayRect` for
    // `tool`; coalesced per annotation, so a slow walk cannot queue up.
    void requestLiveEffectSample(CaptureTool* tool, const QRect& overlayRect);
    // Stores a sample the sampler produced and repaints when the pixels changed.
    void applyLiveEffectSample(CaptureTool* tool,
                               const QRect& overlayRect,
                               const EffectSample& sample);
    void handleLiveEffectSample(quint64 id, const EffectSample& sample);
    static quint64 effectSampleId(CaptureTool* tool);
    // Recording status (encoder backlog, audio trouble) is a diagnostic, not
    // something to draw: the overlay covers the recorded region, so anything it
    // shows there is encoded into the video. Warnings are logged with a rate
    // limit here and reported once, after the recording, in the notification the
    // user reads outside the recorded area.
    void recordVideoWarning(const QString& message);
    QString videoWarningSummary() const;
    void ensureLiveCanvas();
    void clearRecordingSamples();
    QPointer<Recording::CaptureRecordingSession> m_videoSession;
    QTimer m_videoRefresh;
    // Timeline slot per annotation. Keyed by the object the overlay paints, so
    // it is carried across the one-to-one replacement a commit performs.
    QHash<CaptureTool*, AnnotationLifetime> m_videoLifetimes;
    // Last usable live sample of each effect annotation, compared on the poll
    // tick to decide whether the overlay has to repaint at all.
    QHash<CaptureTool*, EffectSample> m_videoEffectSamples;
    // Overlay-sized carrier for live pixels: CaptureTool::process() addresses
    // its source in overlay coordinates, so the sample has to be uploaded into
    // a pixmap of that geometry before the tool can paint from it.
    QPixmap m_videoLiveCanvas;
    // Desktop sampler running off the GUI thread with its own X11 connection;
    // null when this server or platform cannot sample the stack at all.
    QPointer<LiveDesktop::Sampler> m_videoSampler;
    // Which annotation a sampler request id belongs to. QPointer, because an
    // annotation can expire while its sample is still in flight.
    QHash<quint64, QPointer<CaptureTool>> m_videoSampleOwners;
    // The overlay rectangle each in-flight request asked for, so a result can be
    // placed without inverting the physical-coordinate mapping.
    QHash<quint64, QRect> m_videoSampleRects;
    // One entry per reason, so a failure that repeats on every poll tick is told
    // to the user once instead of flickering a message.
    QSet<QString> m_videoEffectFailures;
    // When a sample was last requested for an annotation, so the fade repaint
    // (16 ms) cannot hammer the sampler: at most one request per poll interval.
    QHash<CaptureTool*, qint64> m_videoSampleRequests;
    Recording::AnnotationPolicy m_videoPolicy =
      Recording::AnnotationPolicy::defaults();
    // Release stamp of the first bubble of the current counter group, or a
    // negative value while no group is open (see AnnotationPolicy::stampFor).
    qint64 m_videoCounterGroupStamp{-1};
    bool m_videoLiveSampling{false};
    // Status messages gathered while recording, reported after it ends (see
    // recordVideoWarning()).
    QStringList m_videoWarnings;
    QElapsedTimer m_videoWarningLog;
    QRect m_videoRegion;
    bool m_videoFinalizing{false};
    bool m_recordingInputShaped{false};
#endif
    // Logical rectangle covered by the capture overlay: the whole desktop when
    // the selection is not limited to one monitor.
    QRect m_desktopCanvas;
    // True only when `m_desktopCanvas` really is the union of several monitors
    // (X11 whole-desktop capture). Per-monitor captures keep the plain
    // fullscreen window they always had.
    bool m_spanningCanvas{false};
    bool m_spanningApplied{false};
    void pushObjectsStateToUndoStack();
    void releaseActiveTool();
    void uncheckActiveTool();
    int selectToolItemAtPos(const QPoint& pos);
    void showColorPicker(const QPoint& pos);
    bool startDrawObjectTool(const QPoint& pos);
    QPointer<CaptureTool> activeToolObject();
    void initContext(bool fullscreen, const CaptureRequest& req);
    void initPanel();
    void initSelection();
    void initShortcuts();
    void initButtons();
    void initHelpMessage();
    void initQuitPrompt();
    void updateSizeIndicator();
    void updateCursor();
    void updateSelectionState();
    void updateTool(CaptureTool* tool);
    void updateLayersPanel();
    bool promptQuit();
    void pushToolToStack();
    void makeChild(QWidget* w);
    void restoreCircleCountState();

    QList<QShortcut*> newShortcut(const QKeySequence& key,
                                  QWidget* parent,
                                  const char* slot);

    void setToolSize(int size);

    QRect extendedSelection() const;
    QRect extendedRect(const QRect& r) const;
    QRect paddedUpdateRect(const QRect& r) const;
    void drawErrorMessage(const QString& msg, QPainter* painter);
    void drawInactiveRegion(QPainter* painter);
    void drawToolsData(bool drawSelection = true);
    void drawObjectSelection();

    void processPixmapWithTool(QPixmap* pixmap, CaptureTool* tool);

    CaptureTool* activeButtonTool() const;
    CaptureTool::Type activeButtonToolType() const;

    QPoint snapToGrid(const QPoint& point) const;

    ////////////////////////////////////////
    // Class members

    // Context information
    CaptureContext m_context;

    // Main ui color
    QColor m_uiColor;
    // Secondary ui color
    QColor m_contrastUiColor;

    // Outside selection opacity
    int m_opacity;
    int m_toolSizeByKeyboard;

    // utility flags
    bool m_mouseIsClicked;
    bool m_newSelection;
    bool m_movingSelection;
    bool m_captureDone;
    bool m_recordingSession{ false };
    bool m_exportedKeepOpen;
    bool m_previewEnabled;
    bool m_adjustmentButtonPressed;
    bool m_configError;
    bool m_configErrorResolved;

#if !defined(DISABLE_UPDATE_CHECKER)
    UpdateNotificationWidget* m_updateNotificationWidget;
#endif
    quint64 m_lastMouseWheel;
    QPointer<CaptureToolButton> m_sizeIndButton;
    // Last pressed button
    QPointer<CaptureToolButton> m_activeButton;
    QPointer<CaptureTool> m_activeTool;
    bool m_activeToolIsMoved;
    QPointer<QWidget> m_toolWidget;
    QPointer<QMessageBox> m_quitPrompt;

    ButtonHandler* m_buttonHandler;
    UtilityPanel* m_panel;
    SidePanelWidget* m_sidePanel;
    ColorPicker* m_colorPicker;
    ConfigHandler m_config;
    NotifierBox* m_notifierBox;
    HoverEventFilter* m_eventFilter;
    SelectionWidget* m_selection;
    MagnifierWidget* m_magnifier;
    QString m_helpMessage;

    SelectionWidget::SideType m_mouseOverHandle;

    QMap<CaptureTool::Type, CaptureTool*> m_tools;
    CaptureToolObjects m_captureToolObjects;
    CaptureToolObjects m_captureToolObjectsBackup;

    QPoint m_mousePressedPos;
    QPoint m_activeToolOffsetToMouseOnStart;

    // XYWH display position and timer
    bool m_xywhDisplay;
    QTimer m_xywhTimer;

    QUndoStack m_undoStack;

    bool m_existingObjectIsChanged;

    // For start moving after more than X offset
    QPoint m_startMovePos;
    bool m_startMove;

    // Grid
    bool m_displayGrid{ false };
    int m_gridSize{ 10 };

    bool m_clipboardWorkaroundDone{ false };
};
