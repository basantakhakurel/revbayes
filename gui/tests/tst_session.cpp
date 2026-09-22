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
};

QTEST_MAIN(TstSession)
#include "tst_session.moc"
