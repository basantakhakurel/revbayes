#include "backend/Session.h"

#include "backend/BackendProcess.h"
#include "backend/ProtocolCodec.h"
#include "revstudio_version.h"

namespace revstudio {

namespace {
constexpr int maxMalformedInARow = 3;
}

Session::Session(QObject* parent) : QObject(parent)
{
    handshakeTimer_.setSingleShot(true);
    shutdownTimer_.setSingleShot(true);

    connect(&handshakeTimer_, &QTimer::timeout, this, [this]
    {
        fail(tr("The backend did not answer the handshake within %1 seconds.").arg(handshakeTimeoutMs_ / 1000.0));
    });
    connect(&shutdownTimer_, &QTimer::timeout, this, [this]
    {
        // It ignored the polite request: stop it the hard way. onFinished() then reports the process as closed.
        if (process_)
        {
            process_->kill();
        }
    });
}

Session::~Session() = default;      // the BackendProcess is a child object; its destructor kills a running process

void Session::start(const QString& program, const QString& workingDirectory, const QStringList& extraArguments)
{
    if (state_ != State::NotStarted)
    {
        return;
    }

    process_ = new BackendProcess(this);
    connect(process_, &BackendProcess::started, this, &Session::onStarted);
    connect(process_, &BackendProcess::lineReceived, this, &Session::onLine);
    connect(process_, &BackendProcess::stderrLine, this, &Session::backendStderr);
    connect(process_, &BackendProcess::overflowed, this, [this]
    {
        emit protocolWarning(tr("A protocol line over the size limit was dropped."));
    });
    connect(process_, &BackendProcess::failedToStart, this, [this](const QString& reason)
    {
        fail(tr("Could not start the backend: %1").arg(reason));
        if (!closedEmitted_)
        {
            closedEmitted_ = true;
            emit closed(-1);
        }
    });
    connect(process_, &BackendProcess::finished, this, &Session::onFinished);

    setState(State::Starting);
    handshakeTimer_.start(handshakeTimeoutMs_);

    QStringList arguments{QStringLiteral("--server")};
    arguments += extraArguments;
    process_->start(program, arguments, workingDirectory);
}

qint64 Session::submit(const QString& text)
{
    if (state_ != State::Ready)
    {
        return -1;
    }
    const qint64 id = send(QStringLiteral("submit"), QJsonObject{{QStringLiteral("text"), text}});
    pendingSubmit_ = id;
    setState(State::Busy);
    return id;
}

qint64 Session::ping()
{
    if (state_ != State::Ready && state_ != State::Busy)
    {
        return -1;
    }
    return send(QStringLiteral("ping"));
}

qint64 Session::interrupt()
{
    if (state_ != State::Ready && state_ != State::Busy)
    {
        return -1;
    }
    return send(QStringLiteral("interrupt"));
}

void Session::shutdown()
{
    switch (state_)
    {
    case State::Ready:
    case State::Busy:
        setState(State::Closing);
        send(QStringLiteral("shutdown"));
        shutdownTimer_.start(shutdownGraceMs_);
        break;
    case State::Starting:
        kill();
        break;
    default:
        break;
    }
}

void Session::kill()
{
    if (process_)
    {
        process_->kill();
    }
}

QStringList Session::stderrTail() const
{
    return process_ ? process_->stderrTail() : QStringList();
}

void Session::onStarted()
{
    helloId_ = send(QStringLiteral("hello"),
                    QJsonObject{{QStringLiteral("protocol"), protocol::version},
                                {QStringLiteral("client"), QStringLiteral("RevStudio " REVSTUDIO_VERSION)}});
}

void Session::onLine(const QByteArray& line)
{
    QString error;
    const auto event = protocol::parseMessage(line, &error);
    if (!event)
    {
        emit protocolWarning(tr("Ignored a malformed message from the backend (%1).").arg(error));
        if (++malformedInARow_ >= maxMalformedInARow)
        {
            fail(tr("The backend sent %1 malformed messages in a row.").arg(maxMalformedInARow));
        }
        return;
    }
    malformedInARow_ = 0;
    handleEvent(*event);
}

void Session::handleEvent(const QJsonObject& event)
{
    const QString ev = event.value(QStringLiteral("ev")).toString();
    const qint64  re = event.contains(QStringLiteral("re")) ? event.value(QStringLiteral("re")).toInteger(-1) : -1;

    if (ev == QLatin1String("hello"))
    {
        if (state_ != State::Starting)
        {
            return;
        }
        const int protocolVersion = event.value(QStringLiteral("protocol")).toInt(0);
        if (protocolVersion != protocol::version)
        {
            fail(tr("The backend speaks protocol %1, but this RevStudio needs protocol %2.")
                     .arg(protocolVersion).arg(protocol::version));
            return;
        }
        serverVersion_ = event.value(QStringLiteral("server")).toString();
        cwd_           = event.value(QStringLiteral("cwd")).toString();
        features_.clear();
        const QJsonArray features = event.value(QStringLiteral("features")).toArray();
        for (const QJsonValue& feature : features)
        {
            features_.append(feature.toString());
        }
        handshakeTimer_.stop();
        setState(State::Ready);
        emit ready(serverVersion_);
    }
    else if (ev == QLatin1String("output"))
    {
        emit output(event.value(QStringLiteral("stream")).toString(), event.value(QStringLiteral("text")).toString(),
                    event.contains(QStringLiteral("id")) ? event.value(QStringLiteral("id")).toInteger(-1) : -1);
    }
    else if (ev == QLatin1String("done"))
    {
        if (re == pendingSubmit_)
        {
            pendingSubmit_ = -1;
            if (state_ == State::Busy)
            {
                setState(State::Ready);
            }
        }
        if (event.contains(QStringLiteral("cwd")))
        {
            cwd_ = event.value(QStringLiteral("cwd")).toString();
        }
        emit submitFinished(re, event.value(QStringLiteral("status")).toString(), event);
    }
    else if (ev == QLatin1String("variables"))
    {
        emit variablesChanged(event.value(QStringLiteral("rows")).toArray(), event.value(QStringLiteral("cwd")).toString());
    }
    else if (ev == QLatin1String("pong"))
    {
        emit pongReceived(re);
    }
    else if (ev == QLatin1String("error"))
    {
        const QString code    = event.value(QStringLiteral("code")).toString();
        const QString message = event.value(QStringLiteral("message")).toString();

        if (re == helloId_ && state_ == State::Starting)
        {
            fail(tr("The backend refused the handshake (%1): %2").arg(code, message));
            return;
        }
        if (re == pendingSubmit_)
        {
            pendingSubmit_ = -1;                // no `done` follows an error
            if (state_ == State::Busy)
            {
                setState(State::Ready);
            }
        }
        emit backendError(re, code, message);
    }
    else if (ev == QLatin1String("bye"))
    {
        byeReceived_ = true;
    }
    // Unknown events are ignored on purpose: newer backends may send events this GUI does not know (forward compatibility).
}

void Session::onFinished(int exitCode, bool crashed)
{
    handshakeTimer_.stop();
    shutdownTimer_.stop();

    if (state_ == State::Closing || (byeReceived_ && !crashed && exitCode == 0))
    {
        setState(State::Closed);
    }
    else if (state_ != State::Failed)
    {
        QString reason = crashed ? tr("The backend crashed.") : tr("The backend exited unexpectedly (exit code %1).").arg(exitCode);
        const QStringList tail = stderrTail();
        if (!tail.isEmpty())
        {
            reason += tr("\nLast messages from the backend:\n") + tail.join(QLatin1Char('\n'));
        }
        fail(reason);
    }

    if (!closedEmitted_)
    {
        closedEmitted_ = true;
        emit closed(exitCode);
    }
}

void Session::setState(State state)
{
    if (state_ != state)
    {
        state_ = state;
        emit stateChanged(state);
    }
}

void Session::fail(const QString& reason)
{
    if (state_ == State::Failed || state_ == State::Closed)
    {
        return;
    }
    handshakeTimer_.stop();
    shutdownTimer_.stop();
    failureReason_ = reason;
    setState(State::Failed);
    if (process_ && process_->isRunning())
    {
        process_->kill();
    }
    emit failed(reason);
}

qint64 Session::send(const QString& cmd, const QJsonObject& fields)
{
    const qint64 id = nextId_++;
    if (process_)
    {
        process_->write(protocol::encodeRequest(cmd, id, fields));
    }
    return id;
}

} // namespace revstudio
