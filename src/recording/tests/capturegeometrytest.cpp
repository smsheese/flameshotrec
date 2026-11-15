// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

// Focused regression tests for the pure overlay rules: desktop spanning with
// negative monitor origins, the recording boundary that must stay outside the
// encoded crop, click-through input shape, and deterministic annotation
// lifetimes. No display server, desktop capture or window is involved.

#include "utils/capturegeometry.h"

#include <QRegion>
#include <QTest>

class CaptureGeometryTest : public QObject
{
    Q_OBJECT

private slots:
    void unitedDesktopHandlesNegativeOrigins();
    void unitedDesktopHandlesGapsAndVerticalStacks();
    void spansMultipleScreens_data();
    void spansMultipleScreens();
    void boundaryRingNeverTouchesRegion();
    void boundaryRingIsTheStripOutside();
    void annotationOpacityFollowsHoldThenFade();
    void annotationExpirationBoundaries();
    void overlayInputRegionInteractIsControlsOnly();
    void overlayInputRegionDrawAddsTheRegion();
    void overlayInputRegionClipsToWidget();
    void physicalRectKeepsFractionsContiguous();
};

void CaptureGeometryTest::unitedDesktopHandlesNegativeOrigins()
{
    const QList<QRect> screens{ QRect(-1920, 0, 1920, 1080),
                                QRect(0, 0, 2560, 1440) };
    const QRect desktop = CaptureGeometry::unitedDesktop(screens);
    QCOMPARE(desktop, QRect(-1920, 0, 4480, 1440));
    QCOMPARE(desktop.topLeft(), QPoint(-1920, 0));
    QVERIFY(CaptureGeometry::spansMultipleScreens(screens));
}

void CaptureGeometryTest::unitedDesktopHandlesGapsAndVerticalStacks()
{
    const QList<QRect> screens{ QRect(0, -1080, 1920, 1080),
                                QRect(0, 0, 1920, 1080) };
    QCOMPARE(CaptureGeometry::unitedDesktop(screens),
             QRect(0, -1080, 1920, 2160));

    // A gap between monitors is part of the overlay canvas; a region can span
    // it, so the overlay must cover it.
    const QList<QRect> withGap{ QRect(0, 0, 1000, 800),
                                QRect(1200, 0, 1000, 800) };
    QCOMPARE(CaptureGeometry::unitedDesktop(withGap),
             QRect(0, 0, 2200, 800));
}

// QTest requires the table function to be named `<slot>_data()`. With any other
// suffix it runs as an ordinary test and the data-driven slot then aborts in
// QTest::fetchData() with "Test data requested, but no testdata available" --
// that abort is ledger entry C3, not a geometry-math failure.
void CaptureGeometryTest::spansMultipleScreens_data()
{
    QTest::addColumn<QList<QRect>>("screens");
    QTest::addColumn<bool>("spans");

    QTest::newRow("no screens") << QList<QRect>{} << false;
    QTest::newRow("one screen") << QList<QRect>{ QRect(0, 0, 1920, 1080) }
                                 << false;
    QTest::newRow("two mirrored screens")
      << QList<QRect>{ QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1080) }
      << false;
    QTest::newRow("negative origin")
      << QList<QRect>{ QRect(-1920, 0, 1920, 1080), QRect(0, 0, 1920, 1080) }
      << true;
    QTest::newRow("stacked")
      << QList<QRect>{ QRect(0, 0, 1920, 1080), QRect(0, 1080, 1920, 1080) }
      << true;
    QTest::newRow("gap between monitors")
      << QList<QRect>{ QRect(0, 0, 1000, 800), QRect(1200, 0, 1000, 800) }
      << true;
}

void CaptureGeometryTest::spansMultipleScreens()
{
    QFETCH(QList<QRect>, screens);
    QFETCH(bool, spans);
    QCOMPARE(CaptureGeometry::spansMultipleScreens(screens), spans);
}

void CaptureGeometryTest::boundaryRingNeverTouchesRegion()
{
    const QRect region(100, 200, 640, 480);
    for (int width = 1; width <= 8; ++width) {
        const QRegion ring = CaptureGeometry::boundaryRing(region, width);
        QVERIFY(!ring.isEmpty());
        // The selected border and handles live inside the region; the ring must
        // not share a single pixel with the encoded crop.
        QVERIFY(ring.intersected(QRegion(region)).isEmpty());
        QVERIFY(!ring.contains(region.topLeft()));
        QVERIFY(!ring.contains(region.bottomRight()));
    }
}

void CaptureGeometryTest::boundaryRingIsTheStripOutside()
{
    const QRect region(0, 0, 10, 10);
    const QRegion ring = CaptureGeometry::boundaryRing(region, 2);
    QVERIFY(ring.contains(QPoint(-1, -1)));
    QVERIFY(ring.contains(QPoint(-2, 4)));
    QVERIFY(ring.contains(QPoint(10, 4)));
    QVERIFY(ring.contains(QPoint(5, -2)));
    QVERIFY(!ring.contains(QPoint(0, 0)));
    QVERIFY(!ring.contains(QPoint(9, 9)));
    // Nothing further out than two pixels from the region.
    QVERIFY(!ring.contains(QPoint(-3, 4)));
    QVERIFY(!ring.contains(QPoint(12, 4)));
    QCOMPARE(ring.boundingRect(), QRect(-2, -2, 14, 14));

    QVERIFY(CaptureGeometry::boundaryRing(QRect(), 2).isEmpty());
    QVERIFY(CaptureGeometry::boundaryRing(region, 0).isEmpty());
}

void CaptureGeometryTest::annotationOpacityFollowsHoldThenFade()
{
    // Defaults: 3000 ms hold measured from the release, then a 300 ms fade.
    QCOMPARE(CaptureGeometry::annotationOpacity(0, 3000, 300), 1.0);
    QCOMPARE(CaptureGeometry::annotationOpacity(2999, 3000, 300), 1.0);
    QCOMPARE(CaptureGeometry::annotationOpacity(3000, 3000, 300), 1.0);
    QCOMPARE(CaptureGeometry::annotationOpacity(3150, 3000, 300), 0.5);
    QCOMPARE(CaptureGeometry::annotationOpacity(3300, 3000, 300), 0.0);
    QCOMPARE(CaptureGeometry::annotationOpacity(10000, 3000, 300), 0.0);

    // A stroke finished late in the session still gets the full hold: the age
    // is measured from its own release, never from the start of the recording.
    QCOMPARE(CaptureGeometry::annotationOpacity(0, 3000, 300), 1.0);

    // Zero fade removes the annotation once the hold elapses.
    QCOMPARE(CaptureGeometry::annotationOpacity(0, 0, 0), 1.0);
    QCOMPARE(CaptureGeometry::annotationOpacity(1, 0, 0), 0.0);
    // Zero hold fades immediately.
    QCOMPARE(CaptureGeometry::annotationOpacity(0, 0, 100), 1.0);
    QCOMPARE(CaptureGeometry::annotationOpacity(50, 0, 100), 0.5);
}

void CaptureGeometryTest::annotationExpirationBoundaries()
{
    QVERIFY(!CaptureGeometry::annotationExpired(3299, 3000, 300));
    QVERIFY(CaptureGeometry::annotationExpired(3300, 3000, 300));
    QVERIFY(CaptureGeometry::annotationExpired(0, 0, 0));
    QVERIFY(!CaptureGeometry::annotationExpired(999, 1000, 0));
    QVERIFY(CaptureGeometry::annotationExpired(1000, 1000, 0));
}

void CaptureGeometryTest::overlayInputRegionInteractIsControlsOnly()
{
    const QRect widget(0, 0, 3840, 1080);
    const QRect region(1920, 100, 800, 600);
    const QList<QRect> overlays{ QRect(10, 10, 40, 40), QRect(1900, 900, 200, 40) };

    // Interacting: the whole desktop, including the other monitor, receives
    // normal pointer events; only the overlay controls take input.
    const QRegion interact = CaptureGeometry::overlayInputRegion(
      widget, region, overlays, false);
    QVERIFY(!interact.contains(region.center()));
    QVERIFY(!interact.contains(QPoint(100, 100)));
    QVERIFY(interact.contains(QPoint(20, 20)));
    QVERIFY(interact.contains(QPoint(1950, 920)));
    QCOMPARE(interact.rectCount(), 2);
}

void CaptureGeometryTest::overlayInputRegionDrawAddsTheRegion()
{
    const QRect widget(0, 0, 3840, 1080);
    const QRect region(1920, 100, 800, 600);
    const QList<QRect> overlays{ QRect(10, 10, 40, 40) };

    // Drawing: the recorded region also takes input, so strokes reach the
    // overlay; everything else stays click-through.
    const QRegion draw =
      CaptureGeometry::overlayInputRegion(widget, region, overlays, true);
    QVERIFY(draw.contains(region.center()));
    QVERIFY(draw.contains(QPoint(20, 20)));
    QVERIFY(!draw.contains(QPoint(100, 100)));
    QVERIFY(!draw.contains(QPoint(3800, 1000)));

    // An empty region adds nothing.
    const QRegion none =
      CaptureGeometry::overlayInputRegion(widget, QRect(), overlays, true);
    QVERIFY(!none.contains(region.center()));
    QVERIFY(none.contains(QPoint(20, 20)));
}

void CaptureGeometryTest::overlayInputRegionClipsToWidget()
{
    const QRect widget(0, 0, 800, 600);
    const QList<QRect> overlays{ QRect(-50, -50, 100, 100),
                                 QRect(700, 500, 400, 400) };
    const QRegion input = CaptureGeometry::overlayInputRegion(
      widget, QRect(), overlays, false);
    QVERIFY(input.contains(QPoint(0, 0)));
    QVERIFY(!input.contains(QPoint(850, 650)));
    QVERIFY(input.boundingRect().right() <= widget.right());
    QVERIFY(input.boundingRect().bottom() <= widget.bottom());
}

void CaptureGeometryTest::physicalRectKeepsFractionsContiguous()
{
    QCOMPARE(CaptureGeometry::physicalRect(QRect(10, 20, 30, 40), 1.0),
             QRect(10, 20, 30, 40));
    QCOMPARE(CaptureGeometry::physicalRect(QRect(-100, -50, 200, 100), 2.0),
             QRect(-200, -100, 400, 200));

    // Neighboring logical rectangles must not overlap or leave a gap once
    // scaled: edges are scaled, not widths.
    const qreal scale = 1.5;
    const QRect left = CaptureGeometry::physicalRect(QRect(0, 0, 101, 10), scale);
    const QRect right =
      CaptureGeometry::physicalRect(QRect(101, 0, 99, 10), scale);
    QCOMPARE(left.right() + 1, right.left());
}

QTEST_MAIN(CaptureGeometryTest)
#include "capturegeometrytest.moc"
