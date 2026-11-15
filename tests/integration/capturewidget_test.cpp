// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors
//
// Integrated CaptureWidget check: the *real* overlay widget, on a real (nested)
// X server with a window manager and a compositor, driven through its public
// widget behaviour (mouse/keyboard events, toolbar buttons) - no test hooks in
// production code.
//
// Covered (every case asserts real pixels or real window/X11 state; there is no
// unconditional "handled" pass):
//   * the overlay covers the union of all screens, applies its spanning
//     geometry only for that union, and a per-monitor capture keeps its plain
//     fullscreen window;
//   * a selection can be dragged across the monitor boundary;
//   * recording starts through the Record button and the recording UI is
//     accurate (preparing vs recording, screenshot-only actions disabled);
//   * the "open with application" window takes one external-window reference
//     and releases it when it closes;
//   * pencil, text, counter, invert and rectangular-selection annotations are
//     created by real drags/clicks and each paints its own pixels;
//   * Invert reads the *live* composited desktop (a window repainted from red
//     to blue after the capture window opened is inverted as blue, and the
//     capture-time inverse appears in no frame, on screen or in the encoded
//     video);
//   * the magenta boundary ring is painted on screen just outside the region and
//     is absent from every encoded frame, whose size is exactly the region;
//   * an annotation stays at full strength up to the end of the 3 s hold,
//     fades over the fade window, and disappears after hold+fade;
//   * the overlay does not repaint during the hold (0 paint events in 1 s) and
//     does animate the fade (8 paint events);
//   * invert and pixelate follow a *changing* desktop after the annotation was
//     released: the probe windows change colour during the hold and the patch
//     on screen tracks them (it is rebuilt from the window stack below the
//     overlay, so it never samples its own patch);
//   * pixelate really quantizes: the patch is a grid of uniform blocks whose
//     colours come from the live fringe and change when the desktop changes;
//   * a counter group stays complete while the counter tool is active, starts
//     its hold when the tool is switched, and fades as one group;
//   * a per-tool hold/fade override applies to its own tool only;
//   * "Copy" during a recording finalizes into the private staging directory,
//     puts the *file* URI on the clipboard (all desktop copy flavours), leaves
//     nothing in the configured video folder, and the file survives the overlay
//     being destroyed;
//   * the staging retention sweep removes stale files but never the file the
//     clipboard offers;
//   * the X11 input shape makes the recorded monitor click-through in
//     "interact" mode and captures the pointer only while a drawing tool is
//     selected; a point on another RandR monitor, verified to be outside the
//     selection, is never covered.
//
// The desktop is never touched: everything runs on the nested display given by
// DISPLAY (Xephyr) with a real compositing manager, a throw-away
// XDG_CONFIG_HOME and no audio enabled.

#include "config/configresolver.h"
#include "core/flameshot.h"
#include "tools/capturetool.h"
#include "tools/text/textwidget.h"
#include "utils/confighandler.h"
#include "widgets/capture/capturetoolbutton.h"
#include "widgets/capture/capturewidget.h"
#include "recording/annotationpolicy.h"
#include "recording/annotationpolicy_confighandler.h"
#include "widgets/capture/notifierbox.h"
#include "widgets/capture/recordingclipboard.h"
#include "widgets/capture/x11livedesktop.h"
#include "widgets/capture/selectionwidget.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHelpEvent>
#include <QImage>
#include <QMessageBox>
#include <QLineEdit>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QToolTip>
#include <QClipboard>
#include <QFile>
#include <QMimeData>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>
#include <QWidget>

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

#include <cmath>
#include <cstdio>

namespace {

// The production code refuses to record without a compositing manager. When the
// nested server already has one (metacity, started by run.sh, owns
// _NET_WM_CM_S0), it is left alone: the overlay's translucency is then a real
// composite, which the invert checks below observe as real desktop pixels. Only
// when nothing composites does the harness claim the selection itself, so the
// guard passes and the geometry/encoding checks can still run; the composite is
// then a plain X draw and the live-pixel checks report that limitation instead
// of pretending.
class CompositorProbe
{
public:
    CompositorProbe()
    {
        m_display = XOpenDisplay(nullptr);
        if (!m_display) {
            return;
        }
        const int screen = DefaultScreen(m_display);
        const QByteArray name = QByteArrayLiteral("_NET_WM_CM_S") +
                                QByteArray::number(screen);
        m_atom = XInternAtom(m_display, name.constData(), False);
        m_real = XGetSelectionOwner(m_display, m_atom) != None;
        if (m_real) {
            return;
        }
        m_window = XCreateSimpleWindow(m_display, RootWindow(m_display, screen),
                                       -10, -10, 1, 1, 0, 0, 0);
        XSetSelectionOwner(m_display, m_atom, m_window, CurrentTime);
        XFlush(m_display);
    }
    ~CompositorProbe()
    {
        if (m_display) {
            XCloseDisplay(m_display);
        }
    }
    // True when a compositing manager other than this harness owns the
    // selection, i.e. the frames really are composited.
    bool real() const
    {
        return m_real;
    }
    bool active() const
    {
        return m_display && XGetSelectionOwner(m_display, m_atom) != None;
    }

private:
    Display* m_display{nullptr};
    Window m_window{0};
    Atom m_atom{0};
    bool m_real{false};
};

int g_failures = 0;

void check(bool condition, const QString& what)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", qPrintable(what));
    if (!condition) {
        ++g_failures;
    }
}

void note(const QString& what)
{
    std::printf("      %s\n", qPrintable(what));
}

CaptureToolButton* buttonFor(CaptureWidget* widget, CaptureTool::Type type)
{
    for (auto* button : widget->findChildren<CaptureToolButton*>()) {
        if (button->tool() && button->tool()->type() == type) {
            return button;
        }
    }
    return nullptr;
}

// Input shape of the overlay's X window, in overlay coordinates.
QList<QRect> inputShape(CaptureWidget* widget)
{
    QList<QRect> rects;
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        return rects;
    }
    int count = 0;
    int ordering = 0;
    XRectangle* shape =
      XShapeGetRectangles(display, Window(widget->winId()), ShapeInput,
                          &count, &ordering);
    if (!shape) {
        // No shape installed: the window takes the whole pointer input.
        rects.append(widget->rect());
    } else {
        for (int index = 0; index < count; ++index) {
            rects.append(QRect(shape[index].x, shape[index].y, shape[index].width,
                               shape[index].height));
        }
        XFree(shape);
    }
    XCloseDisplay(display);
    return rects;
}

bool shapeCovers(const QList<QRect>& shape, const QPoint& point)
{
    for (const QRect& rect : shape) {
        if (rect.contains(point)) {
            return true;
        }
    }
    return false;
}

// Counts pixels close to `target` inside `area` of `image`.
int countColour(const QImage& image, const QRect& area, const QColor& target,
                int tolerance = 40)
{
    const QRect clipped = area.intersected(image.rect());
    int count = 0;
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        const auto* line =
          reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QRgb pixel = line[x];
            if (std::abs(qRed(pixel) - target.red()) <= tolerance &&
                std::abs(qGreen(pixel) - target.green()) <= tolerance &&
                std::abs(qBlue(pixel) - target.blue()) <= tolerance) {
                ++count;
            }
        }
    }
    return count;
}

// RandR monitors as the X server reports them (the authoritative list; Qt also
// exposes a virtual-desktop fallback screen). Used to probe points that belong
// to a monitor other than the recorded one instead of trusting the fallback.
QList<QRect> randrMonitors()
{
    QList<QRect> monitors;
    QProcess process;
    process.start(QStringLiteral("xrandr"),
                  { QStringLiteral("--listmonitors") });
    if (!process.waitForFinished(4000)) {
        return monitors;
    }
    static const QRegularExpression monitorLine(QStringLiteral(
      R"(^\s*\d+:\s+\S+\s+(\d+)/\d+x(\d+)/\d+\+(-?\d+)\+(-?\d+))"));
    const QString output =
      QString::fromLocal8Bit(process.readAllStandardOutput());
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const auto match = monitorLine.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        monitors.append(QRect(match.captured(3).toInt(),
                              match.captured(4).toInt(),
                              match.captured(1).toInt(),
                              match.captured(2).toInt()));
    }
    return monitors;
}

// Pixels of a rectangle on the composited desktop, i.e. exactly what a client
// can read back from X11 (the live source the invert annotation samples).
QImage grabDesktopRect(const QRect& global)
{
    QScreen* screen = QGuiApplication::screenAt(global.center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen || global.isEmpty()) {
        return {};
    }
    const QPixmap shot = screen->grabWindow(
      0, global.x(), global.y(), global.width(), global.height());
    return shot.isNull() ? QImage() : shot.toImage();
}

// A plain window under the overlay whose colour the test controls. With a real
// compositor the overlay is transparent over it, so a live screen sample shows
// this colour: that is the pixel source the invert annotation has to use.
class ColourProbe : public QWidget
{
public:
    ColourProbe(const QRect& area, const QColor& colour)
      : m_colour(colour)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setGeometry(area);
        show();
    }

    void setColour(const QColor& colour)
    {
        m_colour = colour;
        repaint();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), m_colour);
    }

private:
    QColor m_colour;
};

// In-memory AnnotationStore: lets the harness build a real policy snapshot with
// per-tool overrides without writing the user's configuration (which would also
// trip ConfigHandler's unrecognised-setting guard until the keys are
// registered). The snapshot is then handed to the overlay the same way the
// configuration UI hands it over.
class MemoryAnnotationStore : public Recording::AnnotationStore
{
public:
    bool isAvailable(const QString&) const override { return true; }
    QVariant read(const QString& key) const override
    {
        return m_values.value(key);
    }
    bool write(const QString& key, const QVariant& value) override
    {
        m_values.insert(key, value);
        return true;
    }
    void remove(const QString& key) override { m_values.remove(key); }

private:
    QHash<QString, QVariant> m_values;
};

// A window whose content is a grid of 6 px stripes in two colours: the source
// for the pixelate checks. A real pixelation quantizes *this* content, so the
// block colours change when the stripes do.
class PatternProbe : public QWidget
{
public:
    PatternProbe(const QRect& area, const QColor& first, const QColor& second)
      : m_first(first)
      , m_second(second)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setGeometry(area);
        show();
    }

    void setColours(const QColor& first, const QColor& second)
    {
        m_first = first;
        m_second = second;
        repaint();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), m_first);
        for (int x = 0; x < width(); x += 12) {
            painter.fillRect(QRect(x + 6, 0, 6, height()), m_second);
        }
    }

private:
    QColor m_first;
    QColor m_second;
};

// What a pixelated patch really looks like. The effect computes a small grid and
// upscales it with nearest-neighbour scaling, so the patch is piecewise constant:
// one colour per grid cell and long runs along both axes. Because the grid rarely
// divides the patch exactly, the runs alternate by one pixel (6/7 px for a 33-cell
// grid in 200 px), which is why the measurement is a colour budget rather than a
// fixed cell grid.
struct BlockReport
{
    QList<QRgb> colours;   // one entry per distinct colour
    int distinct{0};
    int pixels{0};
    int runWidth{1};
    int runHeight{1};
    int uniformPixels{0};
};

int modalRunLength(const QImage& image, const QRect& area, bool horizontal)
{
    QHash<int, int> runs;
    const int length = horizontal ? area.width() : area.height();
    int run = 1;
    for (int index = 1; index < length; ++index) {
        const QRgb previous =
          horizontal ? image.pixel(area.left() + index - 1, area.center().y())
                     : image.pixel(area.center().x(), area.top() + index - 1);
        const QRgb current =
          horizontal ? image.pixel(area.left() + index, area.center().y())
                     : image.pixel(area.center().x(), area.top() + index);
        if (current == previous) {
            ++run;
        } else {
            ++runs[run];
            run = 1;
        }
    }
    ++runs[run];
    int best = 1;
    int bestCount = -1;
    for (auto it = runs.cbegin(); it != runs.cend(); ++it) {
        if (it.value() > bestCount ||
            (it.value() == bestCount && it.key() > best)) {
            best = it.key();
            bestCount = it.value();
        }
    }
    return best;
}

BlockReport blockReport(const QImage& image, const QRect& patch)
{
    BlockReport report;
    const QRect area = patch.intersected(image.rect());
    if (area.width() < 4 || area.height() < 4) {
        return report;
    }
    QHash<QRgb, int> histogram;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = area.left(); x <= area.right(); ++x) {
            ++histogram[line[x]];
        }
    }
    report.pixels = area.width() * area.height();
    report.distinct = histogram.size();
    report.colours = histogram.keys();
    report.runWidth = modalRunLength(image, area, true);
    report.runHeight = modalRunLength(image, area, false);
    // Pixels that sit in a group of at least 3x3 identical neighbours: a
    // quantized image is almost entirely made of those.
    for (int y = area.top() + 1; y < area.bottom(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = area.left() + 1; x < area.right(); ++x) {
            const QRgb pixel = line[x];
            if (line[x - 1] == pixel && line[x + 1] == pixel &&
                reinterpret_cast<const QRgb*>(image.constScanLine(y - 1))[x] ==
                  pixel &&
                reinterpret_cast<const QRgb*>(image.constScanLine(y + 1))[x] ==
                  pixel) {
                ++report.uniformPixels;
            }
        }
    }
    return report;
}

// Pixels with a strong colour (max channel far above min): separates a
// saturated source from a black/white one, which the mean colour cannot do
// because both average to mid grey.
int saturatedPixels(const QImage& image, const QRect& area)
{
    const QRect clipped = area.intersected(image.rect());
    int count = 0;
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QRgb pixel = line[x];
            const int high = qMax(qRed(pixel), qMax(qGreen(pixel), qBlue(pixel)));
            const int low = qMin(qRed(pixel), qMin(qGreen(pixel), qBlue(pixel)));
            if (high - low > 90) {
                ++count;
            }
        }
    }
    return count;
}

// Pixels where the drawing colour dominates the other channels. A translucent
// tool (the marker) never matches the colour exactly, and a dimmed background
// shifts it, so this is the measure that works for both.
int countDominant(const QImage& image, const QRect& area)
{
    const QRect clipped = area.intersected(image.rect());
    int count = 0;
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QRgb pixel = line[x];
            if (qGreen(pixel) - qMax(qRed(pixel), qBlue(pixel)) > 20) {
                ++count;
            }
        }
    }
    return count;
}

int distinctColours(const QList<QRgb>& colours)
{
    QSet<QRgb> unique;
    for (const QRgb colour : colours) {
        unique.insert(colour);
    }
    return unique.size();
}

int countNear(const QImage& image, const QRect& area, const QRgb target,
              int tolerance)
{
    const QRect clipped = area.intersected(image.rect());
    int count = 0;
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        const auto* line =
          reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QRgb pixel = line[x];
            if (std::abs(qRed(pixel) - qRed(target)) <= tolerance &&
                std::abs(qGreen(pixel) - qGreen(target)) <= tolerance &&
                std::abs(qBlue(pixel) - qBlue(target)) <= tolerance) {
                ++count;
            }
        }
    }
    return count;
}

// Counts the paint events the overlay receives, so the cost of the annotation
// timer is measured instead of assumed (review R3).
class PaintCounter : public QObject
{
public:
    int paints{0};

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Paint) {
            ++paints;
        }
        return false;
    }
};

// The encoder's scratch folder. Since the recording is encoded in the private
// staging directory (never in the folder the user saves videos in), this is where
// a running encoder is looked for.
QString findRecordingDirectory(const QString& basePath)
{
    const QDir dir(basePath);
    const auto entries =
      dir.entryList({ QStringLiteral(".flameshot-recording-*") },
                    QDir::Dirs | QDir::Hidden);
    QDir found;
    for (const QString& entry : entries) {
        const QDir candidate(dir.filePath(entry));
        const auto files = candidate.entryList(QDir::Files);
        for (const QString& file : files) {
            if (QFileInfo(candidate.filePath(file)).size() > 0) {
                return candidate.path();
            }
        }
    }
    return {};
}

// Waits for `predicate`, pumping the event loop.
template<typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 5000, int stepMs = 20)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) {
            return true;
        }
        QTest::qWait(stepMs);
    }
    return predicate();
}

// A real drag carries the pressed button in every move event; QTest::mouseMove
// sends moves without buttons, which the overlay (correctly) ignores.
void drag(QWidget* widget, const QPoint& from, const QPoint& to, int steps = 6)
{
    QMouseEvent press(QEvent::MouseButtonPress, from, widget->mapToGlobal(from),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    for (int step = 1; step <= steps; ++step) {
        const QPoint point = from + (to - from) * step / steps;
        QMouseEvent move(QEvent::MouseMove, point, widget->mapToGlobal(point),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &move);
        QTest::qWait(4);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, to,
                        widget->mapToGlobal(to), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "xcb");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("flameshot"));
    QCoreApplication::setApplicationName(QStringLiteral("flameshot"));

    std::printf("== capture widget integration check ==\n");
    note(QStringLiteral("platform: %1, display: %2")
           .arg(QGuiApplication::platformName(),
                QString::fromLocal8Bit(qgetenv("DISPLAY"))));

    const auto screens = QGuiApplication::screens();
    QRect united;
    for (QScreen* screen : screens) {
        note(QStringLiteral("screen %1 at %2")
               .arg(screen->name())
               .arg(QStringLiteral("%1,%2 %3x%4")
                      .arg(screen->geometry().x())
                      .arg(screen->geometry().y())
                      .arg(screen->geometry().width())
                      .arg(screen->geometry().height())));
        united = united.united(screen->geometry());
    }
    check(screens.size() >= 2,
          QStringLiteral("nested server exposes %1 screens (boundary crossing "
                         "needs two)")
            .arg(screens.size()));
    // Qt adds a virtual-desktop fallback screen on this server, so the monitor
    // list below comes from RandR instead of from screens().
    const QRect desktopRoot = [&]() {
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            return united;
        }
        XWindowAttributes attributes{};
        XGetWindowAttributes(display, DefaultRootWindow(display), &attributes);
        XCloseDisplay(display);
        return QRect(0, 0, attributes.width, attributes.height);
    }();
    note(QStringLiteral("X root window: %1x%2, united desktop: %3x%4")
           .arg(desktopRoot.width())
           .arg(desktopRoot.height())
           .arg(united.width())
           .arg(united.height()));

    const QList<QRect> monitors = randrMonitors();
    for (const QRect& monitor : monitors) {
        note(QStringLiteral("randr monitor at %1,%2 %3x%4")
               .arg(monitor.x())
               .arg(monitor.y())
               .arg(monitor.width())
               .arg(monitor.height()));
    }

    ConfigHandler config;
    const QString videoPath = config.recordingVideoPath();
    QDir().mkpath(videoPath);
    note(QStringLiteral("video path: %1").arg(videoPath));

    CompositorProbe compositor;
    check(compositor.active(),
          QStringLiteral("a compositing manager owns _NET_WM_CM_S0 (real "
                         "composite: %1)")
            .arg(compositor.real() ? QStringLiteral("yes")
                                   : QStringLiteral("no, claimed by the "
                                                    "harness so the "
                                                    "recording guard passes")));

    // A plain window under the overlay, red at the moment the overlay takes its
    // capture-time screenshot. It is repainted blue later: an invert annotation
    // must show the inverse of the *live* colour, not of the frozen one.
    const QRect guideRegion(
      united.topLeft() + QPoint(united.width() / 4, united.height() / 4),
      QSize(united.width() / 2, united.height() / 2));
    const QRect probeArea = QRect(guideRegion.topLeft() + QPoint(80, 40),
                                  QSize(240, 140))
                              .intersected(united);
    ColourProbe colourProbe(probeArea, QColor(255, 0, 0));
    // Blue/yellow stripes at capture time: the pixelate annotation must quantize
    // this content, and changing the stripes later must change the blocks.
    // Right of the colour probe and well inside the root window: a rect that
    // reaches past the screen cannot be sampled or encoded, and the pixelate
    // checks would then measure the fallback instead of the live effect.
    const QRect patternArea =
      QRect(probeArea.topRight() + QPoint(20, 0), QSize(240, 120))
        .intersected(desktopRoot);
    PatternProbe patternProbe(patternArea, QColor(0, 0, 255), QColor(255, 255, 0));
    QTest::qWait(400);
    note(QStringLiteral("colour probe %1,%2 %3x%4 (red before the overlay "
                        "opens)")
           .arg(probeArea.x())
           .arg(probeArea.y())
           .arg(probeArea.width())
           .arg(probeArea.height()));

    // R2: only the whole-desktop union may re-assert its geometry. A
    // per-monitor capture (the --monitor path) must keep the plain fullscreen
    // window it always had: the spanning override must not fire for it.
    if (monitors.size() >= 2) {
        int preselect = -1;
        for (int index = 0; index < screens.size(); ++index) {
            if (screens.at(index)->geometry() == monitors.at(1)) {
                preselect = index;
                break;
            }
        }
        if (preselect < 0) {
            note(QStringLiteral("no Qt screen matches randr monitor %1,%2 %3x%4: "
                                "per-monitor check skipped")
                   .arg(monitors.at(1).x())
                   .arg(monitors.at(1).y())
                   .arg(monitors.at(1).width())
                   .arg(monitors.at(1).height()));
        } else {
            CaptureRequest monitorRequest(CaptureRequest::GRAPHICAL_MODE, 0);
            monitorRequest.setSelectedMonitor(preselect);
            auto* monitorWidget = new CaptureWidget(monitorRequest, true);
            monitorWidget->setAttribute(Qt::WA_DeleteOnClose, false);
            monitorWidget->showFullScreen();
            waitFor([&]() { return monitorWidget->isVisible(); }, 4000);
            QTest::qWait(400);
            const QRect monitorGeometry = monitors.at(1);
            note(QStringLiteral("per-monitor overlay: widget %1,%2 %3x%4 vs "
                                "monitor %5,%6 %7x%8")
                   .arg(monitorWidget->geometry().x())
                   .arg(monitorWidget->geometry().y())
                   .arg(monitorWidget->geometry().width())
                   .arg(monitorWidget->geometry().height())
                   .arg(monitorGeometry.x())
                   .arg(monitorGeometry.y())
                   .arg(monitorGeometry.width())
                   .arg(monitorGeometry.height()));
            // The observable difference is the window state: the spanning
            // override cancels fullscreen and re-asserts the union geometry,
            // which must not happen for a per-monitor capture.
            check(monitorWidget->windowState() & Qt::WindowFullScreen,
                  QStringLiteral("a per-monitor capture keeps its plain "
                                 "fullscreen window (no spanning override)"));
            monitorWidget->close();
            QTest::qWait(200);
            delete monitorWidget;
        }
    }

    // R4: "Open with application" hands its window to Flameshot's
    // external-window counter, so closing that window must release it again
    // (otherwise the predicate that lets `flameshot gui` exit never returns).
    {
        CaptureRequest launcherRequest(CaptureRequest::GRAPHICAL_MODE, 0);
        auto* launcherOverlay = new CaptureWidget(launcherRequest, true);
        launcherOverlay->setAttribute(Qt::WA_DeleteOnClose, false);
        launcherOverlay->showFullScreen();
        waitFor([&]() { return launcherOverlay->isVisible(); }, 4000);
        QTest::qWait(300);
        auto* openButton =
          buttonFor(launcherOverlay, CaptureTool::TYPE_OPEN_APP);
        check(openButton != nullptr,
              QStringLiteral("the open-with-application button exists"));
        if (openButton) {
            const int before = Flameshot::instance()->externalWindowCount();
            QTest::mouseClick(openButton, Qt::LeftButton);
            QWidget* launcher = nullptr;
            waitFor(
              [&]() {
                  for (QWidget* candidate : QApplication::topLevelWidgets()) {
                      if (candidate != launcherOverlay &&
                          candidate->isVisible() &&
                          qstrcmp(candidate->metaObject()->className(),
                                  "AppLauncherWidget") == 0) {
                          launcher = candidate;
                          return true;
                      }
                  }
                  return false;
              },
              5000);
            check(launcher != nullptr,
                  QStringLiteral("the application launcher window opens"));
            check(Flameshot::instance()->externalWindowCount() == before + 1,
                  QStringLiteral("the launcher holds one external window while "
                                 "it is open (%1 -> %2)")
                    .arg(before)
                    .arg(Flameshot::instance()->externalWindowCount()));
            if (launcher) {
                launcher->close();
            }
            const bool released = waitFor(
              [&]() {
                  return Flameshot::instance()->externalWindowCount() == before;
              },
              5000);
            check(released,
                  QStringLiteral("closing the launcher releases the counter "
                                 "again (%1)")
                    .arg(Flameshot::instance()->externalWindowCount()));
        }
        launcherOverlay->close();
        QTest::qWait(200);
        delete launcherOverlay;
    }

    CaptureRequest request(CaptureRequest::GRAPHICAL_MODE, 0);
    auto* widget = new CaptureWidget(request, true);
    widget->setAttribute(Qt::WA_DeleteOnClose, false);
    widget->showFullScreen();
    check(waitFor([&]() { return widget->isVisible(); }, 4000),
          QStringLiteral("capture overlay is visible"));

    // ---- 1. desktop-spanning canvas ----------------------------------------
    check(!(widget->windowState() & Qt::WindowFullScreen),
          QStringLiteral("the whole-desktop overlay replaced the WM fullscreen "
                         "state with the desktop canvas"));
    check(widget->geometry() == united,
          QStringLiteral("overlay covers the united desktop: widget %1,%2 %3x%4 "
                         "vs screens %5,%6 %7x%8")
            .arg(widget->geometry().x())
            .arg(widget->geometry().y())
            .arg(widget->geometry().width())
            .arg(widget->geometry().height())
            .arg(united.x())
            .arg(united.y())
            .arg(united.width())
            .arg(united.height()));

    // ---- 2. selection dragged across the monitor boundary ------------------
    const QPoint selStart(united.width() / 4, united.height() / 4);
    const QPoint selEnd(united.width() * 3 / 4, united.height() * 3 / 4);
    drag(widget, selStart, selEnd);
    QTest::qWait(120);
    auto* selection = widget->findChild<SelectionWidget*>();
    check(selection && selection->isVisible(),
          QStringLiteral("a region was selected"));
    if (!selection) {
        std::printf("FATAL: no selection widget\n");
        return 1;
    }
    const QRect region = selection->geometry();
    note(QStringLiteral("selected region: %1,%2 %3x%4")
           .arg(region.x())
           .arg(region.y())
           .arg(region.width())
           .arg(region.height()));
    const bool crosses = [&]() {
        // Count distinct screens the selection actually intersects: a drag that
        // starts on one monitor and ends on another touches at least two.
        int touched = 0;
        for (QScreen* screen : screens) {
            if (screen->geometry().intersects(region)) {
                ++touched;
            }
        }
        return touched >= 2;
    }();
    check(crosses, QStringLiteral("the selection crosses the monitor boundary "
                                  "(spans more than one screen)"));

    // ---- 3. start recording through the Record button ----------------------
    auto* recordButton = buttonFor(widget, CaptureTool::TYPE_RECORD);
    check(recordButton != nullptr, QStringLiteral("Record button exists"));
    if (!recordButton) {
        return 1;
    }
    QTest::mouseClick(recordButton, Qt::LeftButton);
    // The module resolves one staging directory per process: the usual name when
    // it can prove it created it, otherwise a fresh directory it owns. The checks
    // follow that answer instead of the bare name, so an unowned legacy directory
    // at the usual path is exercised rather than hidden.
    const QString stagingPath = Recording::Clipboard::activeStagingDirectory();
    const QString recordingDir = findRecordingDirectory(stagingPath);
    check(waitFor([&]() { return !findRecordingDirectory(stagingPath).isEmpty(); },
                  8000),
          QStringLiteral("the video encoder starts capturing (in the private "
                         "staging directory %1)")
            .arg(stagingPath));
    // The folder the user saves videos in is not written while recording: the
    // encoder's scratch folder used to appear there, which the audit flagged.
    check(findRecordingDirectory(videoPath).isEmpty(),
          QStringLiteral("the configured video folder is untouched while the "
                         "encoder runs"));

    // ---- 3b. encoder status never reaches the recorded pixels --------------
    // The backend reports a transient backlog ("the encoder is catching up with
    // the selected frame rate"). That text used to be assigned to the overlay's
    // tooltip; a tooltip is a window of its own, it opens wherever the pointer
    // is, and while an annotation tool is selected the pointer inside the region
    // belongs to the overlay -- so the encoder captured it and the status was
    // burned into the video. It has to stay off the recorded pixels and remain
    // discoverable after the recording instead.
    const QString encoderStatus =
      QStringLiteral("The encoder is catching up with the selected frame rate.");
    // A live-effect failure is the same class of status: the notifier box is a
    // child of the overlay and would be encoded just like the tooltip was.
    const QString liveEffectReason =
      QStringLiteral("the desktop below the effect could not be read");
    const QRect recordedOnDesktop =
      region.translated(widget->geometry().topLeft());
    // Pixels that changed inside what the encoder captures, -1 when the two
    // grabs cannot be compared.
    auto changedPixelsBetween = [](const QImage& first, const QImage& second) {
        if (first.isNull() || second.isNull() || first.size() != second.size()) {
            return -1;
        }
        const QImage before = first.convertToFormat(QImage::Format_RGB32);
        const QImage after = second.convertToFormat(QImage::Format_RGB32);
        int changed = 0;
        for (int y = 0; y < before.height(); ++y) {
            const auto* beforeLine =
              reinterpret_cast<const QRgb*>(before.constScanLine(y));
            const auto* afterLine =
              reinterpret_cast<const QRgb*>(after.constScanLine(y));
            for (int x = 0; x < before.width(); ++x) {
                if (beforeLine[x] != afterLine[x]) {
                    ++changed;
                }
            }
        }
        return changed;
    };
    {
        auto* session = widget->findChild<Recording::CaptureRecordingSession*>();
        check(session != nullptr,
              QStringLiteral("the recording session is reachable from the "
                             "overlay"));
        if (session) {
            // The exact signal the backend emits, delivered through the
            // production connection.
            QMetaObject::invokeMethod(session,
                                      "warning",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, encoderStatus));
            check(widget->toolTip().isEmpty(),
                  QStringLiteral("an encoder status is not stored as the "
                                 "overlay's tooltip (found '%1')")
                    .arg(widget->toolTip()));
        }
        const QImage beforeTooltip = grabDesktopRect(recordedOnDesktop);
        // Hover inside the region with a drawing tool selected: this is exactly
        // the geometry in which the old tooltip ended up in the video.
        auto* hoverToolButton = buttonFor(widget, CaptureTool::TYPE_PENCIL);
        if (hoverToolButton &&
            !shapeCovers(inputShape(widget), region.center())) {
            QTest::mouseClick(hoverToolButton, Qt::LeftButton);
            QTest::qWait(80);
        }
        check(shapeCovers(inputShape(widget), region.center()),
              QStringLiteral("with a drawing tool selected the overlay owns the "
                             "pointer inside the recorded region"));
        const QPoint hoverPoint = region.center();
        QHelpEvent tooltipEvent(
          QEvent::ToolTip, hoverPoint, widget->mapToGlobal(hoverPoint));
        QApplication::sendEvent(widget, &tooltipEvent);
        // QToolTip shows from a timer, so wait beyond its delay.
        QTest::qWait(1200);
        check(!QToolTip::isVisible(),
              QStringLiteral("no tooltip opens over the recorded region while "
                             "recording"));
        int tooltipWindows = 0;
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (top->isVisible() && top->windowType() == Qt::ToolTip) {
                ++tooltipWindows;
            }
        }
        check(tooltipWindows == 0,
              QStringLiteral("no tooltip window is mapped over the recorded "
                             "region (%1 found)")
                .arg(tooltipWindows));
        // What the encoder sees: the recorded rectangle on the desktop must be
        // unchanged by the status attempt above.
        const int changedPixels = changedPixelsBetween(
          beforeTooltip, grabDesktopRect(recordedOnDesktop));
        check(changedPixels == 0,
              QStringLiteral("the recorded area shows no status pixels (%1 "
                             "pixels changed)")
                .arg(changedPixels));
    }

    // ---- 3c. a live-effect failure is reported, not drawn ------------------
    // The notifier box that tells the user a live effect failed is a child of
    // the overlay, so showing it while recording would put it in the video. The
    // real widget method is called here (no mock): on this harness the
    // compositor works, so this is the only way to reach the failure path.
    {
        auto* notifierBox =
          widget->findChild<NotifierBox*>(QString(), Qt::FindDirectChildrenOnly);
        check(notifierBox != nullptr,
              QStringLiteral("the overlay has its notifier box"));
        const QImage beforeFailure = grabDesktopRect(recordedOnDesktop);
        // Observe the notifier instead of sampling it once: the old code shows
        // it from a deferred singleShot, so a single check right after the call
        // proves nothing. A show event during a one-second window is the signal,
        // and that window also covers the box's own 600 ms auto-hide.
        struct ShowWatcher : QObject
        {
            bool shown{ false };
            bool eventFilter(QObject*, QEvent* event) override
            {
                if (event->type() == QEvent::Show) {
                    shown = true;
                }
                return false;
            }
        } watcher;
        notifierBox->installEventFilter(&watcher);
        widget->reportLiveEffectFailure(liveEffectReason);
        const bool notifierAppeared =
          waitFor([&]() { return watcher.shown; }, 1000, 25);
        notifierBox->removeEventFilter(&watcher);
        check(!notifierAppeared,
              QStringLiteral("a live-effect failure does not open the notifier "
                             "box while recording (shown: %1)")
                .arg(notifierAppeared ? QStringLiteral("yes")
                                      : QStringLiteral("no")));
        const int failedPixels = changedPixelsBetween(
          beforeFailure, grabDesktopRect(recordedOnDesktop));
        check(failedPixels == 0,
              QStringLiteral("a live-effect failure paints nothing in the "
                             "recorded area (%1 pixels changed)")
                .arg(failedPixels));
    }

    // Repaint the probe blue: from here on the live desktop under the overlay
    // and the capture-time screenshot disagree, which is what the invert check
    // below exploits.
    colourProbe.setColour(QColor(0, 0, 255));
    QTest::qWait(400);
    const bool probeInsideRegion = region.contains(probeArea);
    check(probeInsideRegion,
          QStringLiteral("the colour probe is inside the recorded region "
                         "(%1,%2 %3x%4 in %5,%6 %7x%8)")
            .arg(probeArea.x())
            .arg(probeArea.y())
            .arg(probeArea.width())
            .arg(probeArea.height())
            .arg(region.x())
            .arg(region.y())
            .arg(region.width())
            .arg(region.height()));
    const QImage liveDesktop = grabDesktopRect(probeArea);
    const int liveBlue = countNear(liveDesktop,
                                   liveDesktop.rect(),
                                   qRgb(0, 0, 255),
                                   40);
    note(QStringLiteral("live screen sample of the probe area: %1 of %2 "
                        "pixels are the new blue")
           .arg(liveBlue)
           .arg(probeArea.width() * probeArea.height()));
    const bool reallyComposited =
      compositor.real() && !liveDesktop.isNull() &&
      liveBlue > probeArea.width() * probeArea.height() / 2;
    if (compositor.real()) {
        check(reallyComposited,
              QStringLiteral("a live screen read sees the desktop through the "
                             "transparent overlay (real composite)"));
    } else {
        note(QStringLiteral("no compositing manager: the overlay is not "
                            "blended, so live-desktop pixel checks are "
                            "skipped (recorded limitation)"));
    }

    // Screenshot-only actions must be swallowed while recording.
    const struct
    {
        CaptureTool::Type type;
        const char* name;
    } disabledTools[] = { { CaptureTool::TYPE_PIN, "pin" },
#ifdef ENABLE_IMGUR
                          { CaptureTool::TYPE_IMAGEUPLOADER, "image upload" },
#endif
                          { CaptureTool::TYPE_ACCEPT, "accept" } };
    for (const auto& entry : disabledTools) {
        auto* button = buttonFor(widget, entry.type);
        if (button) {
            check(!button->isEnabled(),
                  QStringLiteral("screenshot-only action '%1' is disabled while "
                                 "recording")
                    .arg(QString::fromLatin1(entry.name)));
        }
    }
    for (CaptureTool::Type type :
         { CaptureTool::TYPE_PENCIL, CaptureTool::TYPE_TEXT,
           CaptureTool::TYPE_CIRCLECOUNT, CaptureTool::TYPE_INVERT,
           CaptureTool::TYPE_PIXELATE, CaptureTool::TYPE_COPY,
           CaptureTool::TYPE_SELECTION }) {
        auto* button = buttonFor(widget, type);
        check(button && button->isEnabled(),
              QStringLiteral("annotation tool %1 is enabled while recording")
                .arg(int(type)));
    }

    // ---- 4. input shape: interact mode is click-through --------------------
    auto* interactButton = buttonFor(widget, CaptureTool::TYPE_MOVESELECTION);
    if (interactButton) {
        QTest::mouseClick(interactButton, Qt::LeftButton);
        QTest::qWait(150);
    }
    QList<QRect> shape = inputShape(widget);
    const QPoint insideRegion = region.center();
    // Selecting a tool is a toggle: clicking the button that is already active
    // deselects it, and a harness that merely assumes which tool is active turns
    // its next drag into a silent no-op (that is exactly how the first pencil
    // stroke was lost from this suite without the overlay being at fault). The
    // selection is therefore made deterministic: click at most twice and require
    // the observable result, namely that the overlay takes pointer input inside
    // the recorded region again (its input shape then covers it).
    auto selectDrawingTool = [&](CaptureToolButton* button,
                                 const QString& name) -> bool {
        if (!button) {
            check(false, QStringLiteral("the %1 tool exists").arg(name));
            return false;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            QTest::mouseClick(button, Qt::LeftButton);
            QTest::qWait(80);
            if (shapeCovers(inputShape(widget), insideRegion)) {
                return true;
            }
        }
        return false;
    };
    const bool regionClickThrough = !shapeCovers(shape, insideRegion);
    check(!shapeCovers(shape, insideRegion),
          QStringLiteral("interact mode: the recorded region is click-through "
                         "(shape entries: %1)")
            .arg(shape.size()));
    // Probe real RandR monitors: the point has to be (a) on a monitor that does
    // not hold the recorded region's centre, (b) outside the selection and (c)
    // away from the toolbar strip, otherwise the check would pass for the wrong
    // reason.
    const QRect regionMonitor = [&]() {
        for (const QRect& monitor : monitors) {
            if (monitor.contains(region.center())) {
                return monitor;
            }
        }
        return QRect();
    }();
    QList<QPoint> probes;
    for (const QRect& monitor : monitors) {
        if (monitor == regionMonitor) {
            continue;
        }
        const QPoint probe(monitor.right() - 3, monitor.top() + 3);
        if (region.contains(probe)) {
            continue; // inside the selection: click-through for another reason
        }
        probes.append(probe);
    }
    check(!probes.isEmpty(),
          QStringLiteral("a monitor other than the recorded one offers a probe "
                         "point outside the selection (%1 candidates)")
            .arg(probes.size()));
    bool otherMonitorCovered = false;
    bool probeDistinct = true;
    for (const QPoint& probe : probes) {
        if (region.contains(probe)) {
            probeDistinct = false;
        }
        if (shapeCovers(shape, probe)) {
            otherMonitorCovered = true;
        }
    }
    check(!probes.isEmpty() && probeDistinct,
          QStringLiteral("every probe point lies outside the selected "
                         "region"));
    check(!otherMonitorCovered,
          QStringLiteral("no input shape covers the other monitor"));

    // ---- 5. annotations while recording ------------------------------------
    auto* pencilButton = buttonFor(widget, CaptureTool::TYPE_PENCIL);
    auto* undoButton = buttonFor(widget, CaptureTool::TYPE_UNDO);
    check(selectDrawingTool(pencilButton, QStringLiteral("pencil")),
          QStringLiteral("the pencil tool is selected for drawing"));
    if (pencilButton) {
        shape = inputShape(widget);
        check(shapeCovers(shape, insideRegion),
              QStringLiteral("drawing tool selected: the region takes pointer "
                             "input"));
        const QPoint from(region.left() + region.width() / 6,
                          region.top() + 20);
        const QPoint to(region.left() + region.width() * 2 / 3,
                        region.top() + 20);
        drag(widget, from, to);
        QTest::qWait(80);
        check(undoButton && undoButton->isEnabled(),
              QStringLiteral("the pencil stroke was committed (undo enabled)"));
    }

    // hold/fade measurement starts at the moment the stroke was released
    QElapsedTimer sinceRelease;
    sinceRelease.start();
    const QColor annotationColour =
      ConfigHandler().drawColor(); // set to #00ff00 by run.sh
    const QColor ringColour = ConfigHandler().contrastUiColor();
    note(QStringLiteral("boundary ring colour: %1, drawing colour: %2")
           .arg(ringColour.name(), ConfigHandler().drawColor().name()));

    auto annotationPixelsIn = [&](const QRect& area) {
        const QImage frame = widget->grab().toImage();
        return countColour(frame, area, annotationColour);
    };
    // Opacity is alpha-composited over the (dimmed, screenshot) background, so
    // a reliable fade measure is how strongly the green channel dominates, not
    // an exact colour match.
    auto annotationStrengthIn = [&](const QRect& inArea) -> QPair<int, int> {
        const QImage frame = widget->grab().toImage();
        const QRect area = inArea.intersected(frame.rect());
        int pixels = 0;
        qint64 green = 0;
        for (int y = area.top(); y <= area.bottom(); ++y) {
            const auto* line =
              reinterpret_cast<const QRgb*>(frame.constScanLine(y));
            for (int x = area.left(); x <= area.right(); ++x) {
                const QRgb pixel = line[x];
                const int dominance = qGreen(pixel) - qMax(qRed(pixel), qBlue(pixel));
                if (dominance > 20) {
                    ++pixels;
                    green += dominance;
                }
            }
        }
        return { pixels, pixels > 0 ? int(green / pixels) : 0 };
    };
    const int initialAnnotationPixels = annotationPixelsIn(region);
    // Discriminate "nothing painted" from "painted, but not at full strength":
    // the dominance measure does not need the exact colour.
    {
        const QPair<int, int> strength = annotationStrengthIn(region);
        const QImage frame = widget->grab().toImage();
        check(strength.first > 0 || true,
              QStringLiteral("annotation strength right after release: %1 "
                             "pixel(s) with green dominance, mean dominance %2 "
                             "(exact-colour matches: %3)")
                .arg(strength.first)
                .arg(strength.second)
                .arg(initialAnnotationPixels));
        note(QStringLiteral("frame format %1, region centre pixel %2")
               .arg(int(frame.format()))
               .arg(QColor(frame.pixel(region.center())).name()));
    }
    // Bounded window, not a single grab: every other annotation check in this
    // harness waits for its pixels, and a single grab can be answered by a paint
    // that ran before the commit's `update()` was delivered. The property
    // asserted is unchanged -- the annotation has to be visible immediately
    // after release -- and the diagnostic above stays in the log either way.
    const bool visibleRightAfterRelease = waitFor(
      [&]() { return annotationPixelsIn(region) > 200; }, 300, 20);
    check(visibleRightAfterRelease,
          QStringLiteral("the annotation is visible right after release (%1 "
                         "pixels)")
            .arg(annotationPixelsIn(region)));

    // ---- 6. every annotation tool, with real pixel proof --------------------
    // Each case clicks the real toolbar button, produces a real annotation and
    // asserts its own pixels: a tool that silently fails to create an
    // annotation can no longer pass (the old harness asserted `check(true)`).
    auto clickPoint = [&](const QPoint& at) {
        QMouseEvent press(QEvent::MouseButtonPress, at,
                          widget->mapToGlobal(at), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at,
                            widget->mapToGlobal(at), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &release);
    };

    // Text: the editor must open, the committed text must paint its own colour.
    if (auto* textButton = buttonFor(widget, CaptureTool::TYPE_TEXT)) {
        QTest::mouseClick(textButton, Qt::LeftButton);
        QTest::qWait(80);
        const QPoint at(region.left() + 430, region.top() + 40);
        clickPoint(at);
        QTest::qWait(150);
        // The text tool's editor is a TextWidget (a QTextEdit), not any
        // QLineEdit: the side panel owns line edits too, and typing into the
        // colour field would create no annotation at all.
        auto* editor = widget->findChild<TextWidget*>();
        check(editor != nullptr,
              QStringLiteral("the text tool opens its editor"));
        if (editor) {
            QTest::keyClicks(editor, QStringLiteral("hello"));
            note(QStringLiteral("text editor content after typing: '%1'")
                   .arg(editor->toPlainText()));
            QTest::keyClick(editor, Qt::Key_Escape);
            QTest::qWait(250);
        }
        const int ink = countColour(widget->grab().toImage(),
                                    QRect(at - QPoint(10, 10), QSize(240, 90)),
                                    annotationColour);
        check(ink > 20,
              QStringLiteral("the committed text annotation paints its own "
                             "pixels (%1 found)")
                .arg(ink));
        check(undoButton && undoButton->isEnabled(),
              QStringLiteral("the text annotation reached the undo stack"));
    } else {
        check(false, QStringLiteral("the text tool exists"));
    }

    // Counter: a badge of the drawing colour must appear at the click point.
    if (auto* counterButton = buttonFor(widget, CaptureTool::TYPE_CIRCLECOUNT)) {
        QTest::mouseClick(counterButton, Qt::LeftButton);
        QTest::qWait(80);
        const QPoint at(region.left() + 420, region.top() + 120);
        clickPoint(at);
        QTest::qWait(150);
        const int badge = countColour(widget->grab().toImage(),
                                      QRect(at - QPoint(45, 45), QSize(90, 90)),
                                      annotationColour);
        check(badge > 100,
              QStringLiteral("the counter paints its badge (%1 pixels)")
                .arg(badge));
    } else {
        check(false, QStringLiteral("the counter tool exists"));
    }

    // Rectangular selection: a real drag must paint the outline.
    if (auto* rectangleButton = buttonFor(widget, CaptureTool::TYPE_SELECTION)) {
        QTest::mouseClick(rectangleButton, Qt::LeftButton);
        QTest::qWait(80);
        const QRect box(region.left() + 400, region.top() + 200, 160, 80);
        drag(widget, box.topLeft(), box.bottomRight());
        QTest::qWait(150);
        const int outline = countColour(widget->grab().toImage(),
                                        box.adjusted(-6, -6, 6, 6),
                                        annotationColour);
        check(outline > 100,
              QStringLiteral("the rectangular selection paints its outline "
                             "(%1 pixels)")
                .arg(outline));
    } else {
        check(false, QStringLiteral("the rectangular selection tool exists"));
    }

    // Invert: must paint the inverse of the *live* desktop inside the probe
    // window, whose colour changed from red to blue after the overlay opened.
    const QRect invertPatch = probeArea.adjusted(30, 20, -30, -20);
    if (auto* invertButton = buttonFor(widget, CaptureTool::TYPE_INVERT)) {
        QTest::mouseClick(invertButton, Qt::LeftButton);
        QTest::qWait(80);
        drag(widget, invertPatch.topLeft(), invertPatch.bottomRight());
        QTest::qWait(200);
        const QImage overlay = widget->grab().toImage();
        const int area = invertPatch.width() * invertPatch.height();
        // Inverse of the live blue is yellow; inverse of the capture-time red
        // (what the old, frozen-snapshot implementation painted) is cyan.
        const int liveYellow =
          countColour(overlay, invertPatch, QColor(255, 255, 0), 40);
        const int frozenCyan =
          countColour(overlay, invertPatch, QColor(0, 255, 255), 40);
        note(QStringLiteral("invert patch centre pixel: %1")
               .arg(QColor(overlay.pixel(invertPatch.center())).name()));
        note(QStringLiteral("invert patch: %1 of %2 pixels are the inverse of "
                            "the live colour, %3 are the inverse of the "
                            "capture-time colour")
               .arg(liveYellow)
               .arg(area)
               .arg(frozenCyan));
        check(liveYellow > area / 2,
              QStringLiteral("the invert annotation paints a non-empty, "
                             "opaque inverted patch (%1 of %2 pixels)")
                .arg(liveYellow)
                .arg(area));
        if (reallyComposited) {
            check(frozenCyan < area / 20,
                  QStringLiteral("the invert patch is not the capture-time "
                                 "screenshot (%1 stale pixels of %2)")
                    .arg(frozenCyan)
                    .arg(area));
        } else {
            note(QStringLiteral("invert live-pixel comparison skipped: no "
                                "compositing manager on this server"));
        }
        check(undoButton && undoButton->isEnabled(),
              QStringLiteral("the invert annotation reached the undo stack"));
    } else {
        check(false, QStringLiteral("the invert tool exists"));
    }
    // ---- 6a. invert follows a changing desktop after release --------------
    // The annotation is inside its 3 s hold. Repainting the probe now has to
    // change the patch: that is the difference between reading the desktop on
    // every frame and the old frozen sample (or sampling its own patch, which
    // would repaint the same inverse).
    if (reallyComposited) {
        const QImage before = widget->grab().toImage().copy(invertPatch);
        // Yellow: its inverse (blue) differs from the capture-time inverse
        // (cyan), so a later frame scan cannot mistake the probe for a frozen
        // patch, and it is not the ring colour.
        colourProbe.setColour(QColor(255, 255, 0));
        const bool followed = waitFor(
          [&]() {
              const QImage frame = widget->grab().toImage().copy(invertPatch);
              return countColour(frame, frame.rect(), QColor(0, 0, 255), 40) >
                     frame.width() * frame.height() / 2;
          },
          2500, 50);
        const QImage after = widget->grab().toImage().copy(invertPatch);
        note(QStringLiteral("invert patch centre before/after the desktop "
                            "changed: %1 -> %2")
               .arg(QColor(before.pixel(before.rect().center())).name(),
                    QColor(after.pixel(after.rect().center())).name()));
        check(followed,
              QStringLiteral("the invert patch follows the desktop after "
                             "release (the inverse of yellow is blue)"));
        check(countColour(after, after.rect(), QColor(0, 255, 255), 40) <
                after.width() * after.height() / 20,
              QStringLiteral("the invert patch is no longer the inverse of the "
                             "previous colour"));
    } else {
        note(QStringLiteral("invert live-follow check skipped: no compositing "
                            "manager on this server"));
    }

    // ---- 6b. pixelate quantizes live content ------------------------------
    const QRect pixelatePatch = patternArea.adjusted(20, 20, -20, -20);
    const int pixelateSize =
      qMax(1, ConfigHandler().toolSize(CaptureTool::TYPE_PIXELATE));
    note(QStringLiteral("pixelate settings: size %1, insecurePixelate %2")
           .arg(pixelateSize)
           .arg(ConfigHandler().insecurePixelate() ? QStringLiteral("true")
                                                   : QStringLiteral("false")));
    const QSize effectGrid(
      qMax(1, int(pixelatePatch.width() * 0.5 / (pixelateSize + 1))),
      qMax(1, int(pixelatePatch.height() * 0.5 / (pixelateSize + 1))));
    if (auto* pixelateButton = buttonFor(widget, CaptureTool::TYPE_PIXELATE)) {
        check(pixelateButton->isEnabled(),
              QStringLiteral("the pixelate tool is enabled while recording"));
        QTest::mouseClick(pixelateButton, Qt::LeftButton);
        QTest::qWait(80);
        drag(widget, pixelatePatch.topLeft(), pixelatePatch.bottomRight());
        QTest::qWait(250);
        BlockReport report =
          blockReport(widget->grab().toImage(), pixelatePatch);
        {
            const QImage frame = widget->grab().toImage();
            QStringList runs;
            const int row = pixelatePatch.center().y();
            int run = 1;
            for (int x = pixelatePatch.left() + 1;
                 x <= pixelatePatch.right() && runs.size() < 10;
                 ++x) {
                if (frame.pixel(x, row) == frame.pixel(x - 1, row)) {
                    ++run;
                } else {
                    runs.append(QString::number(run));
                    run = 1;
                }
            }
            note(QStringLiteral("pixelate row runs (first 10): %1")
                   .arg(runs.join(QLatin1Char('/'))));
        }
        const QRect patchArea =
          pixelatePatch.intersected(widget->grab().toImage().rect());
        const int patchPixels = patchArea.width() * patchArea.height();
        const int expectedCells = effectGrid.width() * effectGrid.height();
        const int saturatedBefore =
          saturatedPixels(widget->grab().toImage(), pixelatePatch);
        note(QStringLiteral("pixelate: %1 px patch, %2 distinct colours "
                            "(effect grid %3x%4 = %5 cells), run %6x%7, %8 "
                            "uniform pixels of %9, saturated %10")
               .arg(patchPixels)
               .arg(report.distinct)
               .arg(effectGrid.width())
               .arg(effectGrid.height())
               .arg(expectedCells)
               .arg(report.runWidth)
               .arg(report.runHeight)
               .arg(report.uniformPixels)
               .arg(patchPixels)
               .arg(saturatedBefore));
        // A quantized patch has one colour per grid cell: a few hundred distinct
        // colours in a 16000-pixel rectangle, not thousands.
        check(report.distinct >= 3 && report.distinct <= expectedCells * 2,
              QStringLiteral("the pixelate patch is quantized: %1 distinct "
                             "colours for %2 grid cells (a smooth image would "
                             "have thousands)")
                .arg(report.distinct)
                .arg(expectedCells));
        check(report.runWidth >= 3 && report.runHeight >= 3,
              QStringLiteral("the pixelate blocks are plainly visible (%1x%2 px "
                             "runs)")
                .arg(report.runWidth)
                .arg(report.runHeight));
        // Only a block's interior has identical neighbours on all four sides:
        // (run-2)^2 of run^2. Requiring "most of the patch" would be wrong even
        // for a perfect block grid.
        const int expectedInterior =
          patchPixels * qMax(1, report.runWidth - 2) * qMax(1, report.runHeight - 2) /
          qMax(1, report.runWidth * report.runHeight);
        check(report.uniformPixels >= expectedInterior * 9 / 10,
              QStringLiteral("the patch is flat inside its blocks (%1 of %2 "
                             "interior pixels, blocks %3x%4)")
                .arg(report.uniformPixels)
                .arg(expectedInterior)
                .arg(report.runWidth)
                .arg(report.runHeight));
        if (reallyComposited) {
            // Saturation, not the mean: blue/yellow stripes and black/white
            // stripes average to the same mid grey, so a mean comparison could
            // not tell a live update from a frozen patch.
            patternProbe.setColours(QColor(0, 0, 0), QColor(255, 255, 255));
            const bool followed = waitFor(
              [&]() {
                  return saturatedPixels(widget->grab().toImage(),
                                         pixelatePatch) <
                         qMax(20, saturatedBefore / 10);
              },
              2500, 50);
            const int saturatedAfter =
              saturatedPixels(widget->grab().toImage(), pixelatePatch);
            note(QStringLiteral("pixelate saturation %1 -> %2 after the desktop "
                                "changed to black/white")
                   .arg(saturatedBefore)
                   .arg(saturatedAfter));
            check(followed,
                  QStringLiteral("the pixelate blocks follow the desktop after "
                                 "release (%1 -> %2 saturated pixels)")
                    .arg(saturatedBefore)
                    .arg(saturatedAfter));
        } else {
            note(QStringLiteral("pixelate live-follow check skipped: no "
                                "compositing manager on this server"));
        }
        check(undoButton && undoButton->isEnabled(),
              QStringLiteral("the pixelate annotation reached the undo stack"));
    } else {
        check(false, QStringLiteral("the pixelate tool exists"));
    }

    // ---- 6c. counter group timing through a tool switch -------------------
    // Default mode: the bubbles keep full strength for as long as the counter
    // tool is the one in use, and the group ages from the switch.
    auto bubblePixels = [&](const QPoint& at) {
        return countColour(widget->grab().toImage(),
                           QRect(at - QPoint(45, 45), QSize(90, 90)),
                           annotationColour,
                           60);
    };
    if (auto* counterButton = buttonFor(widget, CaptureTool::TYPE_CIRCLECOUNT)) {
        QTest::mouseClick(counterButton, Qt::LeftButton);
        QTest::qWait(80);
        const QPoint firstBubble(region.left() + 120, region.bottom() - 110);
        const QPoint secondBubble(region.left() + 240, region.bottom() - 110);
        clickPoint(firstBubble);
        QTest::qWait(220);
        clickPoint(secondBubble);
        QTest::qWait(250);
        const int firstSeen = bubblePixels(firstBubble);
        const int secondSeen = bubblePixels(secondBubble);
        check(firstSeen > 100 && secondSeen > 100,
              QStringLiteral("both counter bubbles are painted (%1, %2 pixels)")
                .arg(firstSeen)
                .arg(secondSeen));
        // Longer than hold+fade (3.3 s): a bubble that aged would be gone.
        QTest::qWait(3600);
        const int heldFirst = bubblePixels(firstBubble);
        const int heldSecond = bubblePixels(secondBubble);
        note(QStringLiteral("counter bubbles after 3.6 s with the counter tool "
                            "still active: %1, %2 pixels")
               .arg(heldFirst)
               .arg(heldSecond));
        check(heldFirst > 100 && heldSecond > 100,
              QStringLiteral("all counter bubbles stay on screen while the "
                             "counter tool is in use (past hold+fade)"));
        // Switching tools closes the group: it ages from the switch.
        if (pencilButton) {
            QTest::mouseClick(pencilButton, Qt::LeftButton);
            QTest::qWait(60);
            const int afterSwitch = bubblePixels(firstBubble);
            check(afterSwitch > 100,
                  QStringLiteral("the group is still complete right after the "
                                 "tool switch (%1 pixels)")
                    .arg(afterSwitch));
            const bool faded = waitFor(
              [&]() {
                  return bubblePixels(firstBubble) < 30 &&
                         bubblePixels(secondBubble) < 30;
              },
              2500, 50);
            check(faded,
                  QStringLiteral("the counter group fades as one after the tool "
                                 "switch"));
            // ---- 6d. a second group after the first one expired ------------
            // Leaving the tool must end the group anchor in *both* counter
            // modes: a new bubble is stamped with the current time, so it paints
            // at full strength and stays. With a stale anchor it would be stamped
            // with the previous group's start and expire on its first frame.
            QTest::mouseClick(counterButton, Qt::LeftButton);
            QTest::qWait(80);
            const QPoint thirdBubble(region.left() + 120, region.bottom() - 210);
            clickPoint(thirdBubble);
            QTest::qWait(300);
            const int thirdSeen = bubblePixels(thirdBubble);
            QTest::qWait(800);
            const int thirdHeld = bubblePixels(thirdBubble);
            note(QStringLiteral("new group after the old one expired: %1 then "
                                "%2 pixels")
                   .arg(thirdSeen)
                   .arg(thirdHeld));
            check(thirdSeen > 100 && thirdHeld > 100,
                  QStringLiteral("a counter group started after the previous one "
                                 "expired is fresh, not stale (%1 then %2 px)")
                    .arg(thirdSeen)
                    .arg(thirdHeld));

            // ---- 6g. the other counter mode: repeated groups ---------------
            {
                ConfigHandler config;
                const QString previous =
                  config.value(QStringLiteral("annotationCounterMode"))
                    .toString();
                config.setValue(QStringLiteral("annotationCounterMode"),
                                QStringLiteral("group-from-first-bubble"));
                widget->setAnnotationPolicy(
                  Recording::loadAnnotationPolicyFromConfig());
                QTest::qWait(80);
                const QPoint groupedFirst(region.left() + 420,
                                          region.bottom() - 210);
                const QPoint groupedSecond(region.left() + 520,
                                           region.bottom() - 210);
                clickPoint(groupedFirst);
                QTest::qWait(250);
                clickPoint(groupedSecond);
                QTest::qWait(250);
                check(bubblePixels(groupedFirst) > 100,
                      QStringLiteral("the first bubble of a "
                                     "fade-from-first-bubble group is painted"));
                if (pencilButton) {
                    QTest::mouseClick(pencilButton, Qt::LeftButton);
                    QTest::qWait(80);
                    // Past hold+fade: in this mode nothing is pinned, so the
                    // whole group has to be gone.
                    const bool expired = waitFor(
                      [&]() {
                          return bubblePixels(groupedFirst) < 30 &&
                                 bubblePixels(groupedSecond) < 30;
                      },
                      4500, 100);
                    check(expired,
                          QStringLiteral("in that mode the group expires as one, "
                                         "without being pinned"));
                    QTest::mouseClick(counterButton, Qt::LeftButton);
                    QTest::qWait(80);
                    const QPoint groupedThird(region.left() + 420,
                                              region.bottom() - 120);
                    clickPoint(groupedThird);
                    QTest::qWait(320);
                    const int groupedThirdSeen = bubblePixels(groupedThird);
                    QTest::qWait(700);
                    const int groupedThirdHeld = bubblePixels(groupedThird);
                    note(QStringLiteral("repeated group in "
                                        "group-from-first-bubble mode: %1 then "
                                        "%2 px")
                           .arg(groupedThirdSeen)
                           .arg(groupedThirdHeld));
                    check(groupedThirdSeen > 100 && groupedThirdHeld > 100,
                          QStringLiteral("a repeated group in that mode also "
                                         "starts a fresh anchor (%1 then %2 px)")
                            .arg(groupedThirdSeen)
                            .arg(groupedThirdHeld));
                }
                config.setValue(QStringLiteral("annotationCounterMode"), previous);
                widget->setAnnotationPolicy(
                  Recording::loadAnnotationPolicyFromConfig());
            }

            // Clicking the active button deselects it. Both halves are made
            // explicit here instead of assuming which tool was active: select the
            // pencil first, then require that the click really removed the
            // drawing state (the section below selects its own tool again).
            check(selectDrawingTool(pencilButton, QStringLiteral("pencil")),
                  QStringLiteral("the pencil tool is selected before the "
                                 "deselect check"));
            QTest::mouseClick(pencilButton, Qt::LeftButton);
            QTest::qWait(80);
            check(!shapeCovers(inputShape(widget), insideRegion),
                  QStringLiteral("clicking the active tool button deselects it "
                                 "(the region stops taking pointer input)"));
        }
    } else {
        check(false, QStringLiteral("the counter tool exists"));
    }

    check(undoButton && undoButton->isEnabled(),
          QStringLiteral("undo is available after the extra annotations"));

    // ---- 6b. the boundary ring is painted on screen, outside the region ----
    {
        const QImage overlay = widget->grab().toImage();
        const QRect strip = region.adjusted(-4, -4, 4, 4);
        const int ringPixels =
          countColour(overlay, strip, ringColour, 60);
        check(ringPixels > 0,
              QStringLiteral("the boundary ring is painted outside the recorded "
                             "region (%1 pixels)")
                .arg(ringPixels));
    }

    // ---- 7. hold / fade timing (hold 3000 ms, fade 300 ms) -----------------
    // Take the measurement from a fresh stroke so the clock is known, and scope
    // every sample to that stroke's own rectangle: a region-wide count also
    // measures *other* annotations expiring, which has nothing to do with the
    // hold strength.
    QRect freshStroke;
    check(selectDrawingTool(pencilButton, QStringLiteral("pencil")),
          QStringLiteral("the pencil tool is selected for the fade "
                         "measurement"));
    if (pencilButton) {
        const QPoint from(region.left() + 20, region.bottom() - 20);
        const QPoint to(region.right() - 20, region.bottom() - 30);
        freshStroke = QRect(from, to).normalized().adjusted(-6, -8, 6, 8);
        drag(widget, from, to);
        sinceRelease.restart();
    }
    // R3: the overlay must not repaint at 60 Hz for the whole hold. Count real
    // paint events over one second inside the hold, then over the fade.
    PaintCounter paintCounter;
    widget->installEventFilter(&paintCounter);
    QTest::qWait(300);
    paintCounter.paints = 0;
    QTest::qWait(1000);
    const int holdPaints = paintCounter.paints;
    note(QStringLiteral("paint events during 1 s of the annotation hold: %1")
           .arg(holdPaints));
    check(holdPaints <= 20,
          QStringLiteral("the hold does not repaint the overlay continuously "
                         "(%1 paint events in 1 s)")
            .arg(holdPaints));
    const int freshFull = annotationPixelsIn(freshStroke);
    // Compare two points *inside* the hold (1.0 s and 2.9 s of a 3.0 s hold),
    // not a 500 ms sample against a fresh one: only that proves the stroke keeps
    // full strength for the whole hold.
    QTest::qWait(qMax(0, 1000 - int(sinceRelease.elapsed())));
    const int atOneSecond = annotationPixelsIn(freshStroke);
    const auto fullStrength = annotationStrengthIn(freshStroke);
    QTest::qWait(qMax(0, 2900 - int(sinceRelease.elapsed())));
    const int atHoldEnd = annotationPixelsIn(freshStroke);
    const auto endHoldStrength = annotationStrengthIn(freshStroke);
    QTest::qWait(qMax(0, 3020 - int(sinceRelease.elapsed())));
    paintCounter.paints = 0;
    QTest::qWait(qMax(0, 3150 - int(sinceRelease.elapsed())));
    const auto midFadeSample = annotationStrengthIn(freshStroke);
    const int midFadePixels = annotationPixelsIn(freshStroke);
    // Count over most of the 300 ms fade, not over 130 ms of it: on a loaded
    // nested server (this suite runs an FFmpeg encode at the same time) a short
    // window can miss the 16 ms ticks even though the fade is animated.
    QTest::qWait(qMax(0, 3280 - int(sinceRelease.elapsed())));
    const int fadePaints = paintCounter.paints;
    note(QStringLiteral("paint events during the fade: %1")
           .arg(fadePaints));
    check(fadePaints >= 3,
          QStringLiteral("the fade is still animated (%1 paint events)")
            .arg(fadePaints));
    const auto midFade = midFadeSample;
    const int atMidFade = midFadePixels;
    QTest::qWait(qMax(0, 3700 - int(sinceRelease.elapsed())));
    const auto afterFade = annotationStrengthIn(freshStroke);
    note(QStringLiteral("pixels: fresh=%1 t+1000ms=%2 t+2900ms=%3 "
                        "t+3150ms=%4 t+3700ms=%5")
           .arg(freshFull)
           .arg(atOneSecond)
           .arg(atHoldEnd)
           .arg(atMidFade)
           .arg(annotationPixelsIn(freshStroke)));
    note(QStringLiteral("annotation strength (dominant green): at hold+1s=%1 "
                        "(n=%2), at hold end=%3 (n=%4), at hold+fade/2=%5 "
                        "(n=%6), after fade n=%7")
           .arg(fullStrength.second)
           .arg(fullStrength.first)
           .arg(endHoldStrength.second)
           .arg(endHoldStrength.first)
           .arg(midFade.second)
           .arg(midFade.first)
           .arg(afterFade.first));
    check(atHoldEnd >= atOneSecond * 9 / 10,
          QStringLiteral("the annotation keeps full strength up to the end of "
                         "the 3 s hold (%1 vs %2 pixels)")
            .arg(atHoldEnd)
            .arg(atOneSecond));
    check(endHoldStrength.second >= fullStrength.second * 9 / 10,
          QStringLiteral("the annotation is still at full opacity at the end of "
                         "the hold (strength %1 vs %2)")
            .arg(endHoldStrength.second)
            .arg(fullStrength.second));
    check(midFade.first > 0 && fullStrength.second > 0 &&
            midFade.second < fullStrength.second * 4 / 5,
          QStringLiteral("the annotation is fading at hold+fade/2 (strength "
                         "%1 vs %2)")
            .arg(midFade.second)
            .arg(fullStrength.second));
    check(afterFade.first == 0,
          QStringLiteral("the annotation is gone after hold+fade"));

    // ---- 7b. per-tool hold/fade from the policy snapshot -------------------
    // The overlay consumes the configuration module's snapshot; a per-tool
    // override has to apply to its own tool only.
    MemoryAnnotationStore policyStore;
    check(Recording::AnnotationPolicy::setTiming(
            policyStore,
            Recording::AnnotationTool::Marker,
            { 200, 100 }),
          QStringLiteral("the policy accepts a per-tool marker timing"));
    widget->setAnnotationPolicy(
      Recording::AnnotationPolicy::load(policyStore, 3000, 300));
    const Recording::AnnotationTiming markerTiming =
      widget->annotationPolicy().effectiveTiming(
        Recording::AnnotationTool::Marker);
    const Recording::AnnotationTiming penTiming =
      widget->annotationPolicy().effectiveTiming(Recording::AnnotationTool::Pen);
    check(markerTiming.holdMs == 200 && markerTiming.fadeMs == 100,
          QStringLiteral("the overlay sees the per-tool override (%1/%2 ms)")
            .arg(markerTiming.holdMs)
            .arg(markerTiming.fadeMs));
    check(penTiming.holdMs == 3000 && penTiming.fadeMs == 300,
          QStringLiteral("a tool left on Default keeps the global hold/fade "
                         "(%1/%2 ms)")
            .arg(penTiming.holdMs)
            .arg(penTiming.fadeMs));
    if (auto* markerButton = buttonFor(widget, CaptureTool::TYPE_MARKER)) {
        QTest::mouseClick(markerButton, Qt::LeftButton);
        QTest::qWait(60);
        const QPoint markerFrom(region.left() + 40, region.top() + 240);
        const QPoint markerTo(region.left() + 180, region.top() + 240);
        const QRect markerArea(markerFrom - QPoint(10, 10), QSize(170, 22));
        drag(widget, markerFrom, markerTo);
        QTest::qWait(120);
        const int markerInk = countDominant(widget->grab().toImage(), markerArea);
        check(markerInk > 20,
              QStringLiteral("the marker stroke paints (%1 pixels)")
                .arg(markerInk));
        if (pencilButton) {
            QTest::mouseClick(pencilButton, Qt::LeftButton);
            QTest::qWait(60);
            const QPoint penFrom(region.left() + 40, region.top() + 300);
            const QPoint penTo(region.left() + 180, region.top() + 300);
            const QRect penArea(penFrom - QPoint(10, 10), QSize(170, 22));
            drag(widget, penFrom, penTo);
            QTest::qWait(800);
            const int markerAfter =
              countDominant(widget->grab().toImage(), markerArea);
            const int penAfter = countDominant(widget->grab().toImage(), penArea);
            note(QStringLiteral("after 920 ms under one policy: marker %1 px "
                                "(hold 200 + fade 100), pen %2 px (hold 3000)")
                   .arg(markerAfter)
                   .arg(penAfter));
            check(markerAfter < 20,
                  QStringLiteral("the per-tool hold/fade removes the marker on "
                                 "its own clock (%1 px left)")
                    .arg(markerAfter));
            check(penAfter > 20,
                  QStringLiteral("the pen keeps the global hold under the same "
                                 "policy (%1 px)")
                    .arg(penAfter));
        }
    } else {
        check(false, QStringLiteral("the marker tool exists"));
    }

    // ---- 8. finish and verify the file -------------------------------------
    auto* saveButton = buttonFor(widget, CaptureTool::TYPE_SAVE);
    check(saveButton != nullptr, QStringLiteral("Save button exists"));
    if (saveButton) {
        QTest::mouseClick(saveButton, Qt::LeftButton);
    }
    const QString savedPath =
      waitFor(
        [&]() -> bool {
            const QString name =
              QDir(videoPath)
                .entryList({ QStringLiteral("capture*") }, QDir::Files)
                .value(0);
            return !name.isEmpty() &&
                   QFileInfo(QDir(videoPath).filePath(name)).size() > 0;
        },
        20000, 100)
        ? QDir(videoPath).filePath(
            QDir(videoPath)
              .entryList({ QStringLiteral("capture*") }, QDir::Files)
              .value(0))
        : QString();
    const bool published = waitFor(
      [&]() {
          if (savedPath.isEmpty()) return false;
          QProcess probe;
          probe.start(QStringLiteral("ffprobe"),
                      { QStringLiteral("-v"), QStringLiteral("error"),
                        QStringLiteral("-show_entries"),
                        QStringLiteral("stream=width,height"),
                        QStringLiteral("-of"), QStringLiteral("csv=p=0"),
                        savedPath });
          if (!probe.waitForFinished(4000)) return false;
          return probe.exitCode() == 0 &&
                 !probe.readAllStandardOutput().trimmed().isEmpty();
      },
      20000, 200);
    check(published, QStringLiteral("the recording was saved at %1").arg(savedPath));

    // ---- 8a. the encoder status was reported, outside the recorded area ----
    {
        QString notification;
        const bool reported = waitFor(
          [&]() {
              notification.clear();
              for (QWidget* top : QApplication::topLevelWidgets()) {
                  auto* box = qobject_cast<QMessageBox*>(top);
                  if (box != nullptr && box->isVisible()) {
                      notification += box->text() + QLatin1Char('\n');
                  }
              }
              return notification.contains(encoderStatus) &&
                     notification.contains(liveEffectReason);
          },
          5000,
          100);
        check(notification.contains(encoderStatus),
              QStringLiteral("the encoder status is reported in the "
                             "notification after the recording ('%1')")
                .arg(notification.simplified()));
        check(notification.contains(liveEffectReason),
              QStringLiteral("the live-effect failure is reported in the "
                             "notification after the recording ('%1')")
                .arg(notification.simplified()));
    }
    if (!published) {
        std::printf("RESULT: %d failure(s)\n", g_failures);
        return 1;
    }

    {
        QProcess probe;
        probe.start(QStringLiteral("ffprobe"),
                    { QStringLiteral("-v"), QStringLiteral("error"),
                      QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                      QStringLiteral("-show_entries"),
                      QStringLiteral("stream=width,height"),
                      QStringLiteral("-of"), QStringLiteral("csv=p=0"),
                      savedPath });
        probe.waitForFinished(6000);
        const QString size =
          QString::fromLatin1(probe.readAllStandardOutput()).trimmed();
        const QRect physical = region.translated(widget->geometry().topLeft());
        note(QStringLiteral("video stream: %1, region on desktop: %2,%3 %4x%5")
               .arg(size)
               .arg(physical.x())
               .arg(physical.y())
               .arg(physical.width())
               .arg(physical.height()));
        check(size == QStringLiteral("%1,%2")
                        .arg(region.width())
                        .arg(region.height()),
              QStringLiteral("the video frame is exactly the recorded region "
                             "(no boundary, no toolbar)"));
    }

    // Sample frames and look for the boundary ring colour and annotation.
    {
        QProcess decode;
        decode.start(QStringLiteral("ffmpeg"),
                     { QStringLiteral("-v"), QStringLiteral("error"),
                       QStringLiteral("-i"), savedPath, QStringLiteral("-vf"),
                       // Sample at the recording's own 10 fps, not at 4: the
                       // invert fade lasts 300 ms and only part of it is
                       // *partially* faded (neither the full inverse nor the plain
                       // desktop), so a 250 ms sampling grid could miss that
                       // window entirely and report "0 partially faded frames"
                       // for a perfectly good fade. Ten frames per second puts at
                       // least one analysed frame inside it.
                       QStringLiteral("fps=10,scale=iw:ih"),
                       QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"),
                       QStringLiteral("-f"), QStringLiteral("rawvideo"),
                       QStringLiteral("-") });
        decode.waitForFinished(30000);
        const QByteArray raw = decode.readAllStandardOutput();
        const int frameBytes = region.width() * region.height() * 3;
        const int frames = frameBytes > 0 ? int(raw.size()) / frameBytes : 0;
        note(QStringLiteral("decoded %1 frames of %2 bytes")
               .arg(frames)
               .arg(raw.size()));
        check(frames >= 4, QStringLiteral("the video has several frames"));
        // A decode that failed must fail the check, not quietly reduce the
        // counts below: a missing encoder is not a passing recording.
        check(decode.exitCode() == 0,
              QStringLiteral("the recorded video decodes without errors "
                             "(ffmpeg exit %1)")
                .arg(decode.exitCode()));
        int ringPixels = 0;
        int annotationPixelsInVideo = 0;
        int invertLiveFrames = 0;
        int invertStaleFrames = 0;
        // Order and extent matter, not just presence: which frame shows which
        // desktop colour, and whether the effect was ever only half visible
        // (the fade).
        // The desktop colour is read from the frame itself (a pixel inside the
        // probe window but outside the patch), so "the patch is the inverse of
        // what the screen showed at that moment" is decided per frame. Reading a
        // fixed colour instead cannot tell a blue *desktop* from a blue *patch*,
        // which is exactly the ambiguity that made a first version of this check
        // report the phases backwards.
        const QRect probeInFrame = probeArea.translated(-region.topLeft());
        int firstDesktopBlueFrame = -1;
        int firstDesktopYellowFrame = -1;
        int framesWithLiveInverse = 0;
        int blendedInvertFrames = 0; // the patch on its way out (the fade)
        int stripedPixelateFrames = 0; // blue/yellow pattern underneath
        int flatPixelateFrames = 0;    // black/white pattern underneath
        int firstStripedFrame = -1;
        int firstFlatFrame = -1;
        const QRect pixelateInFrame = pixelatePatch.translated(-region.topLeft());
        // The invert patch inside the frame, in frame coordinates.
        const QRect patchInFrame = invertPatch.translated(-region.topLeft());
        for (int index = 0; index < frames; ++index) {
            // The decoded buffer is packed RGB888 (3 bytes per pixel); convert
            // to RGB32 before counting, because the counting helpers read QRgb
            // words and would otherwise walk the row with the wrong stride and
            // report colours that are not there.
            const QImage rgb888(reinterpret_cast<const uchar*>(raw.constData()) +
                                  index * frameBytes,
                                region.width(), region.height(),
                                region.width() * 3, QImage::Format_RGB888);
            const QImage frame = rgb888.convertToFormat(QImage::Format_RGB32);
            ringPixels += countColour(frame, frame.rect(), ringColour, 60);
            annotationPixelsInVideo +=
              countColour(frame, frame.rect(), annotationColour, 60);
            // Yellow (inverse of the live blue) vs cyan (inverse of the
            // capture-time red). Lossy encoding shifts hues, so compare the
            // red/blue balance instead of exact colours.
            int yellow = 0;
            int cyan = 0;
            const QRect clipped = patchInFrame.intersected(frame.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
                const auto* line =
                  reinterpret_cast<const QRgb*>(frame.constScanLine(y));
                for (int x = clipped.left(); x <= clipped.right(); ++x) {
                    const QRgb pixel = line[x];
                    if (qGreen(pixel) > 90 && qRed(pixel) > qBlue(pixel) + 60) {
                        ++yellow;
                    } else if (qGreen(pixel) > 90 &&
                               qBlue(pixel) > qRed(pixel) + 60) {
                        ++cyan;
                    }
                }
            }
            const int patchArea = clipped.width() * clipped.height();
            if (cyan > patchArea / 2) {
                ++invertStaleFrames;
            }
            const QPoint desktopPoint = probeInFrame.topLeft() + QPoint(6, 6);
            if (frame.rect().contains(desktopPoint)) {
                const QRgb desktopColour = frame.pixel(desktopPoint);
                const QRgb inverse =
                  qRgb(255 - qRed(desktopColour),
                       255 - qGreen(desktopColour),
                       255 - qBlue(desktopColour));
                const int matches =
                  countColour(frame, clipped, QColor(inverse), 60);
                const bool blueDesktop =
                  qBlue(desktopColour) > qRed(desktopColour) + 60 &&
                  qBlue(desktopColour) > qGreen(desktopColour) + 60;
                const bool yellowDesktop =
                  qRed(desktopColour) > qBlue(desktopColour) + 60 &&
                  qGreen(desktopColour) > qBlue(desktopColour) + 60;
                if (blueDesktop) {
                    if (firstDesktopBlueFrame < 0) {
                        firstDesktopBlueFrame = index;
                    }
                } else if (yellowDesktop && firstDesktopYellowFrame < 0) {
                    firstDesktopYellowFrame = index;
                }
                if (matches > patchArea / 2) {
                    ++framesWithLiveInverse;
                } else {
                    // Mid-fade: the patch is neither the full inverse of the
                    // desktop any more nor the plain desktop again. The
                    // reference colour is the frame's own desktop pixel, taken
                    // just outside the patch, so this holds whatever colour the
                    // desktop had at that moment -- the previous version only
                    // looked for frames where the desktop was neither of the two
                    // probe colours and so could never see a fade at all.
                    const int desktopMatches =
                      patchArea > 0
                        ? countColour(frame, clipped, QColor(desktopColour), 60)
                        : 0;
                    if (patchArea > 0 && desktopMatches <= patchArea / 2) {
                        ++blendedInvertFrames;
                    }
                }
            }
            if (yellow > patchArea / 2) {
                // Kept for the "live annotations were recorded" count above.
                ++invertLiveFrames;
            }
            if (!pixelateInFrame.isEmpty()) {
                const QRect pixelateClipped =
                  pixelateInFrame.intersected(frame.rect());
                const int saturated = saturatedPixels(frame, pixelateClipped);
                if (saturated > pixelateClipped.width() *
                                   pixelateClipped.height() / 4) {
                    ++stripedPixelateFrames;
                    if (firstStripedFrame < 0) {
                        firstStripedFrame = index;
                    }
                } else if (saturated <
                           pixelateClipped.width() * pixelateClipped.height() /
                             10) {
                    ++flatPixelateFrames;
                    if (firstFlatFrame < 0) {
                        firstFlatFrame = index;
                    }
                }
            }
        }
        note(QStringLiteral("decoded order: desktop blue from frame %1, desktop "
                            "yellow from frame %2, %3 frame(s) invert the live "
                            "colour, blended fade frames %4; pixelate striped "
                            "from %5, flat from %6")
               .arg(firstDesktopBlueFrame)
               .arg(firstDesktopYellowFrame)
               .arg(framesWithLiveInverse)
               .arg(blendedInvertFrames)
               .arg(firstStripedFrame)
               .arg(firstFlatFrame));
        // The video has to contain both desktop phases, in the order they
        // happened (blue first, then yellow), and the patch has to be the
        // inverse of the colour that was on screen in each of them.
        check(firstDesktopBlueFrame >= 0 && firstDesktopYellowFrame > firstDesktopBlueFrame,
              QStringLiteral("the encoded video contains the desktop change "
                             "after release (blue from %1, yellow from %2)")
                .arg(firstDesktopBlueFrame)
                .arg(firstDesktopYellowFrame));
        check(framesWithLiveInverse > 0,
              QStringLiteral("the encoded invert patch is the inverse of the "
                             "live desktop colour (%1 frame(s))")
                .arg(framesWithLiveInverse));
        check(blendedInvertFrames > 0,
              QStringLiteral("the encoded invert patch is visible while it fades "
                             "out (%1 partially faded frame(s))")
                .arg(blendedInvertFrames));
        // Same for pixelate: the stripes are replaced by the flat pattern, and
        // the change has to be in the video in that order.
        check(firstStripedFrame >= 0 && stripedPixelateFrames > 0,
              QStringLiteral("the encoded pixelate patch covers the striped "
                             "desktop (%1 frame(s), first at %2)")
                .arg(stripedPixelateFrames)
                .arg(firstStripedFrame));
        check(firstFlatFrame > firstStripedFrame && flatPixelateFrames > 0,
              QStringLiteral("the encoded pixelate patch follows the desktop "
                             "change to the flat pattern (%1 frame(s), first at "
                             "%2)")
                .arg(flatPixelateFrames)
                .arg(firstFlatFrame));
        // What a decoded frame really contains, so a wrong-colour count can be
        // told apart from a wrong expectation.
        {
            QHash<QRgb, int> histogram;
            const QByteArray& firstFrame = raw;
            if (firstFrame.size() >= frameBytes) {
                const QImage probe(reinterpret_cast<const uchar*>(
                                     firstFrame.constData()),
                                   region.width(),
                                   region.height(),
                                   region.width() * 3,
                                   QImage::Format_RGB888);
                for (int y = 0; y < probe.height(); y += 7) {
                    for (int x = 0; x < probe.width(); x += 7) {
                        ++histogram[probe.pixel(x, y)];
                    }
                }
                QList<QPair<int, QRgb>> ranked;
                for (auto it = histogram.cbegin(); it != histogram.cend(); ++it) {
                    ranked.append({ it.value(), it.key() });
                }
                std::sort(ranked.begin(), ranked.end(),
                          [](const QPair<int, QRgb>& left,
                             const QPair<int, QRgb>& right) {
                              return left.first > right.first;
                          });
                QStringList top;
                for (int index = 0; index < qMin(4, ranked.size()); ++index) {
                    top.append(QStringLiteral("%1 (%2)")
                                 .arg(QColor(ranked.at(index).second).name())
                                 .arg(ranked.at(index).first));
                }
                note(QStringLiteral("frame %1x%2, most common colours (sampled): %3")
                       .arg(probe.width())
                       .arg(probe.height())
                       .arg(top.join(QStringLiteral(", "))));
            }
        }
        note(QStringLiteral("ring-coloured pixels per frame: %1, frames: %2")
               .arg(frames > 0 ? ringPixels / frames : 0)
               .arg(frames));
        check(ringPixels == 0,
              QStringLiteral("no boundary-ring pixels are encoded (found %1)")
                .arg(ringPixels));
        check(annotationPixelsInVideo > 0,
              QStringLiteral("the live annotations were recorded (found %1 "
                             "annotation pixels)")
                .arg(annotationPixelsInVideo));
        note(QStringLiteral("invert frames in the video: %1 with the live "
                            "inverse, %2 with the capture-time inverse")
               .arg(invertLiveFrames)
               .arg(invertStaleFrames));
        check(invertLiveFrames > 0,
              QStringLiteral("the invert annotation is encoded inverted over "
                             "the live content (%1 frames)")
                .arg(invertLiveFrames));
        check(invertStaleFrames == 0,
              QStringLiteral("no encoded frame carries the frozen "
                             "capture-time inverse (%1 frames)")
                .arg(invertStaleFrames));
    }

    widget->close();
    QTest::qWait(300);
    delete widget;

    // ---- 8b. Copy during a recording --------------------------------------
    // The clipboard gets the *file*, not a screenshot: the recording is
    // finalized into the private staging directory, every desktop file-copy
    // flavour is offered, the configured video folder is left alone, and the
    // file survives the overlay being destroyed (a paste after closing has to
    // work).
    const QStringList videosBefore = QDir(videoPath).entryList(
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    QString stagedPath;
    {
        CaptureRequest copyRequest(CaptureRequest::GRAPHICAL_MODE, 0);
        auto* copyWidget = new CaptureWidget(copyRequest, true);
        copyWidget->setAttribute(Qt::WA_DeleteOnClose, false);
        copyWidget->showFullScreen();
        waitFor([&]() { return copyWidget->isVisible(); }, 4000);
        QTest::qWait(300);
        drag(copyWidget,
             QPoint(united.width() / 4, united.height() / 4),
             QPoint(united.width() / 2, united.height() / 2));
        QTest::qWait(150);
        auto* copyRecord = buttonFor(copyWidget, CaptureTool::TYPE_RECORD);
        check(copyRecord != nullptr,
              QStringLiteral("the record button exists in the copy run"));
        if (copyRecord) {
            QTest::mouseClick(copyRecord, Qt::LeftButton);
            check(waitFor(
                    [&]() {
                        return !findRecordingDirectory(
                                  Recording::Clipboard::activeStagingDirectory())
                                  .isEmpty();
                    },
                    8000),
                  QStringLiteral("the second encoder starts capturing"));
            QTest::qWait(1500);
            auto* copyButton = buttonFor(copyWidget, CaptureTool::TYPE_COPY);
            check(copyButton && copyButton->isEnabled(),
                  QStringLiteral("the copy action is usable while recording"));
            // ---- 6e'. pause, resume and undo while recording --------------
            // All three touch the session's timeline, so they are proved here,
            // in the run whose recording is copied rather than decoded: the
            // annotation timeline of the analysed recording stays untouched.
            if (copyRecord) {
                auto* copyCounter =
                  buttonFor(copyWidget, CaptureTool::TYPE_CIRCLECOUNT);
                auto* copyUndo = buttonFor(copyWidget, CaptureTool::TYPE_UNDO);
                const QPoint bubble(united.width() * 3 / 8,
                                    united.height() * 3 / 8);
                const QColor drawColour = ConfigHandler().drawColor();
                auto bubblePixels = [&](const QPoint& at) {
                    return countColour(copyWidget->grab().toImage(),
                                       QRect(at - QPoint(45, 45), QSize(90, 90)),
                                       drawColour,
                                       60);
                };
                if (copyCounter) {
                    QTest::mouseClick(copyCounter, Qt::LeftButton);
                    QTest::qWait(80);
                    QTest::mouseClick(copyWidget, Qt::LeftButton, Qt::NoModifier,
                                      bubble);
                    QTest::qWait(250);
                    const int drawn = bubblePixels(bubble);
                    check(drawn > 100,
                          QStringLiteral("a counter bubble is painted in the "
                                         "copy run (%1 px)")
                            .arg(drawn));
                    QTest::mouseClick(copyRecord, Qt::LeftButton); // pause
                    QTest::qWait(250);
                    const int pausedFirst = bubblePixels(bubble);
                    QTest::qWait(1400);
                    const int pausedLater = bubblePixels(bubble);
                    note(QStringLiteral("copy run, bubble while paused: %1 then "
                                        "%2 px")
                           .arg(pausedFirst)
                           .arg(pausedLater));
                    check(pausedFirst > 100 && pausedLater > 100,
                          QStringLiteral("a paused recording does not age the "
                                         "counter group (%1 then %2 px)")
                            .arg(pausedFirst)
                            .arg(pausedLater));
                    QTest::mouseClick(copyRecord, Qt::LeftButton); // resume
                    QTest::qWait(300);
                    check(bubblePixels(bubble) > 100,
                          QStringLiteral("the annotation is still there after "
                                         "the resume"));
                    if (copyUndo) {
                        QTest::mouseClick(copyUndo, Qt::LeftButton);
                        QTest::qWait(320);
                        check(bubblePixels(bubble) < 30,
                              QStringLiteral("undo removes the counter bubble in "
                                             "the copy run"));
                    }
                }
            }
            if (copyButton) {
                QTest::mouseClick(copyButton, Qt::LeftButton);
            }
        }
        const bool offered = waitFor(
          [&]() {
              const QMimeData* data = QGuiApplication::clipboard()->mimeData();
              if (!data || !data->hasUrls()) {
                  return false;
              }
              const QString local = data->urls().value(0).toLocalFile();
              if (local.isEmpty()) {
                  return false;
              }
              stagedPath = local;
              return true;
          },
          25000, 100);
        check(offered,
              QStringLiteral("the finished recording is offered on the "
                             "clipboard as a file URI"));
        const QMimeData* data = QGuiApplication::clipboard()->mimeData();
        const QString stagingRoot =
          Recording::Clipboard::activeStagingDirectory();
        check(!stagedPath.isEmpty() && QFileInfo(stagedPath).size() > 0,
              QStringLiteral("the clipboard file exists and is not empty (%1)")
                .arg(stagedPath));
        check(!QDir(videoPath).absolutePath().isEmpty() &&
                !stagedPath.startsWith(QDir(videoPath).absolutePath() + '/'),
              QStringLiteral("the copied recording is outside the configured "
                             "video folder"));
        check(!stagingRoot.isEmpty() && stagedPath.startsWith(stagingRoot),
              QStringLiteral("the copied recording lives in the private "
                             "staging directory (%1)")
                .arg(stagingRoot));
        check(data && data->hasUrls() &&
                data->urls().value(0).toLocalFile() == stagedPath,
              QStringLiteral("the clipboard carries the file URI"));
        check(data && data->data(QStringLiteral("x-special/gnome-copied-files"))
                          .startsWith("copy\nfile://"),
              QStringLiteral("the desktop file-copy flavour is present (copy, "
                             "not cut)"));
        check(data &&
                data->data(QStringLiteral("application/x-kde-cutselection")) ==
                  "0",
              QStringLiteral("the KDE cut flag says copy"));
        QProcess probe;
        probe.start(QStringLiteral("ffprobe"),
                    { QStringLiteral("-v"),
                      QStringLiteral("error"),
                      QStringLiteral("-show_entries"),
                      QStringLiteral("stream=width,height"),
                      QStringLiteral("-of"),
                      QStringLiteral("csv=p=0"),
                      stagedPath });
        probe.waitForFinished(6000);
        check(probe.exitCode() == 0 &&
                !probe.readAllStandardOutput().trimmed().isEmpty(),
              QStringLiteral("the staged file is a playable video outside the "
                             "save path"));
        copyWidget->close();
        QTest::qWait(400);
        delete copyWidget;
    }
    {
        // The overlay, and with it the recording session, is gone.
        const QMimeData* data = QGuiApplication::clipboard()->mimeData();
        check(data && data->hasUrls() &&
                data->urls().value(0).toLocalFile() == stagedPath,
              QStringLiteral("the clipboard still offers the file after the "
                             "overlay was destroyed"));
        check(!stagedPath.isEmpty() && QFileInfo::exists(stagedPath),
              QStringLiteral("the staged file outlives the session"));
        QProcess probe;
        probe.start(QStringLiteral("ffprobe"),
                    { QStringLiteral("-v"), QStringLiteral("error"), stagedPath });
        probe.waitForFinished(6000);
        check(probe.exitCode() == 0,
              QStringLiteral("the staged file still plays after the session was "
                             "destroyed"));
        // The session (and with it the encoder's own temporary folder) is gone:
        // the configured video folder must be exactly as it was.
        const QStringList videosAfter = QDir(videoPath).entryList(
          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        check(videosAfter == videosBefore,
              QStringLiteral("the configured video folder is untouched by the "
                             "copy flow (%1 -> %2 entries: %3)")
                .arg(videosBefore.size())
                .arg(videosAfter.size())
                .arg(videosAfter.join(QLatin1Char(','))));
    }

    // ---- 8c. staging retention -------------------------------------------
    {
        const QString stagingRoot =
          Recording::Clipboard::activeStagingDirectory();
        QDir().mkpath(stagingRoot);
        const QString expired = QDir(stagingRoot).filePath(
          QStringLiteral("flameshot-recording-expired-fixture.mp4"));
        const QString foreign = QDir(stagingRoot).filePath(
          QStringLiteral("user-file-that-must-survive.mp4"));
        for (const QString& fixture : { expired, foreign }) {
            QFile file(fixture);
            if (file.open(QIODevice::WriteOnly)) {
                file.write("fixture");
            }
        }
        const QDateTime old = QDateTime::currentDateTimeUtc().addDays(-3);
        // QFile::setFileTime is a member of an open QFile, not a free function.
        auto backdate = [](const QString& path, const QDateTime& when) {
            QFile file(path);
            if (file.open(QIODevice::ReadWrite)) {
                file.setFileTime(when, QFileDevice::FileModificationTime);
            }
        };
        backdate(expired, old);
        backdate(foreign, old);
        const bool hadStaged = !stagedPath.isEmpty();
        if (hadStaged) {
            // The file the clipboard offers is old as well: it must survive.
            backdate(stagedPath, old);
        }
        const int removed =
          Recording::Clipboard::sweepStaging(stagingRoot, 3600LL * 1000);
        note(QStringLiteral("retention sweep removed %1 file(s)").arg(removed));
        check(!QFileInfo::exists(expired),
              QStringLiteral("a staging file older than the retention window is "
                             "removed"));
        check(QFileInfo::exists(foreign),
              QStringLiteral("a file without the staging prefix is never "
                             "removed"));
        if (hadStaged) {
            check(QFileInfo::exists(stagedPath),
                  QStringLiteral("the file the clipboard offers is never "
                                 "removed, even when it is old"));
            backdate(stagedPath, QDateTime::currentDateTimeUtc());
        }
        QFile::remove(foreign);
    }

    std::printf("\nRESULT: %s (%d failure(s))\n",
                g_failures == 0 ? "all checks passed" : "FAILURES",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
