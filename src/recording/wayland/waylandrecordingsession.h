#pragma once

#include "portalpipesource.h"
#include "../x11recordingbackend.h"

#include <QObject>
#include <QRect>

namespace Recording {

// Portal-backed selected-region session. The supplied region is in global
// logical desktop coordinates from Flameshot; the portal stream geometry is
// used to map it into physical capture pixels.
class WaylandRecordingSession final : public QObject
{
    Q_OBJECT
public:
    explicit WaylandRecordingSession(QObject* parent = nullptr);
    bool start(const QString& ffmpeg, const QRect& regionGlobalLogical,
               const QString& output, const RecordingOptions& options);
    void pause();
    void resume();
    void stop();
    PortalPipeSource* source() { return &m_source; }
    X11RecordingBackend* encoder() { return &m_encoder; }

signals:
    void failed(const QString& message);
    void finished(const QString& path);

private slots:
    void submitCroppedFrame(const QByteArray& bgra, int width, int height,
                            int stride, quint64 timestampNs);

private:
    PortalPipeSource m_source;
    X11RecordingBackend m_encoder;
    QRect m_region;
    QString m_ffmpeg;
    QString m_output;
    RecordingOptions m_options;
    bool m_started{false};
    bool m_encoderStarted{false};
};

} // namespace Recording
