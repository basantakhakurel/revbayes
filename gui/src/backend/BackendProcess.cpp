#include "backend/BackendProcess.h"

namespace revstudio {

namespace {
constexpr int maxStderrTailLines = 50;
}

BackendProcess::BackendProcess(QObject* parent) : QObject(parent)
{
    // stdout is the protocol channel and stderr is a log: never merge them.
    process_.setProcessChannelMode(QProcess::SeparateChannels);

    connect(&process_, &QProcess::started, this, &BackendProcess::started);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &BackendProcess::drainStdout);
    connect(&process_, &QProcess::readyReadStandardError, this, &BackendProcess::drainStderr);

    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
    {
        if (error == QProcess::FailedToStart)
        {
            emit failedToStart(process_.errorString());
        }
    });

    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status)
    {
        // Whatever the backend wrote just before it exited must reach the listeners before finished() does.
        drainStdout();
        drainStderr();
        emit finished(exitCode, status == QProcess::CrashExit);
    });
}

BackendProcess::~BackendProcess()
{
    if (process_.state() != QProcess::NotRunning)
    {
        process_.kill();
        process_.waitForFinished(1000);
    }
}

void BackendProcess::start(const QString& program, const QStringList& arguments, const QString& workingDirectory)
{
    if (!workingDirectory.isEmpty())
    {
        process_.setWorkingDirectory(workingDirectory);
    }
    process_.start(program, arguments);
}

bool BackendProcess::isRunning() const
{
    return process_.state() != QProcess::NotRunning;
}

void BackendProcess::write(const QByteArray& data)
{
    if (process_.state() == QProcess::Running)
    {
        process_.write(data);
    }
}

void BackendProcess::closeInput()
{
    process_.closeWriteChannel();
}

void BackendProcess::kill()
{
    if (process_.state() != QProcess::NotRunning)
    {
        process_.kill();
    }
}

void BackendProcess::drainStdout()
{
    stdoutFramer_.append(process_.readAllStandardOutput());

    QByteArray line;
    while (stdoutFramer_.nextLine(&line))
    {
        emit lineReceived(line);
    }
    if (stdoutFramer_.takeOverflow())
    {
        emit overflowed();
    }
}

void BackendProcess::drainStderr()
{
    stderrFramer_.append(process_.readAllStandardError());

    QByteArray line;
    while (stderrFramer_.nextLine(&line))
    {
        const QString text = QString::fromUtf8(line);
        stderrTail_.append(text);
        while (stderrTail_.size() > maxStderrTailLines)
        {
            stderrTail_.removeFirst();
        }
        emit stderrLine(text);
    }
}

} // namespace revstudio
