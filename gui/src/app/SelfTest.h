#ifndef REVSTUDIO_SELFTEST_H
#define REVSTUDIO_SELFTEST_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace revstudio {

class Session;

/**
 * `RevStudio --selftest [--rb PATH] [--report FILE]`: drives the real start-up path against a backend and reports.
 *
 *   icon      the embedded SVG icon renders (checks the Qt resource, the SVG image-format plugin and the icon engine:
 *             the parts of a deployed bundle that fail silently when a plugin is missing)
 *   locate    find the backend (--rb wins)
 *   probe     `rb --server-info` answers and speaks the protocol this GUI needs
 *   handshake start the session and wait for `hello`
 *   ping      `ping` is answered with `pong`
 *   submit    `1+1` yields output containing 2 and `done ok`, then a `variables` event.
 *             SKIPPED when the backend answers `not_implemented` (the phase 0 stub)
 *   shutdown  the backend exits cleanly with code 0
 *
 * Exit code 0 on success, 1 on any failure. The whole run has a timeout. It is the same code path a user takes at
 * start-up, which is what makes it a meaningful smoke test of a deployed bundle (GUI_Implementation_Note.md, F.9).
 */
class SelfTest : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        QString rbPath;                     // --rb
        QString reportPath;                 // --report: the same text is also written to this file
        QString workingDirectory;
        int     timeoutMs = 30000;
    };

    SelfTest(Session* session, const Options& options, QObject* parent = nullptr);

    void start();

signals:
    void finished(int exitCode);

private:
    enum class Step { Icon, Locate, Probe, Handshake, Ping, Submit, Variables, Shutdown, Done };

    void report(const QString& step, const QString& result);
    void failStep(const QString& detail);
    void startShutdown();
    void finish(bool ok);
    static QString stepName(Step step);

    Session*    session_;
    Options     options_;
    Step        step_ = Step::Locate;
    bool        finished_ = false;
    QString     collectedOutput_;
    QStringList lines_;
    QTimer      overallTimer_;
    QTimer      variablesTimer_;
};

} // namespace revstudio

#endif
