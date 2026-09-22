#ifndef REVSTUDIO_BACKENDPROCESS_H
#define REVSTUDIO_BACKENDPROCESS_H

#include "backend/ProtocolCodec.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace revstudio {

/**
 * Owns the backend process. stdout carries the protocol (framed into lines), stderr carries diagnostics and stray
 * output. Knows nothing about the meaning of the messages; that is Session's job.
 */
class BackendProcess : public QObject
{
    Q_OBJECT

public:
    explicit BackendProcess(QObject* parent = nullptr);
    ~BackendProcess() override;

    void start(const QString& program, const QStringList& arguments, const QString& workingDirectory = QString());
    bool isRunning() const;

    /** Write one already-encoded request line. Ignored if the process is not running. */
    void write(const QByteArray& data);

    /** Close the backend's stdin: it sees end of input, which a well-behaved backend treats as "shut down". */
    void closeInput();

    /** Hard stop (TerminateProcess / SIGKILL). The finished() signal follows. */
    void kill();

    /** The last lines the backend wrote to stderr (up to 50), oldest first. */
    QStringList stderrTail() const { return stderrTail_; }

signals:
    void started();
    void lineReceived(const QByteArray& line);
    void stderrLine(const QString& text);
    void overflowed();                                  // a protocol line exceeded the size limit and was dropped
    void failedToStart(const QString& reason);
    void finished(int exitCode, bool crashed);

private:
    void drainStdout();
    void drainStderr();

    QProcess    process_;
    LineFramer  stdoutFramer_;
    LineFramer  stderrFramer_;
    QStringList stderrTail_;
};

} // namespace revstudio

#endif
