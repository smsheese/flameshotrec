#pragma once

#include <QByteArray>
#include <QDBusMessage>
#include <QEventLoop>
#include <QObject>
#include <QMutex>
#include <QRect>
#include <QString>
#include <QVariantMap>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;

namespace Recording {

QByteArray packBgraFrame(const void* data, qsizetype maxSize, quint32 offset,
                         int sourceStride, int width, int height);

// Starts a portal-authorized PipeWire stream for the user-selected monitor.
// Frames are emitted as tightly packed BGRA bytes in source pixel dimensions.
class PortalPipeSource final : public QObject
{
    Q_OBJECT
public:
    explicit PortalPipeSource(QObject* parent = nullptr);
    ~PortalPipeSource() override;

    bool start(const QString& parentWindow = QStringLiteral("wayland:"));
    void stop();
    int width() const { return m_width; }
    int height() const { return m_height; }
    QRect streamLogicalGeometry() const { return m_streamLogicalGeometry; }

signals:
    void frameReady(const QByteArray& bgra, int width, int height, int stride,
                    quint64 timestampNs);
    void failed(const QString& message);

private:
    bool beginPipeWire(int fd, quint32 nodeId);
    void onProcess();
    void onFormat(const void* format);

private slots:
    void onPortalResponse(uint status, const QVariantMap& results);
    void deliverPendingFrame();

private:
    pw_thread_loop* m_loop{nullptr};
    pw_context* m_context{nullptr};
    pw_core* m_core{nullptr};
    pw_stream* m_stream{nullptr};
    void* m_listener{nullptr};
    int m_width{0};
    int m_height{0};
    int m_stride{0};
    QRect m_streamLogicalGeometry;
    QString m_sessionHandle;
    QVariantMap m_requestResults;
    uint m_requestStatus{2};
    bool m_requestReceived{false};
    QEventLoop* m_requestLoop{nullptr};
    QMutex m_frameMutex;
    QByteArray m_pendingFrame;
    quint64 m_pendingTimestamp{0};
    bool m_deliveryPosted{false};
};

} // namespace Recording
