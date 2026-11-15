// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot IA Contributors

// Regression harness for the AI ask/translate/explain flow.
//
// It drives the real code: AITool::pressed(), AIDialog, AIChatWidget,
// OpenAIClient and Flameshot::exportCapture(). Nothing touches the desktop,
// the microphone or the network:
//
//  * the capture is a painted snapshot, never a screen grab;
//  * the configuration comes from a throw-away XDG_CONFIG_HOME (see run.sh),
//    so the user's own Flameshot config and API token are never read;
//  * the API endpoint is a QTcpServer on 127.0.0.1 that answers with a canned
//    OpenAI compatible completion.
//
// Behaviour under test:
//  1. a cancelled prompt leaves the editor untouched: no request is emitted,
//     no AI task is queued, nothing is pending and no window opens;
//  2. an accepted prompt dismisses the editor immediately (it is asked to
//     close) and hands exactly one answer over to the backend;
//  3. the backend reuses that answer instead of asking again, starts exactly
//     one request for the marked snapshot and shows the answer;
//  4. a request that never went through the AI tool (e.g. `flameshot gui
//     --ai`) still prompts, without blocking its caller, and starts exactly
//     one request.

#include "core/flameshot.h"
#include "tools/ai/aitool.h"
#include "tools/capturecontext.h"
#include "utils/confighandler.h"
#include "widgets/ai/aichatwidget.h"
#include "widgets/ai/aidialog.h"

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextEdit>
#include <QThread>
#include <QTimer>

#include <functional>

namespace {

int g_failures = 0;
QString g_scenario;

void report(bool ok, const QString& what, const QString& detail = QString())
{
    if (ok) {
        qInfo().noquote() << QStringLiteral("  ok   %1").arg(what);
        return;
    }
    ++g_failures;
    QString message = QStringLiteral("  FAIL [%1] %2").arg(g_scenario, what);
    if (!detail.isEmpty()) {
        message += QStringLiteral(" (%1)").arg(detail);
    }
    qCritical().noquote() << message;
}

void check(bool condition, const QString& what, const QString& detail = QString())
{
    report(condition, what, detail);
}

void checkEqual(int actual, int expected, const QString& what)
{
    report(actual == expected,
           what,
           QStringLiteral("got %1, want %2").arg(actual).arg(expected));
}

// A painted snapshot stands in for the capture editor's marked pixmap. The
// annotations make it distinguishable from a plain fill, so the harness can
// prove that the *marked* image is the one that reaches the request.
QPixmap markedSnapshot()
{
    QPixmap pixmap(96, 64);
    pixmap.fill(QColor(12, 34, 56));
    QPainter painter(&pixmap);
    painter.setPen(QPen(Qt::red, 4));
    painter.drawLine(4, 4, 92, 60);
    painter.setPen(Qt::white);
    painter.drawText(QRect(0, 0, 96, 20), Qt::AlignCenter, "marked");
    painter.end();
    return pixmap;
}

QString snapshotBase64(const QPixmap& pixmap)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG");
    return QString::fromLatin1(bytes.toBase64());
}

// Minimal OpenAI compatible endpoint: counts requests and answers with a
// canned completion. Records the last request body for assertions.
class StubApiServer : public QTcpServer
{
public:
    QString replyText = QStringLiteral("stub completion");
    int requestCount = 0;
    QByteArray lastBody;

protected:
    void incomingConnection(qintptr handle) override
    {
        auto* socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            auto it = m_buffers.find(socket);
            if (it == m_buffers.end()) {
                return;
            }
            *it += socket->readAll();
            const int headerEnd = it->indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            int contentLength = 0;
            const QByteArray headers = it->left(headerEnd);
            for (const QByteArray& line : headers.split('\n')) {
                const QByteArray trimmed = line.trimmed();
                if (trimmed.toLower().startsWith("content-length:")) {
                    contentLength = trimmed.mid(15).trimmed().toInt();
                }
            }
            const QByteArray body = it->mid(headerEnd + 4);
            if (body.size() < contentLength) {
                return; // wait for the rest of the payload
            }
            ++requestCount;
            lastBody = body.left(contentLength);
            respond(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }

private:
    void respond(QTcpSocket* socket)
    {
        const QJsonObject choice{
            { QStringLiteral("message"),
              QJsonObject{ { QStringLiteral("content"), replyText } } }
        };
        const QByteArray payload = QJsonDocument(
                                     QJsonObject{ { QStringLiteral("choices"),
                                                    QJsonArray{ choice } } })
                                     .toJson();
        QByteArray response = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Connection: close\r\n"
                              "Content-Length: " +
                              QByteArray::number(payload.size()) + "\r\n\r\n" +
                              payload;
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
    }

    QHash<QTcpSocket*, QByteArray> m_buffers;
};

// Answers the next AI prompt that opens. The prompt is modal, so the answer
// has to come from the event loop while AITool::pressed() blocks in exec().
class PromptAnswerer : public QObject
{
public:
    enum Answer
    {
        Accept,
        Reject,
    };

    explicit PromptAnswerer(Answer answer)
      : m_answer(answer)
    {
        m_timer.setInterval(5);
        connect(&m_timer, &QTimer::timeout, this, &PromptAnswerer::poll);
    }

    void arm(const QString& mode = QStringLiteral("translate"),
             const QString& prompt = QStringLiteral("say it in French"))
    {
        m_wantedMode = mode;
        m_wantedPrompt = prompt;
        m_answeredMode.clear();
        m_answeredPrompt.clear();
        m_timer.start();
    }

    void disarm() { m_timer.stop(); }
    const QString& answeredMode() const { return m_answeredMode; }
    const QString& answeredPrompt() const { return m_answeredPrompt; }

private:
    void poll()
    {
        auto* dialog = qobject_cast<AIDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        m_timer.stop();

        if (auto* combo = dialog->findChild<QComboBox*>()) {
            for (int index = 0; index < combo->count(); ++index) {
                if (combo->itemData(index).toString() == m_wantedMode) {
                    combo->setCurrentIndex(index);
                    break;
                }
            }
        }
        if (auto* edit = dialog->findChild<QPlainTextEdit*>()) {
            edit->setPlainText(m_wantedPrompt);
        }

        m_answeredMode = dialog->mode();
        m_answeredPrompt = dialog->prompt();

        if (m_answer == Accept) {
            dialog->accept();
        } else {
            dialog->reject();
        }
    }

    QTimer m_timer;
    Answer m_answer;
    QString m_wantedMode;
    QString m_wantedPrompt;
    QString m_answeredMode;
    QString m_answeredPrompt;
};

QVector<CaptureTool::Request>& requestLog()
{
    static QVector<CaptureTool::Request> log;
    return log;
}

QString requestName(CaptureTool::Request request)
{
    switch (request) {
        case CaptureTool::REQ_CLOSE_GUI:
            return QStringLiteral("REQ_CLOSE_GUI");
        case CaptureTool::REQ_CAPTURE_DONE_OK:
            return QStringLiteral("REQ_CAPTURE_DONE_OK");
        case CaptureTool::REQ_CAPTURE_DONE_OK_KEEP_OPEN:
            return QStringLiteral("REQ_CAPTURE_DONE_OK_KEEP_OPEN");
        case CaptureTool::REQ_CLEAR_SELECTION:
            return QStringLiteral("REQ_CLEAR_SELECTION");
        default:
            return QStringLiteral("REQ_%1").arg(int(request));
    }
}

QString requestLogText()
{
    QStringList names;
    for (auto request : requestLog()) {
        names.append(requestName(request));
    }
    return names.join(QStringLiteral(", "));
}

AIChatWidget* findChatWidget()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* chat = qobject_cast<AIChatWidget*>(widget)) {
            return chat;
        }
    }
    return nullptr;
}

// Only a visible dialog is asking the user. Dialogs the harness keeps around
// to seed a stored answer are top-level widgets too, but never shown.
AIDialog* findPromptDialog()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* dialog = qobject_cast<AIDialog*>(widget)) {
            if (dialog->isVisible()) {
                return dialog;
            }
        }
    }
    return nullptr;
}

QTextEdit* chatResponseEdit(AIChatWidget* chat)
{
    return chat == nullptr ? nullptr : chat->findChild<QTextEdit*>();
}

QPushButton* chatCopyButton(AIChatWidget* chat)
{
    if (chat == nullptr) {
        return nullptr;
    }
    for (auto* button : chat->findChildren<QPushButton*>()) {
        if (button->text() == QLatin1String("Copy")) {
            return button;
        }
    }
    return nullptr;
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

CaptureRequest aiRequest()
{
    CaptureRequest request(CaptureRequest::GRAPHICAL_MODE);
    request.addTask(CaptureRequest::AI);
    return request;
}

CaptureContext markedContext()
{
    CaptureContext context;
    context.screenshot = markedSnapshot();
    context.selection = QRect(0, 0, 96, 64);
    context.fullscreen = false;
    context.toolSize = 3;
    context.circleCount = 1;
    return context;
}

// 1. Cancelling the prompt must be a no-op for the capture editor.
void scenarioCancelKeepsCapture(const StubApiServer& api)
{
    g_scenario = QStringLiteral("cancel keeps the capture");
    qInfo().noquote() << "scenario:" << g_scenario;

    requestLog().clear();
    AITool tool;
    QObject::connect(&tool,
                     &CaptureTool::requestAction,
                     [](CaptureTool::Request request) {
                         requestLog().append(request);
                     });

    CaptureContext context = markedContext();
    context.request = CaptureRequest(CaptureRequest::GRAPHICAL_MODE);
    const QPixmap before = context.screenshot;

    // An answer stored by an earlier press must not survive a cancellation.
    AIDialog staleDialog(QStringLiteral("explain"), QStringLiteral("stale"));
    AIDialog::setPendingAnswer(staleDialog);

    PromptAnswerer answerer(PromptAnswerer::Reject);
    answerer.arm();
    tool.pressed(context);
    answerer.disarm();

    check(answerer.answeredMode().isEmpty() == false,
          "the prompt was shown before the editor changed");
    check(requestLog().isEmpty(),
          "no editor request is emitted on cancel",
          requestLogText());
    check((context.request.tasks() & CaptureRequest::AI) == 0,
          "no AI task is queued on the request");
    check((context.request.tasks() & CaptureRequest::SAVE) == 0,
          "no save task is queued on the request");
    check(AIDialog::pendingAnswerMode().isEmpty(),
          "nothing is handed over to the backend");
    check(AIDialog::pendingAnswerPrompt().isEmpty(),
          "no older answer is reused after a cancel");
    check(context.screenshot.toImage() == before.toImage(),
          "the capture is left untouched");
    check(findChatWidget() == nullptr, "no AI window is opened");
    checkEqual(api.requestCount, 0, "no request reaches the API");
}

// 2. Accepting the prompt must dismiss the editor at once and hand over one
//    answer. The dismissal itself belongs to the capture widget; the tool's
//    contract is the request sequence below.
void scenarioAcceptDismissesEditor()
{
    g_scenario = QStringLiteral("accept dismisses the editor");
    qInfo().noquote() << "scenario:" << g_scenario;

    requestLog().clear();
    AITool tool;
    QObject::connect(&tool,
                     &CaptureTool::requestAction,
                     [](CaptureTool::Request request) {
                         requestLog().append(request);
                     });

    CaptureContext context = markedContext();
    context.request = CaptureRequest(CaptureRequest::GRAPHICAL_MODE);

    PromptAnswerer answerer(PromptAnswerer::Accept);
    answerer.arm();
    tool.pressed(context);
    answerer.disarm();

    const QVector<CaptureTool::Request> expected{
        CaptureTool::REQ_CLEAR_SELECTION,
        CaptureTool::REQ_CAPTURE_DONE_OK,
        CaptureTool::REQ_CLOSE_GUI,
    };
    check(requestLog() == expected,
          "the editor is told to finish and to close, in that order",
          requestLogText());
    check(!requestLog().contains(
            CaptureTool::REQ_CAPTURE_DONE_OK_KEEP_OPEN),
          "the editor is not asked to stay open");
    check((context.request.tasks() & CaptureRequest::AI) != 0,
          "the AI task is queued exactly once");
    check((context.request.tasks() & CaptureRequest::SAVE) == 0,
          "no implicit save is requested");

    const QString answeredMode = AIDialog::pendingAnswerMode();
    const QString answeredPrompt = AIDialog::pendingAnswerPrompt();
    check(!answeredMode.isEmpty(),
          "one answer is handed over to the backend");
    check(answeredMode == QLatin1String("translate"),
          "the chosen mode is carried over",
          answeredMode);
    check(answeredPrompt == QLatin1String("say it in French"),
          "the chosen prompt is carried over",
          answeredPrompt);
    check(findChatWidget() == nullptr,
          "accepting the prompt alone does not start a request");
}

// 3. The backend must reuse the answered prompt, start exactly one request
//    with the marked snapshot and show the answer independently of the editor.
void scenarioBackendUsesAnswerOnce(StubApiServer& api)
{
    g_scenario = QStringLiteral("backend starts one request");
    qInfo().noquote() << "scenario:" << g_scenario;

    api.requestCount = 0;
    api.lastBody.clear();
    api.replyText = QStringLiteral("translated answer");

    // Same hand-off the AI tool performs when the user accepts the prompt.
    AIDialog answeredDialog(QStringLiteral("translate"),
                            QStringLiteral("say it in French"));
    AIDialog::setPendingAnswer(answeredDialog);

    QPixmap marked = markedSnapshot();
    QRect geometry(0, 0, marked.width(), marked.height());
    CaptureRequest request = aiRequest();
    Flameshot::instance()->exportCapture(marked, geometry, request);

    AIChatWidget* chat = findChatWidget();
    check(chat != nullptr, "the AI window opens without the editor");
    check(AIDialog::pendingAnswerMode().isEmpty(),
          "the answer is consumed by the backend");
    check(findPromptDialog() == nullptr, "the user is not asked a second time");

    QTextEdit* response = chatResponseEdit(chat);
    const bool answeredOk =
      waitFor([response]() {
          return response != nullptr && !response->toPlainText().isEmpty();
      });
    check(answeredOk, "the answer is displayed");
    if (answeredOk) {
        check(response->toPlainText().contains(api.replyText),
              "the displayed text is the model answer",
              response->toPlainText().left(40));
    }

    checkEqual(api.requestCount, 1, "exactly one request reaches the API");

    const QString body = QString::fromUtf8(api.lastBody);
    check(body.contains(QStringLiteral("stub-model")),
          "the configured model is used");
    check(body.contains(QStringLiteral("Translate all visible text")),
          "the translate mode is used");
    check(body.contains(snapshotBase64(marked)),
          "the marked snapshot is the image that was sent");
    check(chatResponseEdit(findChatWidget()) == response,
          "the same window shows the answer");
    check(chatCopyButton(chat) != nullptr &&
            chatCopyButton(chat)->isEnabled(),
          "the answer can be copied");

    const int requestsBefore = api.requestCount;
    if (chat != nullptr) {
        chat->close();
    }
    waitFor([]() { return findChatWidget() == nullptr; });
    checkEqual(api.requestCount, requestsBefore,
               "closing the window does not repeat the request");
}

// 4. Requests that skip the AI tool still prompt. The prompt must not block
//    the caller, because it runs while the capture editor is torn down.
void scenarioBackendPromptsWithoutBlocking(StubApiServer& api)
{
    g_scenario = QStringLiteral("backend prompts once, without blocking");
    qInfo().noquote() << "scenario:" << g_scenario;

    api.requestCount = 0;
    api.lastBody.clear();
    api.replyText = QStringLiteral("explained answer");
    AIDialog::clearPendingAnswer();

    QPixmap marked = markedSnapshot();
    QRect geometry(0, 0, marked.width(), marked.height());
    CaptureRequest request = aiRequest();
    Flameshot::instance()->exportCapture(marked, geometry, request);

    AIDialog* prompt = findPromptDialog();
    check(prompt != nullptr, "the prompt opens");
    check(findChatWidget() == nullptr,
          "no request starts before the prompt is answered");
    // F1: the prompt runs while the capture path already finished, so the
    // process must not decide it has nothing left to do and exit under the
    // user (the answer would be destroyed).
    check(Flameshot::instance()->externalWindowCount() >= 1,
          "the open prompt counts as a window that must outlive the capture");
    check(!Flameshot::instance()->shouldExitAfterCapture(),
          "an unanswered prompt keeps `flameshot gui` alive");

    if (prompt != nullptr) {
        for (int index = 0; index < prompt->findChild<QComboBox*>()->count();
             ++index) {
            if (prompt->findChild<QComboBox*>()->itemData(index).toString() ==
                QLatin1String("explain")) {
                prompt->findChild<QComboBox*>()->setCurrentIndex(index);
                break;
            }
        }
        prompt->findChild<QPlainTextEdit*>()->setPlainText(
          QStringLiteral("what is this?"));
        prompt->accept();
    }

    AIChatWidget* chat = findChatWidget();
    check(chat != nullptr, "the AI window opens after the prompt");
    QTextEdit* response = chatResponseEdit(chat);
    const bool answeredOk =
      waitFor([response]() {
          return response != nullptr && !response->toPlainText().isEmpty();
      });
    check(answeredOk, "the answer is displayed");
    checkEqual(api.requestCount, 1, "exactly one request reaches the API");
    check(QString::fromUtf8(api.lastBody)
            .contains(QStringLiteral("Explain in detail")),
          "the explain mode is used");

    if (chat != nullptr) {
        chat->close();
    }
    waitFor([]() { return findChatWidget() == nullptr; });
    check(findPromptDialog() == nullptr,
          "no second prompt is left open");
    // Nothing is left to show, so a done capture releases the process again.
    waitFor([]() { return Flameshot::instance()->externalWindowCount() == 0; });
    check(Flameshot::instance()->externalWindowCount() == 0,
          "every AI window was deregistered (counter back to zero)");
    check(Flameshot::instance()->shouldExitAfterCapture(),
          "a finished capture releases `flameshot gui` again");
}

// 5. A prompt that the user cancels is the one case where `flameshot gui` may
//    end again: the prompt's window is gone, nothing else holds the process, so
//    the ordinary "last window closed" exit path applies. The point of the
//    check is that the *open* prompt is what keeps the process alive, not an
//    unconditional "never exit after a capture".
void scenarioCancelledPromptReleasesTheProcess()
{
    g_scenario = QStringLiteral("a cancelled prompt releases the process");
    qInfo().noquote() << "scenario:" << g_scenario;

    AIDialog::clearPendingAnswer();
    QPixmap marked = markedSnapshot();
    QRect geometry(0, 0, marked.width(), marked.height());
    CaptureRequest request = aiRequest();
    Flameshot::instance()->exportCapture(marked, geometry, request);

    AIDialog* prompt = findPromptDialog();
    check(prompt != nullptr, "the prompt opens");
    check(!Flameshot::instance()->shouldExitAfterCapture(),
          "the open prompt keeps the process alive");
    if (prompt != nullptr) {
        prompt->reject();
    }
    waitFor([]() { return findPromptDialog() == nullptr; });
    check(findPromptDialog() == nullptr, "the rejected prompt is gone");
    check(findChatWidget() == nullptr,
          "a rejected prompt starts no AI request");
    waitFor([]() { return Flameshot::instance()->externalWindowCount() == 0; });
    check(Flameshot::instance()->externalWindowCount() == 0,
          "a rejected prompt registers no window any more");
    check(Flameshot::instance()->shouldExitAfterCapture(),
          "a cancelled prompt leaves nothing that must keep the process alive");
}

bool configureStub(int port)
{
    ConfigHandler config;
    config.setIaEnabled(true);
    config.setIaShowDialog(true);
    config.setIaDefaultMode(QStringLiteral("ask"));
    config.setIaApiUrl(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions")
                         .arg(port));
    config.setIaApiToken(QStringLiteral("stub-token"));
    config.setIaModel(QStringLiteral("stub-model"));
    config.setIaCopyResult(false);

    return config.iaEnabled() && config.iaShowDialog() &&
           config.iaModel() == QLatin1String("stub-model");
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("flameshot"));
    QCoreApplication::setApplicationName(QStringLiteral("flameshot"));

    StubApiServer api;
    if (!api.listen(QHostAddress::LocalHost, 0)) {
        qCritical() << "cannot start the stub API server";
        return 2;
    }
    if (!configureStub(api.serverPort())) {
        qCritical() << "cannot configure the stubbed AI settings (config error "
                       "flag set?)";
        return 2;
    }

    QPixmap marked = markedSnapshot();
    if (marked.isNull()) {
        qCritical() << "cannot paint the synthetic capture";
        return 2;
    }

    scenarioCancelKeepsCapture(api);
    scenarioAcceptDismissesEditor();
    scenarioBackendUsesAnswerOnce(api);
    scenarioBackendPromptsWithoutBlocking(api);
    scenarioCancelledPromptReleasesTheProcess();

    if (g_failures == 0) {
        qInfo().noquote() << "all AI flow scenarios passed";
        return 0;
    }
    qCritical().noquote() << g_failures << "check(s) failed";
    return 1;
}
