#include "app/ConsoleWidget.h"

#include "backend/Session.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QDropEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QDir>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QtTest>

using namespace revstudio;

namespace {
const QString mock = QStringLiteral(MOCK_RB_PATH);
const QString transcriptDir = QStringLiteral(TRANSCRIPT_DIR);
constexpr int waitMs = 10000;

QPlainTextEdit* findOutput(const ConsoleWidget& widget)
{
    return widget.findChild<QPlainTextEdit*>(QStringLiteral("consoleOutput"));
}

QPlainTextEdit* findInput(const ConsoleWidget& widget)
{
    return widget.findChild<QPlainTextEdit*>(QStringLiteral("consoleInput"));
}

/** Clicks `button` on the QMessageBox that ConsoleWidget's askRequested handler opens. QMessageBox::question()
 *  blocks inside its own nested event loop, so this must be scheduled (QTimer::singleShot(0, ...)) BEFORE
 *  whatever triggers the dialog, so it runs once that nested loop is spinning -- the usual way to test a modal
 *  dialog without an OS-level input-automation tool (none is available in this environment: no xdotool/ydotool).
 */
void clickPendingMessageBox(QMessageBox::StandardButton which, int withinMs = waitMs)
{
    const auto deadline = QDeadlineTimer(withinMs);
    QTimer* poll = new QTimer;
    poll->setInterval(10);
    QObject::connect(poll, &QTimer::timeout, poll, [poll, which, deadline]
    {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
        {
            poll->stop();
            poll->deleteLater();
            QTest::mouseClick(box->button(which), Qt::LeftButton);
            return;
        }
        if (deadline.hasExpired())
        {
            poll->stop();
            poll->deleteLater();
        }
    });
    poll->start();
}

} // namespace

class TstConsoleWidget : public QObject
{
    Q_OBJECT

private slots:
    void submitEchoesTheInputAndShowsTheResult()
    {
        Session session;
        ConsoleWidget widget(&session);
        QSignalSpy ready(&session, &Session::ready);

        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QPlainTextEdit* input = findInput(widget);
        QVERIFY(input);
        QVERIFY(input->isEnabled());

        input->setFocus();
        QTest::keyClicks(input, QStringLiteral("1+1"));
        QTest::keyClick(input, Qt::Key_Return);

        QPlainTextEdit* output = findOutput(widget);
        QVERIFY(output);
        QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("> 1+1")), waitMs);
        QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("2")), waitMs);
        QVERIFY(input->toPlainText().isEmpty());   // cleared after submit
    }

    void askOpensAModalDialogAndTheAnswerReachesTheBackend()
    {
        Session session;
        ConsoleWidget widget(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy done(&session, &Session::submitFinished);
        QSignalSpy quit(&session, &Session::backendQuit);

        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/ask_and_quit.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QPlainTextEdit* input = findInput(widget);
        input->setFocus();

        clickPendingMessageBox(QMessageBox::Yes);
        QTest::keyClicks(input, QStringLiteral("function f(x) { return x + 1 }"));
        QTest::keyClick(input, Qt::Key_Return);

        // askRequested's handler blocks (QMessageBox::question's own exec()) until clickPendingMessageBox's
        // timer fires and clicks Yes, at which point Session::answer() is called and the transcript's `done`
        // follows -- so reaching here at all is most of what this test is checking.
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, waitMs);
        QCOMPARE(done.first().at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(session.state(), Session::State::Ready);

        session.submit(QStringLiteral("quit()"));
        QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, waitMs);
    }

    void clearIsHandledLocallyAndNeverReachesTheBackend()
    {
        Session session;
        ConsoleWidget widget(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy backendError(&session, &Session::backendError);

        // --stub answers submit with not_implemented; if "clear" were actually sent, backendError would fire.
        session.start(mock, QString(), {QStringLiteral("--stub")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QPlainTextEdit* output = findOutput(widget);
        output->appendPlainText(QStringLiteral("some previous output"));
        QVERIFY(!output->toPlainText().isEmpty());

        QPlainTextEdit* input = findInput(widget);
        input->setFocus();
        QTest::keyClicks(input, QStringLiteral("clear"));
        QTest::keyClick(input, Qt::Key_Return);

        QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().isEmpty(), waitMs);
        QCOMPARE(backendError.count(), 0);
    }

    void upArrowFillsThePreviousSubmission()
    {
        Session session;
        ConsoleWidget widget(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy done(&session, &Session::submitFinished);

        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QPlainTextEdit* input = findInput(widget);
        input->setFocus();
        QTest::keyClicks(input, QStringLiteral("1+1"));
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, waitMs);
        QVERIFY(input->toPlainText().isEmpty());

        QTest::keyClick(input, Qt::Key_Up);
        QCOMPARE(input->toPlainText(), QStringLiteral("1+1"));
    }

    void insertTextLeavesTheCursorRightAfterWhatWasInserted()
    {
        Session session;
        ConsoleWidget widget(&session);
        QPlainTextEdit* input = findInput(widget);

        widget.insertText(QStringLiteral("x"));
        widget.insertText(QStringLiteral("y"));   // if the cursor did not move after the first insert, this
                                                   // would land before "x" instead of after it ("yx", not "xy")
        QCOMPARE(input->toPlainText(), QStringLiteral("xy"));
        QCOMPARE(input->textCursor().position(), 2);
    }

    void focusReturnsToTheInputAfterASubmitCompletes()
    {
        Session session;
        ConsoleWidget widget(&session);
        widget.resize(400, 300);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy done(&session, &Session::submitFinished);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QPlainTextEdit* input = findInput(widget);
        input->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(input->hasFocus(), waitMs);

        QTest::keyClicks(input, QStringLiteral("1+1"));
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, waitMs);

        // Busy disables the input (correctly -- it must not accept typing mid-submit), but Qt moves focus away
        // from a widget the moment it is disabled and never puts it back on its own; nothing before this fix
        // gave it back once the input re-enabled, so the console silently stopped accepting Enter/typing after
        // the very first command without the user doing anything to deserve that.
        QTRY_VERIFY_WITH_TIMEOUT(input->hasFocus(), waitMs);
    }

    // A submission that did not come from this console's own input (G6: EditorTabs::runSelectionOrLine()/
    // runCurrentFile() via Ctrl+Enter/F5, routed through the same submitText() used here) must not steal focus
    // away from wherever it actually is once the backend goes back to Ready -- only submitCurrentInput() (the
    // console's own Enter handling) should do that, which focusReturnsToTheInputAfterASubmitCompletes covers.
    void submittingFromOutsideTheConsoleDoesNotStealFocusOnCompletion()
    {
        Session session;
        ConsoleWidget widget(&session);
        widget.resize(400, 300);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QLineEdit elsewhere;
        elsewhere.show();
        QVERIFY(QTest::qWaitForWindowExposed(&elsewhere));
        elsewhere.setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(elsewhere.hasFocus(), waitMs);

        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy done(&session, &Session::submitFinished);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QPlainTextEdit* input = findInput(widget);
        QVERIFY(!input->hasFocus());

        widget.submitText(QStringLiteral("1+1"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, waitMs);

        QVERIFY(elsewhere.hasFocus());
        QVERIFY(!input->hasFocus());
    }

    void requestStopIsANoOpWhenNotBusy()
    {
        Session session;
        ConsoleWidget widget(&session);
        QSignalSpy ready(&session, &Session::ready);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        widget.requestStop();

        auto* backendLog = widget.findChild<QPlainTextEdit*>(QStringLiteral("consoleBackendLog"));
        QVERIFY(backendLog);
        QVERIFY(!backendLog->toPlainText().contains(QStringLiteral("Stop requested")));
        QCOMPARE(session.state(), Session::State::Ready);
    }

    // Ctrl+C/Stop (GUI_Implementation_Note.md's Q2, pulled forward): `interrupt` only ever gets an acknowledgement
    // today (no core interrupt flag, C4) -- this fixture's backend takes the submit and then, deliberately, never
    // replies to it at all, so Session stays Busy forever, exactly like a real stuck MCMC run would. Stop has to
    // fall back to kill-and-restart for that case to ever actually un-stick the GUI.
    void requestStopOffersKillAndRestartWhenTheBackendNeverResponds()
    {
        Session session;
        ConsoleWidget widget(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy failed(&session, &Session::failed);
        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/stuck_submit.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        widget.submitText(QStringLiteral("run_forever()"));
        QCOMPARE(session.state(), Session::State::Busy);

        widget.setStopTimeoutMs(50);
        clickPendingMessageBox(QMessageBox::Yes);
        widget.requestStop();

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, waitMs);   // the restarted backend's own hello
        QCOMPARE(session.state(), Session::State::Ready);
        QCOMPARE(failed.count(), 0);                           // a requested restart, not a crash

        auto* backendLog = widget.findChild<QPlainTextEdit*>(QStringLiteral("consoleBackendLog"));
        QVERIFY(backendLog->toPlainText().contains(QStringLiteral("Stop requested")));
        QVERIFY(backendLog->toPlainText().contains(QStringLiteral("Killing and restarting")));
    }

    // GUI_Implementation_Note.md 6.7: "Dragging a file into the console... inserts its quoted path."
    void droppingAFileUrlInsertsItsQuotedPath()
    {
        Session session;
        ConsoleWidget widget(&session);
        widget.resize(400, 300);
        widget.show();

        QSignalSpy ready(&session, &Session::ready);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);   // input is disabled (and ignores drops) until Ready

        QPlainTextEdit* input = findInput(widget);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/a file.Rev"))});

        // QPlainTextEdit (a QAbstractScrollArea) handles drag/drop on its viewport, not the outer widget.
        QDragEnterEvent enterEvent(QPoint(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(input->viewport(), &enterEvent);

        QDropEvent dropEvent(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(input->viewport(), &dropEvent);

        QCOMPARE(input->toPlainText(), QStringLiteral("\"/tmp/a file.Rev\""));
    }

    // Q3, pulled forward from phase 3: a single match has nothing to choose between, so Tab inserts it directly
    // without ever showing the completer's popup.
    void tabWithASingleMatchInsertsItDirectly()
    {
        Session session;
        ConsoleWidget widget(&session);
        widget.resize(400, 300);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QSignalSpy ready(&session, &Session::ready);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QPlainTextEdit* input = findInput(widget);
        input->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(input->hasFocus(), waitMs);
        QTest::keyClicks(input, QStringLiteral("f <- uniq"));

        QTest::keyClick(input, Qt::Key_Tab);
        QTRY_COMPARE_WITH_TIMEOUT(input->toPlainText(), QStringLiteral("f <- uniqueFunctionName"), waitMs);

        auto* completer = widget.findChild<QCompleter*>();
        QVERIFY(completer);
        QVERIFY(!completer->popup()->isVisible());
    }

    // Several matches: Tab shows the completer's popup (anchored at the caret) with both candidates, rather
    // than auto-inserting one. Accepting one via the popup's own keyboard navigation is NOT exercised here: the
    // offscreen platform used for headless tests does not support keyboard grabbing, which the popup relies on,
    // so that path is flaky in exactly this environment and nowhere else -- insertCompletion() itself (the same
    // code either way) is already fully covered by tabWithASingleMatchInsertsItDirectly.
    void tabWithSeveralMatchesShowsThePopupWithBothCandidates()
    {
        Session session;
        ConsoleWidget widget(&session);
        widget.resize(400, 300);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QSignalSpy ready(&session, &Session::ready);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QPlainTextEdit* input = findInput(widget);
        input->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(input->hasFocus(), waitMs);
        QTest::keyClicks(input, QStringLiteral("mu ~ dnExp"));

        QTest::keyClick(input, Qt::Key_Tab);

        auto* completer = widget.findChild<QCompleter*>();
        QVERIFY(completer);
        QTRY_VERIFY_WITH_TIMEOUT(completer->popup()->isVisible(), waitMs);
        QCOMPARE(completer->completionModel()->rowCount(), 2);
        QCOMPARE(completer->completionModel()->index(0, 0).data().toString(), QStringLiteral("mu ~ dnExponential"));
        QCOMPARE(completer->completionModel()->index(1, 0).data().toString(), QStringLiteral("mu ~ dnExponentialError"));
        QCOMPARE(input->toPlainText(), QStringLiteral("mu ~ dnExp"));   // untouched until something is actually accepted
    }

    // Q3's signature tooltip: it reads from whatever functions snapshot comes in on Session::functionsChanged,
    // not a request this class makes itself (see the class comment on why) -- so the test plays VariablesPanel's
    // part and asks for one, exactly as the real app's own VariablesPanel already does on ready().
    void typingOpenParenAfterAKnownFunctionShowsItsSignature()
    {
        Session session;
        ConsoleWidget widget(&session);
        widget.resize(400, 300);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy functionsChanged(&session, &Session::functionsChanged);
        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/snapshot_and_inspect.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        session.requestSnapshot(QStringLiteral("all"));   // "f(x)" -- see snapshot_and_inspect.jsonl
        QTRY_COMPARE_WITH_TIMEOUT(functionsChanged.count(), 1, waitMs);

        QPlainTextEdit* input = findInput(widget);
        input->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(input->hasFocus(), waitMs);
        QTest::keyClicks(input, QStringLiteral("f("));

        QTRY_VERIFY_WITH_TIMEOUT(QToolTip::isVisible(), waitMs);
        QVERIFY(QToolTip::text().contains(QStringLiteral("RevObject f(x)")));

        QTest::keyClicks(input, QStringLiteral("1)"));   // no longer right after an open paren
        QTRY_VERIFY_WITH_TIMEOUT(!QToolTip::isVisible(), waitMs);
    }

    // Q7's error navigation: RevClient.cpp's "Problem processing line N in file F" (F already quoted and
    // escaped by std::filesystem::path's own operator<<) becomes a real link wherever it lands in the output,
    // and clicking it reports the unescaped path and line via errorLinkActivated().
    void errorMessagesBecomeClickableLinksInTheOutput()
    {
        Session session;
        ConsoleWidget widget(&session);
        widget.resize(500, 300);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy activated(&widget, &ConsoleWidget::errorLinkActivated);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        widget.submitText(QStringLiteral("Problem processing line 7 in file \"/tmp/script.Rev\""));
        QTest::qWait(200);   // let the 16 ms flush timer coalesce the mock's echoed-back output

        QPlainTextEdit* output = findOutput(widget);
        QTextCursor found = output->document()->find(QStringLiteral("Problem processing"));
        QVERIFY(!found.isNull());

        QTextCursor probe(output->document());
        probe.setPosition(found.selectionStart());
        probe.setPosition(found.selectionStart() + 1, QTextCursor::KeepAnchor);
        QVERIFY(probe.charFormat().isAnchor());
        QCOMPARE(probe.charFormat().anchorHref(), QStringLiteral("/tmp/script.Rev"));

        QTextCursor clickTarget(output->document());
        clickTarget.setPosition(found.selectionStart() + 1);
        QTest::mouseClick(output->viewport(), Qt::LeftButton, Qt::NoModifier, output->cursorRect(clickTarget).center());

        QTRY_COMPARE_WITH_TIMEOUT(activated.count(), 1, waitMs);
        QCOMPARE(activated.first().at(0).toString(), QStringLiteral("/tmp/script.Rev"));
        QCOMPARE(activated.first().at(1).toInt(), 7);
    }

    void relativePathsInErrorLinksResolveAgainstTheSessionCwd()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        Session session;
        ConsoleWidget widget(&session);
        widget.resize(500, 300);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy activated(&widget, &ConsoleWidget::errorLinkActivated);
        session.start(mock, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        widget.submitText(QStringLiteral("Problem processing line 3 in file \"script.Rev\""));
        QTest::qWait(200);

        QPlainTextEdit* output = findOutput(widget);
        QTextCursor found = output->document()->find(QStringLiteral("Problem processing"));
        QVERIFY(!found.isNull());
        QTextCursor clickTarget(output->document());
        clickTarget.setPosition(found.selectionStart() + 1);
        QTest::mouseClick(output->viewport(), Qt::LeftButton, Qt::NoModifier, output->cursorRect(clickTarget).center());

        QTRY_COMPARE_WITH_TIMEOUT(activated.count(), 1, waitMs);
        QCOMPARE(activated.first().at(0).toString(), QDir(dir.path()).filePath(QStringLiteral("script.Rev")));
    }

    void tabIsANoOpWhileBusy()
    {
        Session session;
        ConsoleWidget widget(&session);
        widget.resize(400, 300);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QSignalSpy ready(&session, &Session::ready);
        session.start(mock, QString(), {QStringLiteral("--transcript"), transcriptDir + QStringLiteral("/stuck_submit.jsonl")});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        widget.submitText(QStringLiteral("run_forever()"));
        QCOMPARE(session.state(), Session::State::Busy);

        QPlainTextEdit* input = findInput(widget);
        QTest::keyClick(input, Qt::Key_Tab);   // "Idle only" -- consumed quietly, no literal tab inserted either

        auto* completer = widget.findChild<QCompleter*>();
        QVERIFY(!completer->popup()->isVisible());
        QVERIFY(!input->toPlainText().contains(QLatin1Char('\t')));
    }
};

QTEST_MAIN(TstConsoleWidget)
#include "tst_consolewidget.moc"
