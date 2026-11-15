#include "waylandrecordingsession.h"

#include <QImage>
#include <QtMath>

namespace Recording {
WaylandRecordingSession::WaylandRecordingSession(QObject* parent)
  : QObject(parent)
  , m_source(this)
  , m_encoder(this)
{
    connect(&m_source, &PortalPipeSource::failed,
            this, [this](const QString& message) {
                QMetaObject::invokeMethod(this, [this, message]() {
                    if (!m_started) return;
                    m_started = false;
                    m_source.stop();
                    if (m_encoderStarted)
                        m_encoder.abort(message);
                    else
                        emit failed(message);
                }, Qt::QueuedConnection);
            });
    connect(&m_source, &PortalPipeSource::frameReady,
            this, &WaylandRecordingSession::submitCroppedFrame,
            Qt::QueuedConnection);
    connect(&m_encoder, &X11RecordingBackend::recordingFinished,
            this, &WaylandRecordingSession::finished);
}

bool WaylandRecordingSession::start(const QString& ffmpeg,
                                    const QRect& regionGlobalLogical,
                                    const QString& output,
                                    const RecordingOptions& options)
{
    if (m_started || regionGlobalLogical.isEmpty()) return false;
    m_region = regionGlobalLogical;
    m_ffmpeg = ffmpeg;
    m_output = output;
    m_options = options;
    if (!m_source.start()) return false;
    m_started = true;
    return true;
}

void WaylandRecordingSession::pause() { m_encoder.pause(); }
void WaylandRecordingSession::resume() { m_encoder.resume(); }

void WaylandRecordingSession::stop()
{
    m_started = false;
    m_source.stop();
    if (m_encoderStarted) m_encoder.stop();
}

void WaylandRecordingSession::submitCroppedFrame(const QByteArray& bgra,
                                                  int width,
                                                  int height,
                                                  int stride,
                                                  quint64 timestampNs)
{
    if (!m_started || m_encoder.state() == X11RecordingBackend::State::Paused)
        return;
    const QRect logicalStream = m_source.streamLogicalGeometry();
    if (logicalStream.isEmpty() || !logicalStream.contains(m_region) ||
        stride < width * 4 || bgra.size() < stride * height) {
        const QString reason = tr("The selected region does not fit inside the portal stream.");
        m_started = false;
        m_source.stop();
        if (m_encoderStarted)
            m_encoder.abort(reason);
        else
            emit failed(reason);
        return;
    }
    const qreal scaleX = qreal(width) / logicalStream.width();
    const qreal scaleY = qreal(height) / logicalStream.height();
    const int left = qFloor((m_region.left() - logicalStream.left()) * scaleX);
    const int top = qFloor((m_region.top() - logicalStream.top()) * scaleY);
    const int right = qCeil((m_region.right() + 1 - logicalStream.left()) * scaleX);
    const int bottom = qCeil((m_region.bottom() + 1 - logicalStream.top()) * scaleY);
    const QRect crop(left, top, right - left, bottom - top);
    if (crop.left() < 0 || crop.top() < 0 || crop.right() >= width || crop.bottom() >= height) {
        const QString reason = tr("The selected region cannot be mapped to the portal stream.");
        m_started = false;
        m_source.stop();
        if (m_encoderStarted)
            m_encoder.abort(reason);
        else
            emit failed(reason);
        return;
    }
    QImage image(reinterpret_cast<const uchar*>(bgra.constData()), width, height,
                 stride, QImage::Format_ARGB32);
    const QImage cropped = image.copy(crop);
    if (!m_encoderStarted) {
        if (!m_encoder.startEncoder(m_ffmpeg, cropped.size(), m_output, m_options)) {
            emit failed(tr("Could not start FFmpeg with the selected recording settings."));
            m_started = false;
            m_source.stop();
            return;
        }
        m_encoderStarted = true;
    }
    const QByteArray packed(reinterpret_cast<const char*>(cropped.constBits()),
                            cropped.sizeInBytes());
    m_encoder.submitFrame(packed, cropped.width(), cropped.height(),
                          cropped.bytesPerLine(), timestampNs);
}
} // namespace Recording
