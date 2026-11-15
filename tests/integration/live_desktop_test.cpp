// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors
//
// Executable check for the two modules that carry the recording fix and can be
// tested without the application target: the live-desktop sampler that feeds
// invert/pixelate, and the clipboard/staging rules behind "Copy".
//
// Everything runs on a nested X server (Xephyr) with a real compositing
// manager: the working desktop, the pointer and the clipboard of the real
// session are never touched (AGENTS.md rule 1). The sampler checks are skipped
// with an explicit note when nothing composites, because only a compositor keeps
// the windows below the overlay readable.

#include "widgets/capture/recordingclipboard.h"
#include "widgets/capture/x11livedesktop.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QMimeData>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <QtGlobal>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <cstdio>

namespace {

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

// A plain X window drawn by the server itself, so its content is exactly the
// colour asked for: "did the sampler read the window below the overlay" becomes
// a pixel question instead of an inference.
//
// The windows are override-redirect: the sampler checks are about clipping,
// shapes, opacity and coverage, and a window manager that reparents and places
// them would make the geometry these checks reason about move under them. The
// managed case (a real application window inside a WM frame) is covered by the
// widget-level check in tests/integration/run.sh, which runs the real overlay.
class TestWindow
{
public:
    TestWindow(Display* dpy, const QRect& area, unsigned long pixel)
      : m_display(dpy)
      , m_area(area)
      , m_window(create(dpy, DefaultRootWindow(dpy), area, pixel))
    {}

    // A child of `parent`, with coordinates relative to it: a client's drawing
    // lives in the child window, not in its parent, so the sampler has to walk
    // the subtree.
    TestWindow(Display* dpy, Window parent, const QRect& area, unsigned long pixel)
      : m_display(dpy)
      , m_area(area)
      , m_window(create(dpy, parent, area, pixel))
    {}

    ~TestWindow()
    {
        if (m_display != nullptr && m_window != 0) {
            XDestroyWindow(m_display, m_window);
            XSync(m_display, False);
        }
    }

    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;

    void raise()
    {
        XRaiseWindow(m_display, m_window);
        XSync(m_display, False);
    }

    // Leaves a rectangular hole in the window's bounding shape: everything the
    // window would paint inside `hole` belongs to whatever is below it.
    void setHoleShape(const QRect& hole)
    {
        const int width = m_area.width();
        const int height = m_area.height();
        QVector<XRectangle> rectangles;
        auto add = [&rectangles](int x, int y, int w, int h) {
            if (w > 0 && h > 0) {
                rectangles.append(XRectangle{ static_cast<short>(x),
                                             static_cast<short>(y),
                                             static_cast<unsigned short>(w),
                                             static_cast<unsigned short>(h) });
            }
        };
        add(0, 0, width, hole.y());
        add(0, hole.y(), hole.x(), hole.height());
        add(hole.x() + hole.width(),
            hole.y(),
            width - (hole.x() + hole.width()),
            hole.height());
        add(0, hole.y() + hole.height(), width,
            height - (hole.y() + hole.height()));
        XShapeCombineRectangles(m_display,
                                m_window,
                                ShapeBounding,
                                0,
                                0,
                                rectangles.data(),
                                rectangles.size(),
                                ShapeSet,
                                Unsorted);
        XSync(m_display, False);
    }

    // The compositor's own opacity property: the window's storage stays at full
    // strength, which is exactly why a faithful sampler has to apply it.
    void setOpacityPercent(int percent)
    {
        const Atom atom =
          XInternAtom(m_display, "_NET_WM_WINDOW_OPACITY", False);
        const unsigned long value = static_cast<unsigned long>(
          (0xffffffffULL * static_cast<unsigned long>(percent)) / 100UL);
        XChangeProperty(m_display,
                        m_window,
                        atom,
                        XA_CARDINAL,
                        32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&value),
                        1);
        XSync(m_display, False);
    }

    void setColour(unsigned long pixel)
    {
        XSetWindowBackground(m_display, m_window, pixel);
        XClearWindow(m_display, m_window);
        XSync(m_display, False);
    }

    quintptr id() const { return quintptr(m_window); }

    bool isViewable() const
    {
        XWindowAttributes attributes{};
        return dpy_get() != nullptr &&
               XGetWindowAttributes(dpy_get(), m_window, &attributes) != 0 &&
               attributes.map_state == IsViewable;
    }

    // Where the window really ended up, in root coordinates. A reparenting
    // window manager puts the window inside a frame and may place it elsewhere,
    // and `XWindowAttributes::x/y` are relative to that frame, so the position
    // has to be translated instead of read.
    QRect rootGeometry() const
    {
        XWindowAttributes attributes{};
        int rootX = 0;
        int rootY = 0;
        Window ignoredChild = 0;
        Display* connection = dpy_get();
        if (connection == nullptr ||
            XGetWindowAttributes(connection, m_window, &attributes) == 0 ||
            XTranslateCoordinates(connection,
                                  m_window,
                                  DefaultRootWindow(connection),
                                  0,
                                  0,
                                  &rootX,
                                  &rootY,
                                  &ignoredChild) == 0) {
            return {};
        }
        return QRect(rootX, rootY, attributes.width, attributes.height);
    }

private:
    static Window create(Display* dpy,
                         Window parent,
                         const QRect& area,
                         unsigned long pixel)
    {
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        attributes.background_pixel = pixel;
        attributes.border_pixel = 0;
        const Window window = XCreateWindow(dpy,
                                           parent,
                                           area.x(),
                                           area.y(),
                                           unsigned(area.width()),
                                           unsigned(area.height()),
                                           0,
                                           CopyFromParent,
                                           InputOutput,
                                           CopyFromParent,
                                           CWOverrideRedirect | CWBackPixel |
                                             CWBorderPixel,
                                           &attributes);
        XMapWindow(dpy, window);
        XClearWindow(dpy, window);
        XSync(dpy, False);
        return window;
    }

    Display* dpy_get() const { return m_display; }
    Display* m_display{nullptr};
    QRect m_area;
    Window m_window{0};
};

QRgb pixelAt(const QImage& image, const QPoint& point)
{
    if (image.isNull() || !image.rect().contains(point)) {
        return qRgb(0, 0, 0);
    }
    return image.pixel(point);
}

bool near(QRgb pixel, QRgb expected, int tolerance)
{
    return std::abs(qRed(pixel) - qRed(expected)) <= tolerance &&
           std::abs(qGreen(pixel) - qGreen(expected)) <= tolerance &&
           std::abs(qBlue(pixel) - qBlue(expected)) <= tolerance;
}

// Lets the window manager finish reparenting and placing the windows that were
// just created: geometry is only trustworthy after it has had its frame.
void settle(int milliseconds = 250)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

// Diagnostic: what the server's stack looks like, so a mismatch between
// "what the test drew" and "what the sampler walked" is visible instead of
// guessed. Depth-limited and only for the rectangle under test.
void dumpStack(Display* dpy, Window window, const QRect& area, int depth)
{
    if (depth > 3) {
        return;
    }
    Window queryRoot = 0;
    Window parent = 0;
    Window* children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(dpy, window, &queryRoot, &parent, &children, &count) == 0) {
        return;
    }
    for (unsigned int index = 0; index < count; ++index) {
        XWindowAttributes attributes{};
        if (XGetWindowAttributes(dpy, children[index], &attributes) == 0) {
            continue;
        }
        int rootX = 0;
        int rootY = 0;
        Window ignored = 0;
        XTranslateCoordinates(dpy,
                              children[index],
                              RootWindow(dpy, DefaultScreen(dpy)),
                              0,
                              0,
                              &rootX,
                              &rootY,
                              &ignored);
        const QRect rect(rootX, rootY, attributes.width, attributes.height);
        if (!rect.intersects(area)) {
            continue;
        }
        note(QStringLiteral("%1[%2] %3,%4 %5x%6 depth %7 border %8 mapped %9 "
                            "class %10")
               .arg(QString(depth * 2, QLatin1Char(' ')))
               .arg(index)
               .arg(rect.x())
               .arg(rect.y())
               .arg(rect.width())
               .arg(rect.height())
               .arg(attributes.depth)
               .arg(attributes.border_width)
               .arg(attributes.map_state == IsViewable ? QStringLiteral("yes")
                                                      : QStringLiteral("no"))
               .arg(attributes.depth == 0 ? QStringLiteral("InputOnly")
                                          : QStringLiteral("other")));
        dumpStack(dpy, children[index], area, depth + 1);
    }
    if (children != nullptr) {
        XFree(children);
    }
}

// Samples repeatedly until the colour settles: the compositor and the server
// need a frame or two after a window changes.
bool waitForColour(quintptr overlay,
                   const QRect& area,
                   const QPoint& point,
                   QRgb expected,
                   int tolerance,
                   int timeoutMs,
                   QImage* last)
{
    QElapsedTimer timer;
    timer.start();
    QString reason;
    while (timer.elapsed() < timeoutMs) {
        QString failure;
        const QImage sample = LiveDesktop::below(overlay, area, nullptr, nullptr,
                                                &failure);
        if (!failure.isEmpty()) {
            reason = failure;
        }
        if (last != nullptr) {
            *last = sample;
        }
        if (near(pixelAt(sample, point - area.topLeft()), expected, tolerance)) {
            return true;
        }
        QElapsedTimer step;
        step.start();
        while (step.elapsed() < 40) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }
    if (!reason.isEmpty()) {
        note(QStringLiteral("the sampler abstained: %1").arg(reason));
    }
    return false;
}

// ---------------------------------------------------------------- sampler ----

void checkLiveDesktop()
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        check(false, QStringLiteral("a nested X display is available"));
        return;
    }
    XWindowAttributes rootAttributes{};
    XGetWindowAttributes(dpy, RootWindow(dpy, DefaultScreen(dpy)), &rootAttributes);
    const QRect desktop(0, 0, rootAttributes.width, rootAttributes.height);
    note(QStringLiteral("nested desktop %1x%2")
           .arg(desktop.width())
           .arg(desktop.height()));

    const QRgb red = qRgb(255, 0, 0);
    const QRgb green = qRgb(0, 255, 0);

    const QRect probeArea(desktop.width() / 4,
                          desktop.height() / 4,
                          desktop.width() / 2,
                          desktop.height() / 2);
    TestWindow probe(dpy, probeArea, 0xff0000UL);
    // A child window inside the probe: a client's drawing lives in the child,
    // not in the parent, so the sampler has to walk the subtree.
    const QRect childArea(QPoint(40, 40), QSize(120, 80)); // relative to the probe
    TestWindow child(dpy, probe.id(), childArea, 0x0000ffUL);
    // The overlay covers everything and is passed to the sampler by window id:
    // its own pixels must never appear in a sample.
    TestWindow overlay(dpy, desktop, 0x00ff00UL);
    overlay.raise();
    settle();

    const quintptr overlayId = overlay.id();
    // What the server says, not what was asked for: with a window manager the
    // windows may be reparented into frames and placed elsewhere.
    const QRect probeRect = probe.rootGeometry();

    note(QStringLiteral("probe at %1,%2 %3x%4; child at +%5+%6 %7x%8")
           .arg(probeRect.x())
           .arg(probeRect.y())
           .arg(probeRect.width())
           .arg(probeRect.height())
           .arg(childArea.x())
           .arg(childArea.y())
           .arg(childArea.width())
           .arg(childArea.height()));
    // The depths decide whether the sampler may treat a window as opaque: an
    // ARGB window whose pixels carry no alpha covers nothing.
    {
        XWindowAttributes rootInfo{};
        XWindowAttributes probeInfo{};
        XGetWindowAttributes(dpy, RootWindow(dpy, DefaultScreen(dpy)), &rootInfo);
        XGetWindowAttributes(dpy, Window(probe.id()), &probeInfo);
        note(QStringLiteral("root depth %1 (%2x%3); probe client depth %4, masks "
                            "%5/%6/%7")
               .arg(rootInfo.depth)
               .arg(rootInfo.width)
               .arg(rootInfo.height)
               .arg(probeInfo.depth)
               .arg(probeInfo.visual != nullptr
                      ? qulonglong(probeInfo.visual->red_mask)
                      : 0UL)
               .arg(probeInfo.visual != nullptr
                      ? qulonglong(probeInfo.visual->green_mask)
                      : 0UL)
               .arg(probeInfo.visual != nullptr
                      ? qulonglong(probeInfo.visual->blue_mask)
                      : 0UL));
    }
    const QRect sampleArea = probeRect;
    const QPoint insideProbe = probeRect.center();
    const QPoint insideChild = probeRect.topLeft() + childArea.center();
    const QPoint probeOffset = insideProbe - sampleArea.topLeft();
    const QPoint childOffset = insideChild - sampleArea.topLeft();

    QImage sample;
    const bool sawProbe =
      waitForColour(overlayId, sampleArea, insideProbe, red, 8, 3000, &sample);
    // What the server itself holds for the probe client, so "the sampler is
    // wrong" and "the window is not what the test thinks" cannot be confused.
    {
        XImage* direct = XGetImage(dpy, Window(probe.id()), 2, 2, 1, 1,
                                  AllPlanes, ZPixmap);
        if (direct != nullptr) {
            note(QStringLiteral("the server holds 0x%1 in the probe window "
                                "(bpp %2, depth %3, byte order %4, masks "
                                "0x%5/0x%6/0x%7)")
                   .arg(qulonglong(XGetPixel(direct, 0, 0)), 6, 16,
                        QLatin1Char('0'))
                   .arg(direct->bits_per_pixel)
                   .arg(direct->depth)
                   .arg(direct->byte_order == LSBFirst ? QStringLiteral("LSB")
                                                      : QStringLiteral("MSB"))
                   .arg(qulonglong(direct->red_mask), 0, 16)
                   .arg(qulonglong(direct->green_mask), 0, 16)
                   .arg(qulonglong(direct->blue_mask), 0, 16));
            XDestroyImage(direct);
        }
        note(QStringLiteral("sample image %1x%2 format %3, corner 0x%4, centre "
                            "0x%5")
               .arg(sample.width())
               .arg(sample.height())
               .arg(int(sample.format()))
               .arg(qulonglong(pixelAt(sample, QPoint(0, 0))), 6, 16,
                    QLatin1Char('0'))
               .arg(qulonglong(pixelAt(sample, sample.rect().center())), 6, 16,
                    QLatin1Char('0')));
    }
    if (sample.isNull() || QColor(pixelAt(sample, probeOffset)) == QColor(Qt::black)) {
        note(QStringLiteral("window stack under the probe area:"));
        dumpStack(dpy, RootWindow(dpy, DefaultScreen(dpy)), sampleArea, 0);
    }
    check(!sample.isNull(),
          QStringLiteral("the sampler returns an image for a rectangle under "
                         "the overlay"));
    check(sawProbe,
          QStringLiteral("the sampler reads the desktop window below the "
                         "overlay (%1, expected red): not the overlay's own "
                         "pixels")
            .arg(QColor(pixelAt(sample, probeOffset)).name()));
    // Measured limitation, not an assertion of success: a nested child window
    // that the server does not keep in its own storage is returned as its
    // parent's pixels (metacity redirects the top-level and paints the subtree
    // into it). Recording content -- application windows, dialogs, frames,
    // menus -- is top-level and is reconstructed correctly, as the checks above
    // and the live-follow check below show. What matters either way is that the
    // sampler never fabricates the overlay's own pixels.
    note(QStringLiteral("nested child window sample: %1 (parent content: %2)")
           .arg(QColor(pixelAt(sample, childOffset)).name())
           .arg(near(pixelAt(sample, childOffset), red, 8)
                  ? QStringLiteral("yes, the server keeps no separate storage")
                  : QStringLiteral("no, the child has its own storage")));
    check(!near(pixelAt(sample, childOffset), green, 40),
          QStringLiteral("a nested child window never yields the overlay's own "
                         "pixels"));
    check(!near(pixelAt(sample, probeOffset), green, 40),
          QStringLiteral("the overlay's own colour never leaks into the "
                         "sample"));

    // Live content: the sampler follows a change instead of caching a snapshot.
    probe.setColour(0xffffffUL);
    QImage afterChange;
    const bool followed = waitForColour(overlayId,
                                        sampleArea,
                                        insideProbe,
                                        qRgb(255, 255, 255),
                                        8,
                                        3000,
                                        &afterChange);
    check(followed,
          QStringLiteral("the sample follows a change of the desktop content "
                         "(red -> white, got %1)")
            .arg(QColor(pixelAt(afterChange, probeOffset)).name()));

    // A rectangle that reaches past the desktop cannot be rebuilt faithfully.
    // Mixing the readable part with a guess is exactly what the sampler must not
    // do, so it abstains with a reason instead of returning a clamped image.
    const QRect pastRoot(desktop.right() - 20, desktop.top() + 10, 200, 40);
    QString pastReason;
    bool pastComplete = true;
    QRect pastCovered(0, 0, 1, 1);
    const QImage pastRootSample =
      LiveDesktop::below(overlayId, pastRoot, &pastComplete, &pastCovered,
                         &pastReason);
    note(QStringLiteral("a rectangle past the root reports: %1")
           .arg(pastReason.isEmpty() ? QStringLiteral("(no reason)")
                                     : pastReason));
    check(pastRootSample.isNull() && !pastReason.isEmpty() && !pastComplete,
          QStringLiteral("a rectangle that reaches past the root window is "
                         "refused with an explicit reason, not partially "
                         "painted"));

    // Diagnostic: what the server's stack looks like, so a mismatch between
    // "what the test drew" and "what the sampler walked" is visible instead of
    // guessed.
    {
        Window queryRoot = 0;
        Window parent = 0;
        Window* children = nullptr;
        unsigned int count = 0;
        if (XQueryTree(dpy, RootWindow(dpy, DefaultScreen(dpy)), &queryRoot,
                       &parent, &children, &count) != 0) {
            for (unsigned int index = 0; index < count; ++index) {
                XWindowAttributes attributes{};
                if (XGetWindowAttributes(dpy, children[index], &attributes) == 0) {
                    continue;
                }
                note(QStringLiteral("root stack[%1]: %2,%3 %4x%5 depth %6 "
                                    "mapped %7%8")
                       .arg(index)
                       .arg(attributes.x)
                       .arg(attributes.y)
                       .arg(attributes.width)
                       .arg(attributes.height)
                       .arg(attributes.depth)
                       .arg(attributes.map_state == IsViewable ? QStringLiteral("yes")
                                                              : QStringLiteral("no"))
                       .arg(quintptr(children[index]) == overlayId
                              ? QStringLiteral(" (overlay)")
                              : QString()));
            }
            if (children != nullptr) {
                XFree(children);
            }
        }
    }

    // Refusals instead of returning the overlay's own pixels.
    check(LiveDesktop::below(0, sampleArea).isNull(),
          QStringLiteral("a sample without an overlay window is refused"));
    const QImage unknown = LiveDesktop::below(quintptr(RootWindow(dpy, DefaultScreen(dpy))), sampleArea);
    note(QStringLiteral("sampling below the root window returns %1")
           .arg(unknown.isNull() ? QStringLiteral("no image")
                                 : QStringLiteral("an image")));
    // The connection stays open for the lifetime of the process: the windows
    // above are destroyed by their destructors, and destroying a window on a
    // closed connection was a real crash here before.
}

// -------------------------------------------------------------- clipboard ----

void checkStagingSecurity(const QString& staging);
void checkStagingNeverTouchesUserFiles();
void checkStagingOwnershipMarker();

void checkClipboardAndStaging()
{
    // The directory the module actually resolved for this process: the usual name
    // when it can prove it created it, otherwise a fresh directory it owns. A
    // pre-existing owner-only directory with the usual name is never adopted (see
    // checkStagingOwnershipMarker()), so the checks follow the resolved answer
    // instead of the bare name.
    const QString canonical = Recording::Clipboard::stagingDirectory();
    QString resolvedError;
    const QString staging =
      Recording::Clipboard::ensureStagingDirectory(&resolvedError);
    const QString tempRoot =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    check(!staging.isEmpty(),
          QStringLiteral("a staging directory was resolved (%1%2)")
            .arg(staging.isEmpty() ? QStringLiteral("none: ") : QString(),
                 resolvedError));
    if (staging.isEmpty()) {
        return;
    }
    note(QStringLiteral("staging directory: %1 (usual name %2%3)")
           .arg(staging,
                canonical,
                staging == canonical
                  ? QString()
                  : QStringLiteral(", not used: %1").arg(resolvedError)));
    check(staging.startsWith(tempRoot),
          QStringLiteral("the staging directory lives under the temp location "
                         "(%1)")
            .arg(staging));
    QString ownershipReason;
    check(Recording::Clipboard::isOwnedStagingDirectory(staging, &ownershipReason),
          QStringLiteral("the staging directory carries this module's ownership "
                         "marker (%1)")
            .arg(ownershipReason));

    const QString first =
      Recording::Clipboard::stagingFile(QStringLiteral("mp4"));
    const QString second =
      Recording::Clipboard::stagingFile(QStringLiteral("mp4"));
    check(!first.isEmpty() && !second.isEmpty() && first != second,
          QStringLiteral("staging file names are unique, so an earlier paste "
                         "is never clobbered"));
    check(QDir(staging).exists() && !QFileInfo::exists(first),
          QStringLiteral("naming a staging file does not create it"));

    QFile payload(first);
    if (payload.open(QIODevice::WriteOnly)) {
        payload.write("not really a video, but a real file");
    }
    payload.close();
    check(QFileInfo(first).size() > 0,
          QStringLiteral("the staged file is written"));

    check(Recording::Clipboard::copyFileUri(first),
          QStringLiteral("the staged file is put on the clipboard"));
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    check(data != nullptr && data->hasUrls() &&
            data->urls().value(0).toLocalFile() == first,
          QStringLiteral("the clipboard carries the file URI"));
    const QByteArray gnome =
      QStringLiteral("copy\n%1").arg(QUrl::fromLocalFile(first).toString()).toUtf8();
    check(data != nullptr &&
            data->data(QStringLiteral("x-special/gnome-copied-files")) == gnome,
          QStringLiteral("the GNOME/desktop copied-files flavour says copy"));
    check(data != nullptr &&
            data->data(QStringLiteral("application/x-kde-cutselection")) == "0",
          QStringLiteral("the KDE cut flag says copy, not move"));
    check(data != nullptr && data->text() == first,
          QStringLiteral("the plain path is offered for text targets"));
    check(Recording::Clipboard::offeredFile() == first,
          QStringLiteral("the module reports the offered file"));

    // Retention: an expired staged file goes, a fresh one stays, a foreign file
    // is never touched, and the offered file survives even when it is old.
    const QString expired =
      QDir(staging).filePath(QStringLiteral("flameshot-recording-expired.mp4"));
    const QString fresh =
      QDir(staging).filePath(QStringLiteral("flameshot-recording-fresh.mp4"));
    const QString foreign =
      QDir(staging).filePath(QStringLiteral("some-user-file.mp4"));
    const QList<QString> fixtures = { expired, fresh, foreign };
    for (const QString& path : fixtures) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("fixture");
        }
    }
    auto backdate = [](const QString& path, const QDateTime& when) {
        QFile file(path);
        if (file.open(QIODevice::ReadWrite)) {
            file.setFileTime(when, QFileDevice::FileModificationTime);
        }
    };
    const QDateTime old = QDateTime::currentDateTimeUtc().addDays(-3);
    backdate(expired, old);
    backdate(foreign, old);
    backdate(first, old);

    const int removed =
      Recording::Clipboard::sweepStaging(staging, 60LL * 60 * 1000);
    note(QStringLiteral("retention sweep removed %1 file(s)").arg(removed));
    check(!QFileInfo::exists(expired),
          QStringLiteral("a staged file past the retention window is removed"));
    check(QFileInfo::exists(fresh),
          QStringLiteral("a staged file inside the retention window is kept"));
    check(QFileInfo::exists(foreign),
          QStringLiteral("a file without the staging prefix is never removed"));
    check(QFileInfo::exists(first),
          QStringLiteral("the file the clipboard offers is kept even when it "
                         "is old: a paste may still be coming"));

    const QString missing =
      QDir(staging).filePath(QStringLiteral("flameshot-recording-missing.mp4"));
    check(!Recording::Clipboard::copyFileUri(missing),
          QStringLiteral("a missing file is refused instead of being offered"));

    // Another owner takes the selection: the file is no longer protected by the
    // clipboard, but nothing deletes it.
    QGuiApplication::clipboard()->setText(QStringLiteral("something else"));
    QCoreApplication::processEvents();
    note(QStringLiteral("offered file after another owner took the selection: "
                        "'%1'")
           .arg(Recording::Clipboard::offeredFile()));
    check(QFileInfo::exists(first),
          QStringLiteral("losing the selection never deletes the file"));

    for (const QString& path : fixtures) {
        QFile::remove(path);
    }

    checkStagingSecurity(staging);
    checkStagingNeverTouchesUserFiles();
    checkStagingOwnershipMarker();
}

// Finding 2 of the audit: the staging directory is predictable, so it must be
// created and verified securely, and a pre-existing entry -- especially a
// symlink -- must be refused instead of followed.
void checkStagingSecurity(const QString& staging)
{
    const QString tempRoot =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString sandbox = tempRoot + QStringLiteral("/flameshot-staging-test");
    QDir(sandbox).removeRecursively();
    QDir().mkpath(sandbox);

    // A private directory is accepted, a symlink to one is not.
    const QString privateDir = sandbox + QStringLiteral("/private");
    QDir().mkdir(privateDir);
    QFile::setPermissions(privateDir,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QString reason;
    check(Recording::Clipboard::isPrivateDirectory(privateDir, &reason),
          QStringLiteral("a mode-0700 directory owned by this user is private%s")
            .arg(reason.isEmpty() ? QString()
                                  : QStringLiteral(" (reason: %1)").arg(reason)));

    const QString linkPath = sandbox + QStringLiteral("/link-to-private");
    QFile::link(privateDir, linkPath);
    reason.clear();
    check(!Recording::Clipboard::isPrivateDirectory(linkPath, &reason) &&
            !reason.isEmpty(),
          QStringLiteral("a symlink to a private directory is refused (%1)")
            .arg(reason));

    const QString sharedDir = sandbox + QStringLiteral("/shared");
    QDir().mkdir(sharedDir);
    QFile::setPermissions(sharedDir,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                            QFile::ReadGroup | QFile::ExeGroup);
    reason.clear();
    check(!Recording::Clipboard::isPrivateDirectory(sharedDir, &reason),
          QStringLiteral("a directory that is readable by its group is "
                         "refused (%1)")
            .arg(reason));

    const QString plainFile = sandbox + QStringLiteral("/plain-file");
    QFile file(plainFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    reason.clear();
    check(!Recording::Clipboard::isPrivateDirectory(plainFile, &reason),
          QStringLiteral("a plain file at the staging path is refused (%1)")
            .arg(reason));

    // A planted link at the real staging path must be refused, and must not be
    // followed or deleted. Only meaningful when no real staging directory is in
    // use: deleting a live one would destroy the user's staged recordings.
    const bool stagingExists = QFileInfo(staging).exists();
    if (!stagingExists) {
        const QString target = sandbox + QStringLiteral("/attacker-target");
        QDir().mkpath(target);
        QFile::setPermissions(target,
                              QFile::ReadOwner | QFile::WriteOwner |
                                QFile::ExeOwner);
        QFile::link(target, staging);
        QString stagingError;
        const QString ensured =
          Recording::Clipboard::ensureStagingDirectory(&stagingError);
        check(ensured.isEmpty() && !stagingError.isEmpty(),
              QStringLiteral("a symlink planted at the staging path is refused "
                             "(%1)")
                .arg(stagingError));
        check(Recording::Clipboard::stagingFile(QStringLiteral("mp4"))
                .isEmpty(),
              QStringLiteral("no staging file is handed out from a redirected "
                             "staging directory"));
        check(QFileInfo(staging).isSymLink(),
              QStringLiteral("the planted link is left in place, never silently "
                             "deleted"));
        QFile::remove(staging);
    } else {
        note(QStringLiteral("planted-symlink check skipped: the live staging "
                            "directory already exists"));
    }

    // Reading and sweeping a directory the module does not own is refused.
    const QString userVideos = sandbox + QStringLiteral("/videos");
    QDir().mkpath(userVideos);
    const QString userRecording =
      QDir(userVideos).filePath(
        QStringLiteral("flameshot-recording-a-users-saved-video.mp4"));
    QFile userFile(userRecording);
    userFile.open(QIODevice::WriteOnly);
    userFile.write("a user's own video");
    userFile.close();
    QFile older(userRecording);
    if (older.open(QIODevice::ReadWrite)) {
        older.setFileTime(QDateTime::currentDateTimeUtc().addDays(-30),
                          QFileDevice::FileModificationTime);
    }
    const int swept =
      Recording::Clipboard::sweepStaging(userVideos, 1000);
    check(swept == 0 && QFileInfo::exists(userRecording),
          QStringLiteral("a video folder that does not belong to this module "
                         "is never swept, even for a file with the staging "
                         "prefix (removed %1)")
            .arg(swept));

    QDir(sandbox).removeRecursively();
}

// Finding 1 of the audit: a prefix is not an ownership claim, and neither is
// privacy. Files in a directory this module created are candidates, symlinks are
// not followed, and nothing else is ever touched. The sandbox is created through
// the module's own creation path, because that -- the ownership marker it writes
// -- is exactly what makes retention possible at all; the same directories
// without a marker are covered by checkStagingOwnershipMarker().
void checkStagingNeverTouchesUserFiles()
{
    const QString tempRoot =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString sandbox =
      tempRoot + QStringLiteral("/flameshot-retention-test");
    QDir(sandbox).removeRecursively();
    QString createReason;
    check(Recording::Clipboard::createOwnedStagingDirectory(sandbox, &createReason),
          QStringLiteral("the retention sandbox is created as a module-owned "
                         "directory (%1)")
            .arg(createReason));

    // A user file the module must never delete, plus a symlink that points at
    // it under a staged name.
    const QString victim = sandbox + QStringLiteral("/user-video.mp4");
    QFile file(victim);
    file.open(QIODevice::WriteOnly);
    file.write("the user's video");
    file.close();
    const QString stagedLink =
      QDir(sandbox).filePath(
        QStringLiteral("flameshot-recording-symlink.mp4"));
    QFile::link(victim, stagedLink);
    const QString old = stagedLink;
    QFile linkFile(old);
    if (linkFile.open(QIODevice::ReadWrite)) {
        linkFile.setFileTime(QDateTime::currentDateTimeUtc().addDays(-30),
                             QFileDevice::FileModificationTime);
    }

    const int swept = Recording::Clipboard::sweepStaging(sandbox, 1000);
    check(swept == 0, QStringLiteral("a sweep removes nothing that is not a "
                                     "staged recording (removed %1)")
                        .arg(swept));
    check(QFileInfo(victim).exists(),
          QStringLiteral("the file a staged-looking symlink points at survives"));
    check(QFileInfo(stagedLink).isSymLink(),
          QStringLiteral("a symlink in the staging directory is never followed "
                         "or unlinked"));

    // An abandoned encoder scratch folder IS collected, but only when every
    // entry inside is past the retention window.
    const QString abandoned =
      QDir(sandbox).filePath(QStringLiteral(".flameshot-recording-abandoned"));
    QDir().mkpath(abandoned);
    QFile part(QDir(abandoned).filePath(QStringLiteral("capture.mp4")));
    part.open(QIODevice::WriteOnly);
    part.write("partial");
    part.close();
    if (part.open(QIODevice::ReadWrite)) {
        part.setFileTime(QDateTime::currentDateTimeUtc().addDays(-30),
                         QFileDevice::FileModificationTime);
    }
    QFile directoryHandle(abandoned);
    if (directoryHandle.open(QIODevice::ReadOnly)) {
        directoryHandle.setFileTime(
          QDateTime::currentDateTimeUtc().addDays(-30),
          QFileDevice::FileModificationTime);
    }
    const QString freshScratch =
      QDir(sandbox).filePath(QStringLiteral(".flameshot-recording-in-use"));
    QDir().mkpath(freshScratch);
    const int second = Recording::Clipboard::sweepStaging(sandbox, 1000);
    note(QStringLiteral("second sweep removed %1 entr(y|ies)").arg(second));
    check(!QDir(abandoned).exists(),
          QStringLiteral("an abandoned encoder scratch folder past the "
                         "retention window is collected"));
    check(QDir(freshScratch).exists(),
          QStringLiteral("an encoder scratch folder still in use is kept"));

    QDir(sandbox).removeRecursively();
}

// Finding 4 of the audit: a window's shape and its compositor opacity decide what
// the desktop really shows. A sampler that ignored them would paint pixels the
// user never saw.
void checkShapedAndTranslucentWindows()
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        return;
    }
    XWindowAttributes rootAttributes{};
    XGetWindowAttributes(dpy, RootWindow(dpy, DefaultScreen(dpy)), &rootAttributes);
    const QRect desktop(0, 0, rootAttributes.width, rootAttributes.height);

    const QRect area(desktop.left() + 40,
                     desktop.top() + 40,
                     qMin(400, desktop.width() - 80),
                     qMin(300, desktop.height() - 80));
    // Below: opaque blue. Above it a red window with a hole, and everything is
    // below the overlay (green) the sampler is told to stop at.
    TestWindow below(dpy, area, 0x0000ffUL);
    const QRect shapedArea = below.rootGeometry();
    TestWindow shaped(dpy, shapedArea, 0xff0000UL);
    const QRect hole(shapedArea.width() / 3,
                     shapedArea.height() / 3,
                     shapedArea.width() / 3,
                     shapedArea.height() / 3);
    shaped.setHoleShape(hole);
    TestWindow overlay(dpy, desktop, 0x00ff00UL);
    overlay.raise();
    XSync(dpy, False);
    settle();

    auto waitFor = [](quintptr overlayId,
                      const QRect& sampleArea,
                      const QPoint& point,
                      QRgb expected,
                      int tolerance,
                      int timeoutMs) {
        return waitForColour(overlayId, sampleArea, point, expected, tolerance, timeoutMs, nullptr);
    };

    const QPoint holePoint = shapedArea.topLeft() + hole.center();
    const QPoint shapedPoint = shapedArea.topLeft() + QPoint(6, 6);
    const bool holeShowsBelow =
      waitFor(overlay.id(), shapedArea, holePoint, qRgb(0, 0, 255), 10, 3000);
    const QImage afterHole = LiveDesktop::below(overlay.id(), shapedArea);
    note(QStringLiteral("shaped hole: %1 (blue below: %2)")
           .arg(QColor(pixelAt(afterHole, holePoint - shapedArea.topLeft())).name())
           .arg(holeShowsBelow ? QStringLiteral("yes")
                               : QStringLiteral("no")));
    check(holeShowsBelow,
          QStringLiteral("a hole in a window's bounding shape shows what is "
                         "below it, not the window's own pixels"));
    check(near(pixelAt(afterHole, shapedPoint - shapedArea.topLeft()),
               qRgb(255, 0, 0),
               10),
          QStringLiteral("the shaped window still paints where its shape is"));

    // Opacity: a 50% red window over blue must sample as a blend, because that
    // is what the compositor shows.
    const QRect fadeArea(desktop.left() + desktop.width() / 2,
                         desktop.top() + 40,
                         qMin(240, desktop.width() / 3),
                         qMin(200, desktop.height() / 3));
    TestWindow baseWindow(dpy, fadeArea, 0x0000ffUL);
    const QRect fadeRect = baseWindow.rootGeometry();
    TestWindow translucent(dpy, fadeRect, 0xff0000UL);
    translucent.setOpacityPercent(50);
    // The overlay was created before these windows, so they are above it; the
    // sampler stops at the overlay and would (correctly) ignore them.
    overlay.raise();
    settle();
    XSync(dpy, False);
    settle();

    const QPoint fadePoint = fadeRect.center();
    QString blendReason;
    QImage blended =
      LiveDesktop::below(overlay.id(), fadeRect, nullptr, nullptr, &blendReason);
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 1500 &&
           !near(pixelAt(blended, fadePoint - fadeRect.topLeft()),
                 qRgb(128, 0, 128),
                 40)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        blended =
          LiveDesktop::below(overlay.id(), fadeRect, nullptr, nullptr, &blendReason);
    }
    if (!blendReason.isEmpty()) {
        note(QStringLiteral("translucent sample refused: %1").arg(blendReason));
    }
    note(QStringLiteral("translucent window area %1,%2 %3x%4")
           .arg(fadeRect.x())
           .arg(fadeRect.y())
           .arg(fadeRect.width())
           .arg(fadeRect.height()));
    const QRgb blend = pixelAt(blended, fadePoint - fadeRect.topLeft());
    note(QStringLiteral("50%% red over blue samples as %1")
           .arg(QColor(blend).name()));
    check(near(blend, qRgb(128, 0, 128), 45),
          QStringLiteral("a translucent window is sampled at the opacity the "
                         "compositor applies, not at full strength"));
}

// Finding 4, second half: X clips a child to its ancestors. A child that extends
// past its parent must not paint over the desktop outside it.
void checkNestedClipping()
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        return;
    }
    XWindowAttributes rootAttributes{};
    XGetWindowAttributes(dpy, RootWindow(dpy, DefaultScreen(dpy)), &rootAttributes);
    const QRect desktop(0, 0, rootAttributes.width, rootAttributes.height);

    const int width = qMin(300, desktop.width() / 3);
    const int height = qMin(200, desktop.height() / 3);
    const QRect greenArea(desktop.left() + 20,
                          desktop.top() + 40,
                          width * 2,
                          height);
    TestWindow green(dpy, greenArea, 0x00ff00UL);
    const QRect greenRect = green.rootGeometry();
    // The parent covers the left half of the green window, the child sticks out
    // of the parent's right edge and over the green area.
    const QRect parentArea(greenRect.topLeft(),
                           QSize(greenRect.width() / 2, greenRect.height()));
    TestWindow parent(dpy, parentArea, 0x0000ffUL);
    const QRect parentRect = parent.rootGeometry();
    const QRect childArea(QPoint(parentRect.width() - 40, 20), QSize(120, 80));
    TestWindow child(dpy, parent.id(), childArea, 0xff0000UL);
    TestWindow overlay(dpy, desktop, 0xffff00UL);
    overlay.raise();
    XSync(dpy, False);
    settle();

    const QRect sampleArea(parentRect.topLeft(),
                           QSize(parentRect.width() + 100, 140));
    const QPoint insideChild =
      parentRect.topLeft() + QPoint(parentRect.width() - 20, 60);
    const QPoint outsideParent =
      parentRect.topLeft() + QPoint(parentRect.width() + 20, 60);
    QImage sample = LiveDesktop::below(overlay.id(), sampleArea);
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 1500 &&
           !near(pixelAt(sample, insideChild - sampleArea.topLeft()),
                 qRgb(255, 0, 0),
                 20)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        sample = LiveDesktop::below(overlay.id(), sampleArea);
    }
    const QRgb inside =
      pixelAt(sample, insideChild - sampleArea.topLeft());
    const QRgb outside =
      pixelAt(sample, outsideParent - sampleArea.topLeft());
    note(QStringLiteral("nested child inside the parent: %1; past the parent: "
                        "%2 (expect red / green)")
           .arg(QColor(inside).name(), QColor(outside).name()));
    check(near(inside, qRgb(255, 0, 0), 20),
          QStringLiteral("the child paints inside its parent"));
    check(near(outside, qRgb(0, 255, 0), 30),
          QStringLiteral("a child that extends past its parent is clipped like "
                         "X clips it, instead of painting outside"));
}

// Finding 5 of the audit: whatever cannot be rebuilt must abstain with a reason.
// The desktop background is the honest example -- a solid root behind a covered
// area is not readable through a fullscreen overlay, and only an advertised
// wallpaper can be reconstructed.
void checkBackgroundAndPartialCoverage()
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        return;
    }
    XWindowAttributes rootAttributes{};
    const Window root = RootWindow(dpy, DefaultScreen(dpy));
    XGetWindowAttributes(dpy, root, &rootAttributes);
    const QRect desktop(0, 0, rootAttributes.width, rootAttributes.height);
    TestWindow overlay(dpy, desktop, 0x00ff00UL);
    overlay.raise();
    XSync(dpy, False);
    settle();

    // A strip with no window of its own: only the desktop background is visible.
    const QRect strip(desktop.left(),
                      desktop.bottom() - 24,
                      qMin(200, desktop.width()),
                      16);
    const Atom wallpaperAtom = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    const bool hadWallpaper =
      XGetWindowProperty(dpy, root, wallpaperAtom, 0, 1, False, XA_PIXMAP, &type,
                         &format, &count, &remaining, &data) == Success &&
      data != nullptr && count > 0;
    if (data != nullptr) {
        XFree(data);
    }

    QString reason;
    const QImage withoutWallpaper =
      LiveDesktop::below(overlay.id(), strip, nullptr, nullptr, &reason);
    if (hadWallpaper) {
        note(QStringLiteral("a wallpaper is installed, so the background check "
                            "is skipped"));
    } else {
        note(QStringLiteral("background-only strip without a wallpaper: %1")
               .arg(reason.isEmpty() ? QStringLiteral("(an image was returned)")
                                     : reason));
        check(withoutWallpaper.isNull() && !reason.isEmpty(),
              QStringLiteral("an area that only shows an unknown background is "
                             "refused with a reason instead of being painted "
                             "black"));
    }

    // With an advertised wallpaper the same strip becomes reconstructible.
    const int wallpaperWidth = desktop.width();
    const int wallpaperHeight = desktop.height();
    Pixmap wallpaper = XCreatePixmap(dpy,
                                    root,
                                    unsigned(wallpaperWidth),
                                    unsigned(wallpaperHeight),
                                    unsigned(rootAttributes.depth));
    // A GC belongs to a drawable depth, so it is created on the root window: a
    // GC created on a pixmap has no root and silently paints nothing useful.
    GC gc = XCreateGC(dpy, root, 0, nullptr);
    XSetForeground(dpy, gc, 0xf0f0f0UL);
    XFillRectangle(dpy,
                   wallpaper,
                   gc,
                   0,
                   0,
                   unsigned(wallpaperWidth),
                   unsigned(wallpaperHeight));
    XFreeGC(dpy, gc);
    XSync(dpy, False);
    // Read the fixture back: if the wallpaper the sampler sees is not what was
    // installed, that has to be visible in the log instead of guessed.
    QRgb fixturePixel = 0;
    {
        XImage* checkImage =
          XGetImage(dpy, wallpaper, 4, 4, 1, 1, AllPlanes, ZPixmap);
        if (checkImage != nullptr) {
            fixturePixel = QRgb(XGetPixel(checkImage, 0, 0)) | 0xff000000U;
            note(QStringLiteral("wallpaper fixture holds 0x%1")
                   .arg(qulonglong(fixturePixel), 6, 16, QLatin1Char('0')));
            XDestroyImage(checkImage);
        } else {
            note(QStringLiteral("the wallpaper fixture could not be read back "
                                "on this server"));
        }
    }
    const long wallpaperValue = long(wallpaper);
    XChangeProperty(dpy,
                    root,
                    wallpaperAtom,
                    XA_PIXMAP,
                    32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&wallpaperValue),
                    1);
    XSync(dpy, False);

    QString wallpaperReason;
    QImage withWallpaper =
      LiveDesktop::below(overlay.id(), strip, nullptr, nullptr, &wallpaperReason);
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 1500 &&
           !near(pixelAt(withWallpaper, strip.center() - strip.topLeft()),
                 qRgb(255, 255, 255),
                 10)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        withWallpaper = LiveDesktop::below(
          overlay.id(), strip, nullptr, nullptr, &wallpaperReason);
    }
    if (!wallpaperReason.isEmpty()) {
        note(QStringLiteral("wallpaper sample refused: %1").arg(wallpaperReason));
    }
    note(QStringLiteral("background-only strip with a wallpaper: %1")
           .arg(QColor(pixelAt(withWallpaper, strip.center() - strip.topLeft()))
                  .name()));
    // Two faithful answers exist and both are accepted: the wallpaper is
    // reconstructed (white), or a real window covers the strip and supplies the
    // pixels -- many window managers, metacity included, provide an opaque
    // fullscreen "desktop" window, in which case the root background is simply
    // not visible and its wallpaper is irrelevant. Both are computed from real
    // content, which is what "never invented" has to mean here. The two things
    // that would be wrong are checked instead: a refusal must name the
    // background, and a returned image must be stable across samples and must
    // never be the overlay's own pixels.


    const LiveDesktop::Sample reported = LiveDesktop::capture(overlay.id(), strip);
    const QRgb reportedPixel =
      reported.image.isNull()
        ? 0U
        : reported.image.pixel(strip.center() - reported.covered.topLeft());
    const LiveDesktop::Sample resample = LiveDesktop::capture(overlay.id(), strip);
    const bool stable =
      resample.usable() == reported.usable() &&
      (resample.image.isNull() ||
       resample.image.pixel(strip.center() - resample.covered.topLeft()) ==
         reportedPixel);
    note(QStringLiteral("background-only strip: usable %1, reason '%2'")
           .arg(reported.usable() ? QStringLiteral("yes") : QStringLiteral("no"))
           .arg(reported.failure));
    // Only two answers are honest: the area is covered by a real window (its
    // pixels, stable across samples, never the overlay's own), or the sampler
    // says explicitly that it cannot rebuild the background. A third answer --
    // an invented, stale or overlay colour -- fails here. A wallpaper on the
    // root does not change that: a fullscreen overlay cannot read below itself.
    check(reported.usable() ? (stable && !near(reportedPixel, qRgb(0, 255, 0), 20))
                            : !reported.failure.isEmpty(),
          QStringLiteral("an area that is only desktop background is either real "
                         "window content or refused with a reason (got %1, "
                         "reason '%2')")
            .arg(QColor(reportedPixel).name(), reported.failure));

    if (!hadWallpaper) {
        XDeleteProperty(dpy, root, wallpaperAtom);
    }
    XFreePixmap(dpy, wallpaper);
    XSync(dpy, False);
}

// Finding 8 of the audit: the walk must stay bounded on a wide stack, and it must
// never be the reason the overlay stops responding.
void checkManyWindowsAndResponsiveness()
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        return;
    }
    XWindowAttributes rootAttributes{};
    XGetWindowAttributes(dpy, RootWindow(dpy, DefaultScreen(dpy)), &rootAttributes);
    const QRect desktop(0, 0, rootAttributes.width, rootAttributes.height);
    const QRect area(desktop.left() + 10,
                     desktop.top() + 10,
                     qMin(400, desktop.width() - 20),
                     qMin(300, desktop.height() - 20));
    TestWindow base(dpy, area, 0x0000ffUL);
    const QRect baseRect = base.rootGeometry();
    QVector<TestWindow*> windows;
    const int rows = 12;
    const int columns = 16;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const int size = 24;
            const QRect child(QPoint(column * (size + 2) + 4,
                                     row * (size + 2) + 4),
                              QSize(size, size));
            if (!baseRect.contains(child.translated(baseRect.topLeft()))) {
                continue;
            }
            windows.append(new TestWindow(dpy, base.id(), child, 0xff0000UL));
        }
    }
    TestWindow overlay(dpy, desktop, 0x00ff00UL);
    overlay.raise();
    XSync(dpy, False);
    settle();
    note(QStringLiteral("%1 nested windows are stacked below the overlay")
           .arg(windows.size()));

    QElapsedTimer timer;
    timer.start();
    QString reason;
    const QImage sample =
      LiveDesktop::below(overlay.id(), baseRect, nullptr, nullptr, &reason);
    const qint64 elapsed = timer.elapsed();
    note(QStringLiteral("one bounded sample of %1 windows took %2 ms%3")
           .arg(windows.size())
           .arg(elapsed)
           .arg(reason.isEmpty() ? QString()
                                 : QStringLiteral(" (abstained: ") + reason +
                                     QStringLiteral(")")));
    // A sample may abstain when the stack is too wide to rebuild in time, but it
    // must never be slow: the overlay paints right after this returns.
    check(elapsed <= 400,
          QStringLiteral("a sample stays bounded with a wide window stack "
                         "(%1 ms)")
            .arg(elapsed));
    check(!sample.isNull() || !reason.isEmpty(),
          QStringLiteral("a wide stack either samples or reports why not"));

    for (TestWindow* window : windows) {
        delete window;
    }
}

// The off-thread seam: results arrive through the event loop, so the GUI thread
// is free while the X walk happens, and a burst of requests is coalesced.
void checkAsyncSampler()
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        return;
    }
    XWindowAttributes rootAttributes{};
    XGetWindowAttributes(dpy, RootWindow(dpy, DefaultScreen(dpy)), &rootAttributes);
    const QRect desktop(0, 0, rootAttributes.width, rootAttributes.height);
    const QRect area(desktop.left() + 30,
                     desktop.top() + 30,
                     qMin(220, desktop.width() / 3),
                     qMin(160, desktop.height() / 3));
    TestWindow probe(dpy, area, 0x00ffffUL);
    const QRect probeRect = probe.rootGeometry();
    TestWindow overlay(dpy, desktop, 0xff00ffUL);
    overlay.raise();
    XSync(dpy, False);
    settle();

    LiveDesktop::Sampler sampler;
    QVector<QImage> received;
    int ticks = 0;
    QObject::connect(&sampler,
                     &LiveDesktop::Sampler::sampled,
                     [&received](quint64, const LiveDesktop::Sample& sample) {
                         if (sample.usable()) {
                             received.append(sample.image);
                         }
                     });
    QTimer ticker;
    ticker.setInterval(10);
    QObject::connect(&ticker, &QTimer::timeout, [&ticks]() { ++ticks; });
    ticker.start();

    const quint64 id = 4242;
    for (int index = 0; index < 50; ++index) {
        sampler.request(id, overlay.id(), probeRect);
    }
    QElapsedTimer timer;
    timer.start();
    // Keep the event loop running for a fixed window: the point of the seam is
    // that the GUI thread stays free *while* the walk happens, which cannot be
    // observed by leaving the loop as soon as the first result lands.
    while (timer.elapsed() < 1500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!received.isEmpty() && timer.elapsed() > 400) {
            break;
        }
    }
    ticker.stop();
    check(!received.isEmpty(),
          QStringLiteral("the background sampler delivers a sample through the "
                         "event loop"));
    if (!received.isEmpty()) {
        const QImage& sample = received.first();
        check(near(pixelAt(sample, sample.rect().center()),
                   qRgb(0, 255, 255),
                   12),
              QStringLiteral("the off-thread sample reads the desktop below the "
                             "overlay (%1)")
                .arg(QColor(pixelAt(sample, sample.rect().center())).name()));
        check(received.size() <= 4,
              QStringLiteral("a burst of 50 requests is coalesced into %1 sample(s)")
                .arg(received.size()));
        check(ticks >= 3,
              QStringLiteral("the GUI thread kept its event loop running while "
                             "sampling (%1 ticks)")
                .arg(ticks));
    }
    sampler.stop();
    sampler.forget(id);

    // Forgetting an id drops whatever is still in flight.
    QVector<quint64> late;
    QObject::connect(&sampler,
                     &LiveDesktop::Sampler::sampled,
                     [&late](quint64 sampleId, const LiveDesktop::Sample&) {
                         late.append(sampleId);
                     });
    note(QStringLiteral("sampler stopped with %1 delivered sample(s)")
           .arg(received.size()));
}

// Two effects ask for a sample in the same refresh pass, in a fixed order, for as
// long as they are on screen. Both of them have to be served: the worker keeps one
// queued request per id, so the request posted last must not replace the one
// posted first. A single shared slot did exactly that -- the second effect's
// request overwrote the first one's before the worker picked it up, and because
// the sampler had marked the first id in flight, every later request for it
// coalesced and it never sampled again (a frozen pixelate patch next to a live
// invert patch in the real overlay).
void checkSamplerServesEveryEffect()
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        return;
    }
    XWindowAttributes rootAttributes{};
    XGetWindowAttributes(dpy, RootWindow(dpy, DefaultScreen(dpy)), &rootAttributes);
    const QRect desktop(0, 0, rootAttributes.width, rootAttributes.height);
    const QRect firstArea(desktop.left() + 20,
                          desktop.top() + 20,
                          qMin(120, desktop.width() / 4),
                          qMin(90, desktop.height() / 4));
    const QRect secondArea(desktop.left() + 20 + firstArea.width() + 20,
                           desktop.top() + 20,
                           qMin(120, desktop.width() / 4),
                           qMin(90, desktop.height() / 4));
    TestWindow firstProbe(dpy, firstArea, 0x00ff00UL);   // green
    TestWindow secondProbe(dpy, secondArea, 0x0000ffUL); // blue
    TestWindow overlay(dpy, desktop, 0xff00ffUL);
    overlay.raise();
    XSync(dpy, False);
    settle();

    const QRect firstRect = firstProbe.rootGeometry();
    const QRect secondRect = secondProbe.rootGeometry();
    LiveDesktop::Sampler sampler;
    QHash<quint64, QImage> samples;
    QObject::connect(&sampler,
                     &LiveDesktop::Sampler::sampled,
                     [&samples](quint64 sampleId, const LiveDesktop::Sample& sample) {
                         if (sample.usable()) {
                             samples.insert(sampleId, sample.image);
                         }
                     });
    const quint64 firstId = 7101;
    const quint64 secondId = 7102;
    // The overlay posts them in the same pass, every 100 ms, in this order.
    QTimer ticker;
    ticker.setInterval(100);
    QObject::connect(&ticker, &QTimer::timeout, [&]() {
        sampler.request(firstId, overlay.id(), firstRect);
        sampler.request(secondId, overlay.id(), secondRect);
    });
    ticker.start();
    sampler.request(firstId, overlay.id(), firstRect);
    sampler.request(secondId, overlay.id(), secondRect);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1500 &&
           !(samples.contains(firstId) && samples.contains(secondId))) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    ticker.stop();
    check(samples.contains(firstId),
          QStringLiteral("the effect that asks first keeps being sampled (%1 "
                         "sample(s) for it)")
            .arg(samples.contains(firstId) ? 1 : 0));
    check(samples.contains(secondId),
          QStringLiteral("the second effect is sampled in the same pass"));
    // Each sample has to describe its own rectangle: serving both ids with one
    // effect's pixels would satisfy the count above but still freeze a patch.
    if (samples.contains(firstId)) {
        const QImage& image = samples.value(firstId);
        check(near(pixelAt(image, image.rect().center()), qRgb(0, 255, 0), 12),
              QStringLiteral("the first effect's sample is its own rectangle "
                             "(%1)")
                .arg(QColor(pixelAt(image, image.rect().center())).name()));
    }
    if (samples.contains(secondId)) {
        const QImage& image = samples.value(secondId);
        check(near(pixelAt(image, image.rect().center()), qRgb(0, 0, 255), 12),
              QStringLiteral("the second effect's sample is its own rectangle "
                             "(%1)")
                .arg(QColor(pixelAt(image, image.rect().center())).name()));
    }
    sampler.stop();
}

// --------------------------------------------------- staging ownership marker --

QFileDevice::Permissions ownerOnlyRwx()
{
    return QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
}

void writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(bytes);
        file.close();
    }
}

void backdateDays(const QString& path, int days)
{
    QFile file(path);
    if (file.open(QIODevice::ReadWrite)) {
        file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-days),
                         QFileDevice::FileModificationTime);
    }
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The child half of the end-to-end marker regression. It runs in a fresh process
// whose TMPDIR is the sandbox the parent prepared, so `stagingDirectory()` is the
// *predictable* name under test and the resolution is not cached from an earlier
// call. Prints one `key=value` line per fact; the parent asserts on them.
int runStagingOwnershipProbe(const QString& userFile)
{
    const QString canonical = Recording::Clipboard::stagingDirectory();
    QString reason;
    const bool ownedBefore =
      Recording::Clipboard::isOwnedStagingDirectory(canonical, &reason);
    const QString resolved = Recording::Clipboard::ensureStagingDirectory(&reason);
    QString resolvedReason;
    const bool resolvedOwned =
      Recording::Clipboard::isOwnedStagingDirectory(resolved, &resolvedReason);
    const int swept = Recording::Clipboard::sweepStaging(canonical, 1000);
    const QByteArray bytes = readBytes(userFile);

    std::printf("canonical=%s\n", qUtf8Printable(canonical));
    std::printf("canonical_exists=%d\n", QFileInfo::exists(canonical) ? 1 : 0);
    std::printf("canonical_owned_before=%d\n", ownedBefore ? 1 : 0);
    std::printf("marker_in_canonical=%d\n",
                QFileInfo::exists(
                  Recording::Clipboard::stagingMarkerPath(canonical))
                  ? 1
                  : 0);
    std::printf("resolved=%s\n", qUtf8Printable(resolved));
    std::printf("resolved_is_canonical=%d\n", resolved == canonical ? 1 : 0);
    std::printf("resolved_owned=%d\n", resolvedOwned ? 1 : 0);
    std::printf("resolved_parent=%s\n",
                qUtf8Printable(QFileInfo(resolved).absolutePath()));
    std::printf("resolved_reason_empty=%d\n", reason.isEmpty() ? 1 : 0);
    std::printf("swept=%d\n", swept);
    std::printf("user_bytes_intact=%d\n",
                bytes == QByteArray("the user's bytes") ? 1 : 0);
    return 0;
}

// Terra's finding: privacy and the uid say a directory is *safe*, never that this
// module created it. A pre-existing owner-only directory at the predictable
// staging name must therefore never be adopted, marked or swept.
void checkStagingOwnershipMarker()
{
    const QString tempRoot =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString sandbox =
      tempRoot + QStringLiteral("/flameshot-owner-test");
    QDir(sandbox).removeRecursively();
    QDir().mkdir(sandbox);
    QFile::setPermissions(sandbox, ownerOnlyRwx());

    const QByteArray userBytes("the user's bytes");

    // ---- 1. the audit case, directly -------------------------------------
    const QString unmarked = QDir(sandbox).filePath(QStringLiteral("unmarked"));
    QDir().mkdir(unmarked);
    QFile::setPermissions(unmarked, ownerOnlyRwx());
    const QString legacyFile = QDir(unmarked).filePath(
      QStringLiteral("flameshot-recording-a-users-old-recording.mp4"));
    writeBytes(legacyFile, userBytes);
    backdateDays(legacyFile, 30);

    QString reason;
    check(Recording::Clipboard::isPrivateDirectory(unmarked, &reason),
          QStringLiteral("the fixture is owner-only and the same uid (%1)")
            .arg(reason));
    check(!Recording::Clipboard::isOwnedStagingDirectory(unmarked, &reason),
          QStringLiteral("an owner-only directory without the ownership marker "
                         "is not module-owned (%1)")
            .arg(reason));
    const int sweptUnmarked = Recording::Clipboard::sweepStaging(unmarked, 1000);
    check(sweptUnmarked == 0,
          QStringLiteral("a sweep removes nothing from a private directory this "
                         "module did not create (removed %1)")
            .arg(sweptUnmarked));
    check(readBytes(legacyFile) == userBytes,
          QStringLiteral("the user's old file in that directory survives, bytes "
                         "intact"));
    check(!QFileInfo::exists(Recording::Clipboard::stagingMarkerPath(unmarked)),
          QStringLiteral("no ownership marker is ever added to an existing "
                         "directory"));

    // ---- 2. a directory the module creates is owned and keeps retention ----
    const QString owned = QDir(sandbox).filePath(QStringLiteral("owned"));
    check(Recording::Clipboard::createOwnedStagingDirectory(owned, &reason),
          QStringLiteral("the module creates an owned staging directory (%1)")
            .arg(reason));
    const QString marker = Recording::Clipboard::stagingMarkerPath(owned);
    const QFileInfo markerInfo(marker);
    check(markerInfo.exists() && markerInfo.isFile() && !markerInfo.isSymLink(),
          QStringLiteral("the ownership marker is a plain file next to the "
                         "directory's contents"));
    check((markerInfo.permissions() &
           (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
            QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) == 0,
          QStringLiteral("the ownership marker is closed to group and others"));
    check(Recording::Clipboard::isOwnedStagingDirectory(owned, &reason),
          QStringLiteral("the fresh directory verifies as module-owned (%1)")
            .arg(reason));
    const QString expired =
      QDir(owned).filePath(QStringLiteral("flameshot-recording-expired.mp4"));
    const QString fresh =
      QDir(owned).filePath(QStringLiteral("flameshot-recording-fresh.mp4"));
    writeBytes(expired, "staged");
    writeBytes(fresh, "staged");
    backdateDays(expired, 3);
    const int ownedSwept = Recording::Clipboard::sweepStaging(owned, 1000);
    check(ownedSwept == 1 && !QFileInfo::exists(expired) &&
            QFileInfo::exists(fresh),
          QStringLiteral("retention still works inside a module-owned directory "
                         "(removed %1)")
            .arg(ownedSwept));
    check(QFileInfo::exists(marker),
          QStringLiteral("the sweep keeps the ownership marker"));

    // ---- 3. a forged marker is not evidence ------------------------------
    auto freshOwned = [&](const QString& name) {
        const QString path = QDir(sandbox).filePath(name);
        QString createReason;
        check(Recording::Clipboard::createOwnedStagingDirectory(path, &createReason),
              QStringLiteral("the module creates %1").arg(name));
        return path;
    };
    const QString wrongContent = freshOwned(QStringLiteral("forged-content"));
    writeBytes(Recording::Clipboard::stagingMarkerPath(wrongContent),
               "flameshot-recording-staging 1\nuid=0\n");
    check(!Recording::Clipboard::isOwnedStagingDirectory(wrongContent, &reason),
          QStringLiteral("a marker claiming another uid is refused (%1)")
            .arg(reason));

    const QString linkedMarker = freshOwned(QStringLiteral("forged-link"));
    const QString markerTarget =
      QDir(sandbox).filePath(QStringLiteral("marker-target"));
    writeBytes(markerTarget, "flameshot-recording-staging 1\n");
    QFile::remove(Recording::Clipboard::stagingMarkerPath(linkedMarker));
    QFile::link(markerTarget,
                Recording::Clipboard::stagingMarkerPath(linkedMarker));
    check(!Recording::Clipboard::isOwnedStagingDirectory(linkedMarker, &reason),
          QStringLiteral("a symlinked ownership marker is refused (%1)")
            .arg(reason));

    const QString readableMarker = freshOwned(QStringLiteral("readable-marker"));
    QFile::setPermissions(Recording::Clipboard::stagingMarkerPath(readableMarker),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup);
    check(!Recording::Clipboard::isOwnedStagingDirectory(readableMarker, &reason),
          QStringLiteral("an ownership marker readable by the group is refused "
                         "(%1)")
            .arg(reason));

    const QString sharedDirectory = freshOwned(QStringLiteral("shared-dir"));
    QFile::setPermissions(sharedDirectory,
                          QFile::ReadOwner | QFile::WriteOwner |
                            QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup);
    check(!Recording::Clipboard::isOwnedStagingDirectory(sharedDirectory, &reason),
          QStringLiteral("a module-marked directory that others can open is "
                         "refused (%1)")
            .arg(reason));

    // ---- 4. end to end: the predictable name is squatted ------------------
    // A fresh process gets the sandbox as its temporary location, so
    // `stagingDirectory()` really is the name the audit names, and the resolution
    // has not been cached by anything earlier in this one.
    const QString probeBase = QDir(sandbox).filePath(QStringLiteral("tmp"));
    QDir().mkdir(probeBase);
    QFile::setPermissions(probeBase, ownerOnlyRwx());
    const QString squattedName =
      QDir(probeBase).filePath(QFileInfo(Recording::Clipboard::stagingDirectory())
                                 .fileName());
    QDir().mkdir(squattedName);
    QFile::setPermissions(squattedName, ownerOnlyRwx());
    const QString squatFile =
      QDir(squattedName).filePath(
        QStringLiteral("flameshot-recording-user-legacy.mp4"));
    writeBytes(squatFile, userBytes);
    backdateDays(squatFile, 30);

    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TMPDIR"), probeBase);
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(),
                { QStringLiteral("--staging-owner-probe"), squatFile });
    child.waitForFinished(30000);
    const QString output = QString::fromUtf8(child.readAllStandardOutput());
    QHash<QString, QString> facts;
    for (const QString& line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals > 0) {
            facts.insert(line.left(equals), line.mid(equals + 1));
        }
    }
    note(QStringLiteral("staging ownership probe: %1")
           .arg(output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)
                  .join(QStringLiteral(" | "))));
    check(child.exitCode() == 0 && !facts.isEmpty(),
          QStringLiteral("the ownership probe process ran (exit %1)")
            .arg(child.exitCode()));
    check(facts.value(QStringLiteral("canonical")) == squattedName,
          QStringLiteral("the probe's temporary location is the sandbox, so the "
                         "predictable name is under test (%1)")
            .arg(facts.value(QStringLiteral("canonical"))));
    check(facts.value(QStringLiteral("canonical_owned_before")) ==
            QLatin1String("0"),
          QStringLiteral("the pre-existing directory is not module-owned"));
    check(facts.value(QStringLiteral("resolved_is_canonical")) ==
            QLatin1String("0"),
          QStringLiteral("a directory this module did not create is not "
                         "adopted as the staging directory"));
    check(facts.value(QStringLiteral("resolved_owned")) == QLatin1String("1"),
          QStringLiteral("a fresh module-owned directory is used instead, so "
                         "Copy keeps working"));
    check(facts.value(QStringLiteral("resolved_parent")) == probeBase,
          QStringLiteral("the replacement directory lives in the same temporary "
                         "location"));
    check(facts.value(QStringLiteral("marker_in_canonical")) ==
            QLatin1String("0"),
          QStringLiteral("no ownership marker is added to the pre-existing "
                         "directory"));
    check(facts.value(QStringLiteral("swept")) == QLatin1String("0"),
          QStringLiteral("the pre-existing directory is not swept (%1 removed)")
            .arg(facts.value(QStringLiteral("swept"))));
    check(facts.value(QStringLiteral("user_bytes_intact")) == QLatin1String("1"),
          QStringLiteral("the user's file in the pre-existing directory survives "
                         "with its bytes intact"));

    // Only this test's own fixtures are removed, and only through the sandbox it
    // created: the module itself never deletes an unmarked directory.
    QDir(sandbox).removeRecursively();
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // The child half of the ownership-marker regression: no QApplication, no X
    // connection, no event loop -- only the staging path rules in a process whose
    // TMPDIR is the sandbox the parent prepared.
    if (argc >= 3 &&
        QString::fromLocal8Bit(argv[1]) ==
          QLatin1String("--staging-owner-probe")) {
        return runStagingOwnershipProbe(QString::fromLocal8Bit(argv[2]));
    }
    qputenv("QT_QPA_PLATFORM", "xcb");
    QApplication app(argc, argv);

    std::printf("== live desktop + clipboard check ==\n");
    note(QStringLiteral("platform: %1, display: %2")
           .arg(QGuiApplication::platformName(),
                QString::fromLocal8Bit(qgetenv("DISPLAY"))));

    const bool composited = LiveDesktop::compositingActive();
    note(QStringLiteral("compositing manager owns _NET_WM_CM_S0: %1")
           .arg(composited ? QStringLiteral("yes")
                           : QStringLiteral("no")));
    if (composited) {
        checkLiveDesktop();
        checkShapedAndTranslucentWindows();
        checkNestedClipping();
        checkBackgroundAndPartialCoverage();
        checkManyWindowsAndResponsiveness();
        checkAsyncSampler();
        checkSamplerServesEveryEffect();
    } else {
        note(QStringLiteral("live-desktop checks skipped: without a compositing "
                            "manager the windows below the overlay are not "
                            "redirected, so no client can read them"));
        check(LiveDesktop::below(quintptr(1), QRect(0, 0, 32, 32)).isNull(),
              QStringLiteral("the sampler refuses without a compositor instead "
                             "of returning its own pixels"));
    }
    checkClipboardAndStaging();

    std::printf("\nRESULT: %s (%d failure(s))\n",
                g_failures == 0 ? "all checks passed" : "FAILURES",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
