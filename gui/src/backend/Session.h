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
 * Handles hello, ping, submit (output, done, variables, errors), interrupt, shutdown, ask/answer, quit,
 * on-demand snapshot (variables/functions/all), inspect, complete (Q3) and help (Q8).
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

    /** Answers a pending `ask` (see askRequested()). `askId` is the id that event carried, not a request id. */
    void answer(qint64 askId, bool value);

    /** On-demand snapshot ("variables", "functions" or "all"); Idle only, like inspect/complete/help. Returns
     *  -1 if not Ready. The automatic `variables` broadcast after every `submit` does not need this -- it
     *  already arrives via variablesChanged() -- but nothing refreshes `functions` on its own, and nothing
     *  populates either before the first submit, so G4's VariablesPanel calls this itself. */
    qint64 requestSnapshot(const QString& what = QStringLiteral("variables"));

    /** `structure(name, verbose=TRUE)`, captured instead of printed (see inspected()). Idle only. */
    qint64 requestInspect(const QString& name);

    /** Q3: completion candidates for `buffer` with the caret at `cursor` (a UTF-8 BYTE offset, per
     *  doc/server-protocol.md -- NOT a QString character index; a caller with a character position must convert
     *  it first). `cursor` defaults to the end of `buffer`. Idle only; see completionsReceived(). */
    qint64 requestComplete(const QString& buffer, int cursor = -1);

    /** Q8: the backend's help text for `topic` ("" for the general index). Idle only; see helpReceived(). */
    qint64 requestHelp(const QString& topic);

    /** Graceful: ask the backend to shut down; closed() follows. Falls back to killing it after a grace period. */
    void shutdown();

    /** Hard stop. */
    void kill();

    /** Kills the backend (if still running) and starts a new one with the same program/cwd/arguments as the last
     *  start() call -- the Q2 "kill and restart" fallback for when `interrupt` (sent, but not honoured: there is
     *  no core interrupt flag yet, C4) leaves a computation stuck. A no-op if start() was never called. Unlike a
     *  crash, this does not emit failed() -- see onFinished()'s own restarting_ check -- since the user asked for
     *  this, it is not a failure. */
    void restart();

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
    /** Reply to requestSnapshot("functions") or ("all") -- never an automatic broadcast, unlike variables. */
    void functionsChanged(const QJsonArray& rows);
    /** Reply to requestInspect(); `requestId` is that call's return value, for matching a reply to a request
     *  when the user may have since selected something else. */
    void inspected(qint64 requestId, const QString& name, const QString& text, bool truncated);
    /** Reply to requestComplete(). `replaceFrom` is also a UTF-8 byte offset into the `buffer` that was sent;
     *  `items` is `completions.items` verbatim (each a `{text, kind}` object -- see doc/server-protocol.md's
     *  "Completion item"), left as JSON rather than unpacked since ConsoleWidget is this signal's only
     *  consumer and reads `items[i].text` directly off it. */
    void completionsReceived(qint64 requestId, int replaceFrom, const QJsonArray& items);
    /** Reply to requestHelp(). `topic` is echoed back (not necessarily the current topic by the time this
     *  arrives, if the dock navigated again meanwhile -- the caller matches on `requestId`, not this). */
    void helpReceived(qint64 requestId, const QString& topic, bool found, const QString& text);
    void pongReceived(qint64 requestId);
    void backendError(qint64 requestId, const QString& code, const QString& message);
    void backendStderr(const QString& text);
    void protocolWarning(const QString& text);
    void failed(const QString& reason);
    void closed(int exitCode);                          // exactly once, when the backend process is gone

    /** A yes/no question raised mid-`submit` (for example redefining an existing function). The interpreter is
     *  blocked until answer(askId, ...) is called; `askId` is NOT a request id, it is its own namespace. */
    void askRequested(qint64 askId, const QString& question);
    /** Rev code called quit()/q(); `closed()` follows shortly, once the backend's own `bye` arrives. */
    void backendQuit(const QString& reason);

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
    bool    restarting_ = false;

    QString     serverVersion_;
    QString     cwd_;
    QStringList features_;
    QString     failureReason_;

    // Remembered from the last start(), so restart() knows how to start a new process the same way.
    QString     startProgram_;
    QString     startWorkingDirectory_;
    QStringList startExtraArguments_;
};

} // namespace revstudio

Q_DECLARE_METATYPE(revstudio::Session::State)

#endif
