#ifndef REVSTUDIO_SESSION_H
#define REVSTUDIO_SESSION_H

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace revstudio {

class BackendProcess;

/**
 * One conversation with a backend (`rb --server`): starts the process, performs the handshake, keeps the state
 * machine (Starting, Ready, Busy, Closing, Closed, Failed) and turns protocol events into signals.
 *
 * Phase 0 handles what the stub backend and the mock speak: hello, ping, submit (output, done, variables, errors),
 * interrupt and shutdown. Later phases add snapshots, inspection, completion and help without changing the
 * structure.
 */
class Session : public QObject
{
    Q_OBJECT

public:
    enum class State { NotStarted, Starting, Ready, Busy, Closing, Closed, Failed };
    Q_ENUM(State)

    explicit Session(QObject* parent = nullptr);
    ~Session() override;

    /** Start the backend: `<program> --server <extraArguments>`. Asynchronous; watch ready() and failed(). */
    void start(const QString& program, const QString& workingDirectory = QString(),
               const QStringList& extraArguments = QStringList());

    /** Send a statement. Returns the request id, or -1 if the session is not Ready. */
    qint64 submit(const QString& text);
    qint64 ping();
    qint64 interrupt();

    /** Graceful: ask the backend to shut down; closed() follows. Falls back to killing it after a grace period. */
    void shutdown();

    /** Hard stop. */
    void kill();

    State       state() const { return state_; }
    QString     serverVersion() const { return serverVersion_; }
    QString     cwd() const { return cwd_; }
    QStringList features() const { return features_; }
    QString     failureReason() const { return failureReason_; }
    QStringList stderrTail() const;

    void setHandshakeTimeoutMs(int ms) { handshakeTimeoutMs_ = ms; }
    void setShutdownGraceMs(int ms) { shutdownGraceMs_ = ms; }

signals:
    void stateChanged(revstudio::Session::State state);
    void ready(const QString& serverVersion);
    void output(const QString& stream, const QString& text, qint64 requestId);
    void submitFinished(qint64 requestId, const QString& status, const QJsonObject& doneEvent);
    void variablesChanged(const QJsonArray& rows, const QString& cwd);
    void pongReceived(qint64 requestId);
    void backendError(qint64 requestId, const QString& code, const QString& message);
    void backendStderr(const QString& text);
    void protocolWarning(const QString& text);
    void failed(const QString& reason);
    void closed(int exitCode);                          // exactly once, when the backend process is gone

private:
    void onStarted();
    void onLine(const QByteArray& line);
    void onFinished(int exitCode, bool crashed);
    void handleEvent(const QJsonObject& event);
    void setState(State state);
    void fail(const QString& reason);
    qint64 send(const QString& cmd, const QJsonObject& fields = QJsonObject());

    BackendProcess* process_ = nullptr;
    State           state_ = State::NotStarted;
    QTimer          handshakeTimer_;
    QTimer          shutdownTimer_;
    int             handshakeTimeoutMs_ = 10000;
    int             shutdownGraceMs_ = 3000;

    qint64  nextId_ = 1;
    qint64  helloId_ = -1;
    qint64  pendingSubmit_ = -1;
    int     malformedInARow_ = 0;
    bool    byeReceived_ = false;
    bool    closedEmitted_ = false;

    QString     serverVersion_;
    QString     cwd_;
    QStringList features_;
    QString     failureReason_;
};

} // namespace revstudio

Q_DECLARE_METATYPE(revstudio::Session::State)

#endif
