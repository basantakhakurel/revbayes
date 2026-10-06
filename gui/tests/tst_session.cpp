#include "backend/Session.h"

#include <QSignalSpy>
#include <QtTest>

using namespace revstudio;

namespace {
const QString mock = QStringLiteral(MOCK_RB_PATH);
const QString transcriptDir = QStringLiteral(TRANSCRIPT_DIR);
constexpr int waitMs = 10000;
}

class TstSession : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<Session::State>();
    }

    void submitBeforeReadyIsRefused()
    {
        Session session;
        QCOMPARE(session.state(), Session::State::NotStarted);
        QCOMPARE(session.submit(QStringLiteral("1+1")), qint64(-1));
        QCOMPARE(session.ping(), qint64(-1));
        QCOMPARE(session.requestSnapshot(), qint64(-1));
        QCOMPARE(session.requestInspect(QStringLiteral("x")), qint64(-1));
    }

    void handshakeSubmitAndShutdown()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy pong(&session, &Session::pongReceived);
        QSignalSpy output(&session, &Session::output);
        QSignalSpy done(&session, &Session::submitFinished);
        QSignalSpy variables(&session, &Session::variablesChanged);
        QSignalSpy closed(&session, &Session::closed);

        session.start(mock, QDir::tempPath());
        QCOMPARE(session.state(), Session::State::Starting);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);
        QCOMPARE(session.state(), Session::State::Ready);
        QVERIFY(session.serverVersion().contains(QStringLiteral("mock")));
        QVERIFY(session.features().contains(QStringLiteral("mock")));
        QVERIFY(!session.cwd().isEmpty());

        const qint64 pingId = session.ping();
        QTRY_COMPARE_WITH_TIMEOUT(pong.count(), 1, waitMs);
        QCOMPARE(pong.first().at(0).toLongLong(), pingId);

        const qint64 id = session.submit(QStringLiteral("1+1"));
        QVERIFY(id > 0);
        QCOMPARE(session.state(), Session::State::Busy);
        QCOMPARE(session.submit(QStringLiteral("2+2")), qint64(-1));       // busy: refused

        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, waitMs);
        QCOMPARE(done.first().at(0).toLongLong(), id);
        QCOMPARE(done.first().at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(session.state(), Session::State::Ready);
        QCOMPARE(output.count(), 1);
        QCOMPARE(output.first().at(1).toString(), QStringLiteral("2\n"));
        QTRY_COMPARE_WITH_TIMEOUT(variables.count(), 1, waitMs);

        session.shutdown();
        QCOMPARE(session.state(), Session::State::Closing);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);
        QCOMPARE(closed.first().at(0).toInt(), 0);
        QCOMPARE(session.state(), Session::State::Closed);
    }

    void aBackendThatDoesNotImplementSubmitLeavesTheSessionUsable()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy errors(&session, &Session::backendError);

        session.start(mock, QString(), {QStringLiteral("--stub")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        const qint64 id = session.submit(QStringLiteral("1+1"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, waitMs);
        QCOMPARE(errors.first().at(0).toLongLong(), id);
        QCOMPARE(errors.first().at(1).toString(), QStringLiteral("not_implemented"));
        QCOMPARE(session.state(), Session::State::Ready);                  // no `done` follows an error; the session recovers
        session.kill();
    }

    void aCrashIsReportedWithTheExitCode()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy failed(&session, &Session::failed);
        QSignalSpy closed(&session, &Session::closed);

        session.start(mock, QString(), {QStringLiteral("--crash-after"), QStringLiteral("2")});   // hello, then ping, then gone
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);
        session.ping();

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, waitMs);
        QVERIFY2(failed.first().at(0).toString().contains(QStringLiteral("exit code 3")),
                 qPrintable(failed.first().at(0).toString()));
        QCOMPARE(session.state(), Session::State::Failed);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);
        QCOMPARE(closed.first().at(0).toInt(), 3);
    }

    void aSilentBackendTimesOutTheHandshake()
    {
        Session session;
        session.setHandshakeTimeoutMs(300);
        QSignalSpy failed(&session, &Session::failed);
        QSignalSpy closed(&session, &Session::closed);

        session.start(mock, QString(), {QStringLiteral("--hello-delay-ms"), QStringLiteral("5000")});
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, waitMs);
        QVERIFY2(failed.first().at(0).toString().contains(QStringLiteral("handshake")), qPrintable(failed.first().at(0).toString()));
        QCOMPARE(session.state(), Session::State::Failed);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);              // the slow backend was killed, not left running
    }

    void aProtocolMismatchFailsTheSession()
    {
        Session session;
        QSignalSpy failed(&session, &Session::failed);

        session.start(mock, QString(), {QStringLiteral("--hello-protocol"), QStringLiteral("2")});
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, waitMs);
        QVERIFY2(failed.first().at(0).toString().contains(QStringLiteral("protocol 2")), qPrintable(failed.first().at(0).toString()));
    }

    void oneMalformedMessageIsIgnored()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy warnings(&session, &Session::protocolWarning);
        QSignalSpy pong(&session, &Session::pongReceived);

        session.start(mock, QString(), {QStringLiteral("--malformed-after"), QStringLiteral("1")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);
        QTRY_VERIFY_WITH_TIMEOUT(warnings.count() >= 1, waitMs);
        QCOMPARE(session.state(), Session::State::Ready);
        session.ping();
        QTRY_COMPARE_WITH_TIMEOUT(pong.count(), 1, waitMs);                // still talking
        session.kill();
    }

    void threeMalformedMessagesInARowFailTheSession()
    {
        Session session;
        QSignalSpy failed(&session, &Session::failed);

        session.start(mock, QString(), {QStringLiteral("--malformed-after"), QStringLiteral("1"),
                                        QStringLiteral("--malformed-count"), QStringLiteral("3")});
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, waitMs);
        QVERIFY2(failed.first().at(0).toString().contains(QStringLiteral("malformed")), qPrintable(failed.first().at(0).toString()));
    }

    void aProgramThatCannotStartIsReported()
    {
        Session session;
        QSignalSpy failed(&session, &Session::failed);
        QSignalSpy closed(&session, &Session::closed);

        session.start(QStringLiteral("/definitely/not/here/rb"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, waitMs);
        QVERIFY2(failed.first().at(0).toString().contains(QStringLiteral("Could not start")), qPrintable(failed.first().at(0).toString()));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);
        QCOMPARE(closed.first().at(0).toInt(), -1);
    }

    void aTranscriptCanDriveAConversation()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy done(&session, &Session::submitFinished);
        QSignalSpy variables(&session, &Session::variablesChanged);
        QSignalSpy closed(&session, &Session::closed);

        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/basic.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);
        QCOMPARE(session.serverVersion(), QStringLiteral("RevBayes (transcript)"));

        session.submit(QStringLiteral("x <- 1.5"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, waitMs);
        QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("ok"));
        QTRY_COMPARE_WITH_TIMEOUT(variables.count(), 1, waitMs);
        const QJsonArray rows = variables.first().at(0).toJsonArray();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toObject().value("name").toString(), QStringLiteral("x"));

        session.submit(QStringLiteral("y <- x +"));                        // continuation
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, waitMs);
        QCOMPARE(done.at(1).at(1).toString(), QStringLiteral("incomplete"));
        QCOMPARE(session.state(), Session::State::Ready);

        session.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);
        QCOMPARE(closed.first().at(0).toInt(), 0);
    }

    void killStopsTheBackend()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy closed(&session, &Session::closed);

        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);
        session.kill();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);
        QCOMPARE(session.state(), Session::State::Failed);                 // killing is not a clean close
    }

    void restartBeforeAnyStartIsANoOp()
    {
        Session session;
        session.restart();
        QCOMPARE(session.state(), Session::State::NotStarted);
    }

    // Q2's "kill and restart" fallback, for when `interrupt` is not honoured (no core interrupt flag yet, C4):
    // unlike killStopsTheBackend just above, this kill was asked for, so it must not look like a crash -- the new
    // process has to come up Ready with no failed() in between, and actually be usable afterward.
    void restartReplacesTheBackendWithoutReportingFailure()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy closed(&session, &Session::closed);
        QSignalSpy failed(&session, &Session::failed);
        QSignalSpy done(&session, &Session::submitFinished);

        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        session.restart();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);   // the old process going away
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, waitMs);    // the new one's own hello
        QCOMPARE(session.state(), Session::State::Ready);
        QCOMPARE(failed.count(), 0);                            // a requested restart, not a crash

        QVERIFY(session.submit(QStringLiteral("1+1")) >= 0);    // the new session actually works
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, waitMs);
    }

    // D.4/D.5 of doc/server-protocol.md: a redefinition raises `ask`, which blocks the interpreter until
    // `answer` arrives; quit() raises `quit` then `bye`. See transcripts/ask_and_quit.jsonl for the scripted
    // exchange this drives.
    void askIsAnsweredAndQuitIsReported()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy ask(&session, &Session::askRequested);
        QSignalSpy done(&session, &Session::submitFinished);
        QSignalSpy quit(&session, &Session::backendQuit);
        QSignalSpy closed(&session, &Session::closed);

        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/ask_and_quit.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        const qint64 submitId = session.submit(QStringLiteral("function f(x) { return x + 1 }"));
        QTRY_COMPARE_WITH_TIMEOUT(ask.count(), 1, waitMs);
        const qint64 askId = ask.first().at(0).toLongLong();
        QCOMPARE(ask.first().at(1).toString(), QStringLiteral("Replace existing function with same signature"));
        QCOMPARE(session.state(), Session::State::Busy);                   // still mid-submit while the ask is pending

        session.answer(askId, true);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, waitMs);
        QCOMPARE(done.first().at(0).toLongLong(), submitId);
        QCOMPARE(done.first().at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(session.state(), Session::State::Ready);

        session.submit(QStringLiteral("quit()"));
        QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, waitMs);
        QCOMPARE(quit.first().at(0).toString(), QStringLiteral("quit()"));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);
        QCOMPARE(session.state(), Session::State::Closed);
    }

    // G4's VariablesPanel: an on-demand snapshot (what="all" gets both functions and variables) and inspect.
    void onDemandSnapshotAndInspect()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy variables(&session, &Session::variablesChanged);
        QSignalSpy functions(&session, &Session::functionsChanged);
        QSignalSpy inspected(&session, &Session::inspected);
        QSignalSpy closed(&session, &Session::closed);

        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/snapshot_and_inspect.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QVERIFY(session.requestSnapshot(QStringLiteral("all")) >= 0);
        QTRY_COMPARE_WITH_TIMEOUT(functions.count(), 1, waitMs);
        QTRY_COMPARE_WITH_TIMEOUT(variables.count(), 1, waitMs);
        QCOMPARE(functions.first().at(0).toJsonArray().size(), 1);
        QCOMPARE(variables.first().at(0).toJsonArray().size(), 1);

        const qint64 inspectId = session.requestInspect(QStringLiteral("x"));
        QVERIFY(inspectId >= 0);
        QTRY_COMPARE_WITH_TIMEOUT(inspected.count(), 1, waitMs);
        QCOMPARE(inspected.first().at(0).toLongLong(), inspectId);
        QCOMPARE(inspected.first().at(1).toString(), QStringLiteral("x"));
        QVERIFY(inspected.first().at(2).toString().contains(QStringLiteral("1.5")));
        QCOMPARE(inspected.first().at(3).toBool(), false);

        session.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, waitMs);
    }

    // Q3: the same "mu ~ dnExp" -> "mu ~ dnExponential"/"mu ~ dnExponentialError" example doc/server-protocol.md
    // itself uses, which mock-rb's own `complete` handler reproduces for exactly this reason.
    void requestCompleteReturnsCompletionsFromTheMock()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy completions(&session, &Session::completionsReceived);

        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        const qint64 id = session.requestComplete(QStringLiteral("mu ~ dnExp"));
        QVERIFY(id >= 0);
        QTRY_COMPARE_WITH_TIMEOUT(completions.count(), 1, waitMs);
        QCOMPARE(completions.first().at(0).toLongLong(), id);
        QCOMPARE(completions.first().at(1).toInt(), 5);   // "mu ~ " is 5 bytes; "dnExp" is the prefix being completed

        const QJsonArray items = completions.first().at(2).toJsonArray();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items.at(0).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("mu ~ dnExponential"));
        QCOMPARE(items.at(1).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("mu ~ dnExponentialError"));
    }

    // Q8: the mock's own deterministic stand-in (see mock_rb.cpp's "help" handler) covers the index, a known
    // topic and an unknown one.
    void requestHelpReturnsTextForKnownTopicsAndFoundFalseOtherwise()
    {
        Session session;
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy help(&session, &Session::helpReceived);

        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        const qint64 indexId = session.requestHelp(QString());
        QTRY_COMPARE_WITH_TIMEOUT(help.count(), 1, waitMs);
        QCOMPARE(help.last().at(0).toLongLong(), indexId);
        QCOMPARE(help.last().at(2).toBool(), true);
        QVERIFY(help.last().at(3).toString().contains(QStringLiteral("Index")));

        session.requestHelp(QStringLiteral("dnExponential"));
        QTRY_COMPARE_WITH_TIMEOUT(help.count(), 2, waitMs);
        QCOMPARE(help.last().at(1).toString(), QStringLiteral("dnExponential"));
        QCOMPARE(help.last().at(2).toBool(), true);
        QVERIFY(help.last().at(3).toString().contains(QStringLiteral("exponential distribution")));

        session.requestHelp(QStringLiteral("notARealTopic"));
        QTRY_COMPARE_WITH_TIMEOUT(help.count(), 3, waitMs);
        QCOMPARE(help.last().at(2).toBool(), false);
    }
};

QTEST_MAIN(TstSession)
#include "tst_session.moc"
