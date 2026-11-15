#include "portalpipesource.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QEventLoop>
#include <QRandomGenerator>
#include <QTimer>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>

#include <unistd.h>

namespace Recording {
QByteArray packBgraFrame(const void* data,
                         qsizetype maxSize,
                         quint32 offset,
                         int sourceStride,
                         int width,
                         int height)
{
    if (!data || width <= 0 || height <= 0 || sourceStride < width * 4 ||
        maxSize < 0)
        return {};
    const quint64 required = quint64(offset) +
                             quint64(sourceStride) * quint64(height);
    if (required > quint64(maxSize)) return {};
    QByteArray frame(width * height * 4, Qt::Uninitialized);
    const auto* bytes = static_cast<const char*>(data) + offset;
    for (int y = 0; y < height; ++y)
        memcpy(frame.data() + y * width * 4, bytes + y * sourceStride, width * 4);
    return frame;
}

namespace {
constexpr auto service = "org.freedesktop.portal.Desktop";
constexpr auto desktopPath = "/org/freedesktop/portal/desktop";
constexpr auto requestInterface = "org.freedesktop.portal.Request";
constexpr auto screenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr auto sessionInterface = "org.freedesktop.portal.Session";

QString token()
{
    return QStringLiteral("flameshot_%1")
      .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
}

QString requestPath(const QString& token)
{
    QString sender = QDBusConnection::sessionBus().baseService();
    sender.remove(':').replace('.', '_');
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
      .arg(sender, token);
}

struct StreamInfo { quint32 node{0}; QVariantMap properties; };
void operator>>(const QDBusArgument& arg, StreamInfo& stream)
{
    arg.beginStructure();
    arg >> stream.node;
    arg >> stream.properties;
    arg.endStructure();
}

QVariantMap options(const QString& handleToken)
{
    return {{QStringLiteral("handle_token"), handleToken}};
}

} // namespace

class PortalPipeSourcePrivate {};

PortalPipeSource::PortalPipeSource(QObject* parent) : QObject(parent)
{
    pw_init(nullptr, nullptr);
}

PortalPipeSource::~PortalPipeSource() { stop(); }

bool PortalPipeSource::start(const QString& parentWindow)
{
    QDBusInterface portal(service, desktopPath, screenCastInterface,
                          QDBusConnection::sessionBus());
    if (!portal.isValid()) {
        emit failed(tr("The XDG ScreenCast portal is unavailable."));
        return false;
    }

    const QString sessionToken = token();
    const QString createToken = token();
    const QString createPath = requestPath(createToken);
    QEventLoop createLoop;
    uint status = 2;
    QVariantMap response;
    // Connect before issuing the call so the Response signal cannot be missed.
    m_requestLoop = &createLoop;
    m_requestReceived = false;
    QDBusConnection::sessionBus().connect(
      service, createPath, requestInterface, QStringLiteral("Response"), this,
      SLOT(onPortalResponse(uint,QVariantMap)));
    QVariantMap createOptions{{QStringLiteral("session_handle_token"), sessionToken},
                              {QStringLiteral("handle_token"), createToken}};
    auto createReply = portal.call(QStringLiteral("CreateSession"), createOptions);
    if (createReply.type() == QDBusMessage::ErrorMessage) {
        QDBusConnection::sessionBus().disconnect(
          service, createPath, requestInterface, QStringLiteral("Response"), this,
          SLOT(onPortalResponse(uint,QVariantMap)));
        m_requestLoop = nullptr;
        emit failed(createReply.errorMessage());
        return false;
    }
    QTimer createTimeout;
    createTimeout.setSingleShot(true);
    QObject::connect(&createTimeout, &QTimer::timeout, &createLoop, &QEventLoop::quit);
    createTimeout.start(30000);
    if (!m_requestReceived) createLoop.exec();
    status = m_requestStatus;
    response = m_requestResults;
    bool received = m_requestReceived;
    m_requestLoop = nullptr;
    QDBusConnection::sessionBus().disconnect(
      service, createPath, requestInterface, QStringLiteral("Response"), this,
      SLOT(onPortalResponse(uint,QVariantMap)));
    if (!received || status != 0) {
        emit failed(tr("The ScreenCast portal did not create a session."));
        return false;
    }
    m_sessionHandle = response.value(QStringLiteral("session_handle")).value<QDBusObjectPath>().path();

    auto request = [&](const QString& method, const QVariantList& args,
                       QVariantMap& results) {
        const QString t = token();
        const QString path = requestPath(t);
        QEventLoop loop;
        m_requestLoop = &loop;
        m_requestReceived = false;
        QDBusConnection::sessionBus().connect(
          service, path, requestInterface, QStringLiteral("Response"), this,
          SLOT(onPortalResponse(uint,QVariantMap)));
        QVariantList callArgs = args;
        QVariantMap opts = options(t);
        if (method == QStringLiteral("SelectSources")) {
            opts.insert(QStringLiteral("types"), 1u); // monitor
            opts.insert(QStringLiteral("multiple"), false);
            opts.insert(QStringLiteral("cursor_mode"), 2u); // embed cursor
        }
        callArgs << opts;
        QDBusMessage reply = portal.callWithArgumentList(QDBus::Block, method, callArgs);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            QDBusConnection::sessionBus().disconnect(
              service, path, requestInterface, QStringLiteral("Response"), this,
              SLOT(onPortalResponse(uint,QVariantMap)));
            emit failed(reply.errorMessage());
            return false;
        }
        QTimer timer; timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(30000);
        if (!m_requestReceived) loop.exec();
        results = m_requestResults;
        m_requestLoop = nullptr;
        QDBusConnection::sessionBus().disconnect(
          service, path, requestInterface, QStringLiteral("Response"), this,
          SLOT(onPortalResponse(uint,QVariantMap)));
        return m_requestReceived && m_requestStatus == 0;
    };

    QVariantMap ignored;
    if (!request(QStringLiteral("SelectSources"),
                 {QVariant::fromValue(QDBusObjectPath(m_sessionHandle))}, ignored))
        return false;
    QVariantMap started;
    if (!request(QStringLiteral("Start"),
                 {QVariant::fromValue(QDBusObjectPath(m_sessionHandle)), parentWindow}, started))
        return false;
    const auto streamsArg = started.value(QStringLiteral("streams")).value<QDBusArgument>();
    QList<StreamInfo> streams;
    streamsArg >> streams;
    if (streams.isEmpty()) {
        emit failed(tr("The ScreenCast portal returned no PipeWire stream."));
        return false;
    }
    const QPoint position = streams.first().properties.value(QStringLiteral("position")).toPoint();
    const QSize logicalSize = streams.first().properties.value(QStringLiteral("size")).toSize();
    if (logicalSize.isEmpty()) {
        emit failed(tr("The portal did not provide the selected monitor geometry."));
        return false;
    }
    m_streamLogicalGeometry = QRect(position, logicalSize);

    QDBusMessage open = portal.call(QStringLiteral("OpenPipeWireRemote"),
                                    QDBusObjectPath(m_sessionHandle), QVariantMap{});
    if (open.type() == QDBusMessage::ErrorMessage || open.arguments().isEmpty()) {
        emit failed(open.errorMessage());
        return false;
    }
    const auto remote = open.arguments().first().value<QDBusUnixFileDescriptor>();
    if (!remote.isValid()) {
        emit failed(tr("The portal returned an invalid PipeWire descriptor."));
        return false;
    }
    const int fd = dup(remote.fileDescriptor());
    return beginPipeWire(fd, streams.first().node);
}

bool PortalPipeSource::beginPipeWire(int fd, quint32 nodeId)
{
    m_loop = pw_thread_loop_new("flameshot-screencast", nullptr);
    if (!m_loop) { close(fd); return false; }
    m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    if (!m_context) { close(fd); stop(); return false; }
    m_core = pw_context_connect_fd(m_context, fd, nullptr, 0);
    if (!m_core) { close(fd); stop(); return false; }
    m_stream = pw_stream_new(m_core, "Flameshot screen capture",
                             pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                               PW_KEY_MEDIA_CATEGORY, "Capture",
                                               PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (!m_stream) { stop(); return false; }
    auto* hook = new spa_hook{};
    m_listener = hook;
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.process = [](void* data) { static_cast<PortalPipeSource*>(data)->onProcess(); };
        e.param_changed = [](void* data, uint32_t id, const spa_pod* format) {
            if (id == SPA_PARAM_Format && format)
                static_cast<PortalPipeSource*>(data)->onFormat(format);
        };
        return e;
    }();
    pw_stream_add_listener(m_stream, hook, &events, this);
    pw_thread_loop_start(m_loop);

    spa_video_info_raw info{};
    info.format = SPA_VIDEO_FORMAT_BGRA;
    info.framerate = SPA_FRACTION(30, 1);
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[] = { spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };
    pw_thread_loop_lock(m_loop);
    const int result = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, nodeId,
                                         static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                         params, 1);
    pw_thread_loop_unlock(m_loop);
    if (result < 0) { emit failed(tr("Could not connect to the PipeWire screen stream.")); stop(); return false; }
    return true;
}

void PortalPipeSource::onFormat(const void* format)
{
    auto* pod = static_cast<const spa_pod*>(format);
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(pod, &info) < 0 || info.format != SPA_VIDEO_FORMAT_BGRA) {
        emit failed(tr("PipeWire provided an unsupported pixel format."));
        return;
    }
    m_width = info.size.width;
    m_height = info.size.height;
}

void PortalPipeSource::onProcess()
{
    pw_buffer* buffer = pw_stream_dequeue_buffer(m_stream);
    if (!buffer || !buffer->buffer->n_datas) return;
    spa_data* data = &buffer->buffer->datas[0];
    spa_chunk* chunk = data->chunk;
    if (data->data && chunk && m_width > 0 && m_height > 0) {
        const int stride = chunk->stride > 0 ? chunk->stride : m_width * 4;
        QByteArray frame = packBgraFrame(data->data, data->maxsize,
                                         chunk->offset, stride,
                                         m_width, m_height);
        if (frame.isEmpty()) {
            pw_stream_queue_buffer(m_stream, buffer);
            emit failed(tr("PipeWire returned a truncated video frame."));
            return;
        }
        bool postDelivery = false;
        {
            QMutexLocker lock(&m_frameMutex);
            m_pendingFrame = std::move(frame);
            m_pendingTimestamp = buffer->time;
            if (!m_deliveryPosted) {
                m_deliveryPosted = true;
                postDelivery = true;
            }
        }
        if (postDelivery)
            QMetaObject::invokeMethod(this, &PortalPipeSource::deliverPendingFrame,
                                      Qt::QueuedConnection);
    }
    pw_stream_queue_buffer(m_stream, buffer);
}

void PortalPipeSource::stop()
{
    if (m_loop) pw_thread_loop_stop(m_loop);
    if (m_stream) pw_stream_destroy(m_stream);
    if (m_core) pw_core_disconnect(m_core);
    if (m_context) pw_context_destroy(m_context);
    if (m_loop) pw_thread_loop_destroy(m_loop);
    m_stream = nullptr; m_core = nullptr; m_context = nullptr; m_loop = nullptr;
    delete static_cast<spa_hook*>(m_listener); m_listener = nullptr;
    if (!m_sessionHandle.isEmpty()) {
        QDBusInterface session(service, m_sessionHandle, sessionInterface,
                               QDBusConnection::sessionBus());
        session.call(QStringLiteral("Close"));
        m_sessionHandle.clear();
    }
}

void PortalPipeSource::onPortalResponse(uint status, const QVariantMap& results)
{
    m_requestStatus = status;
    m_requestResults = results;
    m_requestReceived = true;
    if (m_requestLoop) m_requestLoop->quit();
}

void PortalPipeSource::deliverPendingFrame()
{
    QByteArray frame;
    quint64 timestamp = 0;
    {
        QMutexLocker lock(&m_frameMutex);
        frame.swap(m_pendingFrame);
        timestamp = m_pendingTimestamp;
        m_deliveryPosted = false;
    }
    if (!frame.isEmpty()) emit frameReady(frame, m_width, m_height, m_width * 4, timestamp);
}
} // namespace Recording
