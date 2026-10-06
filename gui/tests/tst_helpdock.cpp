#include "app/HelpDock.h"

#include "backend/Session.h"

#include <QLineEdit>
#include <QSignalSpy>
#include <QTextBrowser>
#include <QToolButton>
#include <QtTest>

using namespace revstudio;

namespace {
const QString mock = QStringLiteral(MOCK_RB_PATH);
constexpr int waitMs = 10000;
}

class TstHelpDock : public QObject
{
    Q_OBJECT

private slots:
    void showTopicRequestsHelpAndDisplaysTheResult()
    {
        Session session;
        HelpDock dock(&session);
        QSignalSpy ready(&session, &Session::ready);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        dock.showTopic(QStringLiteral("dnExponential"));

        auto* browser = dock.findChild<QTextBrowser*>(QStringLiteral("helpBrowser"));
        QTRY_VERIFY_WITH_TIMEOUT(browser->toPlainText().contains(QStringLiteral("exponential distribution")), waitMs);
        QCOMPARE(dock.findChild<QLineEdit*>(QStringLiteral("helpTopic"))->text(), QStringLiteral("dnExponential"));
    }

    void anUnknownTopicShowsANotFoundMessage()
    {
        Session session;
        HelpDock dock(&session);
        QSignalSpy ready(&session, &Session::ready);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        dock.showTopic(QStringLiteral("notARealTopic"));

        auto* browser = dock.findChild<QTextBrowser*>(QStringLiteral("helpBrowser"));
        QTRY_VERIFY_WITH_TIMEOUT(browser->toPlainText().contains(QStringLiteral("No help found")), waitMs);
    }

    // Ordinary browser-history semantics: Back/Forward are disabled until there is somewhere to go, navigating
    // to a new topic after going Back drops whatever was ahead, and repeating the current topic does not grow
    // the history.
    void backAndForwardNavigateHistory()
    {
        Session session;
        HelpDock dock(&session);
        QSignalSpy ready(&session, &Session::ready);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        auto* browser = dock.findChild<QTextBrowser*>(QStringLiteral("helpBrowser"));
        auto* backButton = dock.findChild<QToolButton*>(QStringLiteral("helpBack"));
        auto* forwardButton = dock.findChild<QToolButton*>(QStringLiteral("helpForward"));
        QVERIFY(!backButton->isEnabled());
        QVERIFY(!forwardButton->isEnabled());

        dock.showTopic(QString());                        // the index
        QTRY_VERIFY_WITH_TIMEOUT(browser->toPlainText().contains(QStringLiteral("Index")), waitMs);
        QVERIFY(!backButton->isEnabled());                 // only one entry so far

        dock.showTopic(QStringLiteral("dnExponential"));
        QTRY_VERIFY_WITH_TIMEOUT(browser->toPlainText().contains(QStringLiteral("exponential")), waitMs);
        QVERIFY(backButton->isEnabled());
        QVERIFY(!forwardButton->isEnabled());

        QTest::mouseClick(backButton, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(browser->toPlainText().contains(QStringLiteral("Index")), waitMs);
        QVERIFY(!backButton->isEnabled());
        QVERIFY(forwardButton->isEnabled());

        QTest::mouseClick(forwardButton, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(browser->toPlainText().contains(QStringLiteral("exponential")), waitMs);
        QVERIFY(!forwardButton->isEnabled());

        // Repeating the current topic must not duplicate it in the history.
        dock.showTopic(QStringLiteral("dnExponential"));
        QVERIFY(!forwardButton->isEnabled());
        QTest::mouseClick(backButton, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(browser->toPlainText().contains(QStringLiteral("Index")), waitMs);
        QVERIFY(!backButton->isEnabled());   // would still be enabled if the repeat above had pushed a duplicate
    }
};

QTEST_MAIN(TstHelpDock)
#include "tst_helpdock.moc"
