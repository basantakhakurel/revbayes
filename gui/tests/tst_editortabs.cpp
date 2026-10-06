#include "app/EditorTabs.h"

#include "app/ConsoleWidget.h"
#include "app/ScriptEditor.h"
#include "app/Settings.h"
#include "backend/Session.h"

#include <QFile>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTimer>
#include <QtTest>

using namespace revstudio;

namespace {
const QString mock = QStringLiteral(MOCK_RB_PATH);
constexpr int waitMs = 10000;

/** Same technique as tst_consolewidget.cpp's modal-QMessageBox test: schedule this before whatever triggers the
 *  dialog, since QMessageBox::question() blocks inside its own nested event loop. */
void clickPendingMessageBox(QMessageBox::StandardButton which, int withinMs = waitMs)
{
    const auto deadline = QDeadlineTimer(withinMs);
    auto* poll = new QTimer;
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

class TstEditorTabs : public QObject
{
    Q_OBJECT

private slots:
    void newDocumentAddsATitledUntitledTab()
    {
        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);   // never touched; no file I/O needed
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);

        ScriptEditor* editor = tabs.newDocument();
        QVERIFY(editor);
        QVERIFY(editor->isUntitled());
        QCOMPARE(tabs.count(), 1);
        QVERIFY(tabs.tabText(0).startsWith(QStringLiteral("Untitled")));
        QVERIFY(!tabs.tabText(0).endsWith(QLatin1Char('*')));
    }

    void openingTheSameFileTwiceFocusesTheExistingTab()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/script.Rev");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("a <- 1\n"));
        file.close();

        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);

        QVERIFY(tabs.openFile(path));
        QCOMPARE(tabs.count(), 1);
        tabs.newDocument();
        QCOMPARE(tabs.count(), 2);

        QVERIFY(tabs.openFile(path));
        QCOMPARE(tabs.count(), 2);           // no new tab
        QCOMPARE(tabs.currentIndex(), 0);    // focused the existing one
    }

    // Q7's error navigation: MainWindow calls this from ConsoleWidget::errorLinkActivated().
    void openFileAtLineOpensAndMovesTheCaret()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/script.Rev");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("a <- 1\nb <- 2\nc <- 3\n"));
        file.close();

        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);

        QVERIFY(tabs.openFileAtLine(path, 2));
        QCOMPARE(tabs.count(), 1);
        ScriptEditor* editor = qobject_cast<ScriptEditor*>(tabs.widget(0));
        QVERIFY(editor);
        QCOMPARE(editor->cursorLine(), 2);

        // Already open: still moves the caret, the same way openFile() alone still focuses the existing tab.
        QVERIFY(tabs.openFileAtLine(path, 3));
        QCOMPARE(tabs.count(), 1);
        QCOMPARE(editor->cursorLine(), 3);
    }

    void editingMarksTheTabDirtyAndSavingClearsIt()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/script.Rev");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("a <- 1\n"));
        file.close();

        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);

        QVERIFY(tabs.openFile(path));
        ScriptEditor* editor = qobject_cast<ScriptEditor*>(tabs.widget(0));
        QVERIFY(editor);

        QTextCursor cursor = editor->textCursor();
        cursor.insertText(QStringLiteral("x"));
        QVERIFY(tabs.tabText(0).endsWith(QLatin1Char('*')));

        QVERIFY(tabs.saveCurrent());
        QVERIFY(!tabs.tabText(0).endsWith(QLatin1Char('*')));
    }

    void closingADirtyTabCanBeCancelled()
    {
        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);

        ScriptEditor* editor = tabs.newDocument();
        editor->textCursor().insertText(QStringLiteral("a <- 1"));
        QVERIFY(editor->isModified());

        clickPendingMessageBox(QMessageBox::Cancel);
        QVERIFY(!tabs.closeTabWithPrompt(0));
        QCOMPARE(tabs.count(), 1);   // still there
    }

    void closingADirtyTabCanDiscardChanges()
    {
        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);

        ScriptEditor* editor = tabs.newDocument();
        editor->textCursor().insertText(QStringLiteral("a <- 1"));

        clickPendingMessageBox(QMessageBox::Discard);
        QVERIFY(tabs.closeTabWithPrompt(0));
        QCOMPARE(tabs.count(), 0);
    }

    void runSelectionOrLineSubmitsTheCurrentLineWhenNothingIsSelected()
    {
        Session session;
        ConsoleWidget console(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy submitted(&session, &Session::submitFinished);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);
        ScriptEditor* editor = tabs.newDocument();
        editor->textCursor().insertText(QStringLiteral("mu <- 5"));

        tabs.runSelectionOrLine();
        QTRY_COMPARE_WITH_TIMEOUT(submitted.count(), 1, waitMs);

        auto* output = console.findChild<QPlainTextEdit*>(QStringLiteral("consoleOutput"));
        QVERIFY(output->toPlainText().contains(QStringLiteral("mu <- 5")));
    }

    // RStudio/VSCode-R style statement-aware run (6.8/Q7, pulled forward from phase 3): the cursor sits on the
    // MIDDLE line of a multi-line for-loop, not its first line, yet the whole loop (one open brace to its
    // matching close) runs as a single submission -- a per-line split would send "x <- i" on its own, which the
    // backend would report as a dangling fragment instead of loop body.
    void ctrlEnterOnAMultiLineStatementRunsTheWholeStatement()
    {
        Session session;
        ConsoleWidget console(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy submitted(&session, &Session::submitFinished);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);
        tabs.resize(400, 300);
        tabs.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tabs));

        ScriptEditor* editor = tabs.newDocument();
        editor->textCursor().insertText(
            QStringLiteral("for (i in 1:3) {\n  x <- i\n}\n\n# a trailing comment\nnext_stmt <- 1"));

        editor->setTextCursor(QTextCursor(editor->document()->findBlockByNumber(1)));   // inside the loop body
        editor->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(editor->hasFocus(), waitMs);

        tabs.runSelectionOrLine();
        QTRY_COMPARE_WITH_TIMEOUT(submitted.count(), 1, waitMs);

        auto* output = console.findChild<QPlainTextEdit*>(QStringLiteral("consoleOutput"));
        QVERIFY(output->toPlainText().contains(QStringLiteral("for (i in 1:3) {")));
        QVERIFY(output->toPlainText().contains(QStringLiteral("x <- i")));
        QVERIFY(output->toPlainText().contains(QStringLiteral("}")));

        QVERIFY(editor->hasFocus());
        QCOMPARE(editor->cursorLine(), 6);   // "next_stmt <- 1" -- past the loop's own "}", a blank line and a comment
    }

    // Comments are skipped by default, the other half of the same RStudio/VSCode-R convention: a comment-only
    // (or blank) line submits nothing, and Ctrl+Enter still moves straight to the next real statement, so
    // repeatedly pressing it steps through actual code instead of stalling on decoration.
    void ctrlEnterOnACommentLineSubmitsNothingAndSkipsToTheNextStatement()
    {
        Session session;
        ConsoleWidget console(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy submitted(&session, &Session::submitFinished);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);
        tabs.resize(400, 300);
        tabs.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tabs));

        ScriptEditor* editor = tabs.newDocument();
        editor->textCursor().insertText(QStringLiteral("# just a comment\nmu <- 5"));
        editor->setTextCursor(QTextCursor(editor->document()->findBlockByNumber(0)));
        editor->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(editor->hasFocus(), waitMs);

        tabs.runSelectionOrLine();
        QTest::qWait(200);   // long enough for a (wrongly) submitted statement to have round-tripped
        QCOMPARE(submitted.count(), 0);
        QCOMPARE(editor->cursorLine(), 2);   // skipped straight to "mu <- 5"
    }

    // The focus complaint that followed G6/G3's fix for the console losing focus after a submit: that fix made
    // ConsoleWidget unconditionally refocus its own input on every busy->ready transition, which also fired for
    // a Ctrl+Enter run from the editor and yanked focus away from it. The caret should land on the next line,
    // like a notebook cell, and stay in the editor -- not jump to the console.
    void ctrlEnterKeepsFocusInTheEditorAndMovesToTheNextLine()
    {
        Session session;
        ConsoleWidget console(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy submitted(&session, &Session::submitFinished);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);
        tabs.resize(400, 300);
        tabs.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tabs));

        ScriptEditor* editor = tabs.newDocument();
        editor->textCursor().insertText(QStringLiteral("mu <- 5\nsigma <- 1"));
        QTextCursor cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::EndOfBlock);   // caret on line 1, "mu <- 5"
        editor->setTextCursor(cursor);
        editor->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(editor->hasFocus(), waitMs);

        tabs.runSelectionOrLine();
        QTRY_COMPARE_WITH_TIMEOUT(submitted.count(), 1, waitMs);

        QVERIFY(editor->hasFocus());
        QCOMPARE(editor->cursorLine(), 2);   // on "sigma <- 1" now, not left behind and not sent to the console

        auto* output = console.findChild<QPlainTextEdit*>(QStringLiteral("consoleOutput"));
        QVERIFY(output->toPlainText().contains(QStringLiteral("mu <- 5")));
    }

    void runCurrentFileAlsoReturnsFocusToTheEditor()
    {
        Session session;
        ConsoleWidget console(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy submitted(&session, &Session::submitFinished);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);
        tabs.resize(400, 300);
        tabs.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tabs));

        ScriptEditor* editor = tabs.newDocument();
        editor->textCursor().insertText(QStringLiteral("a <- 1"));
        editor->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(editor->hasFocus(), waitMs);

        tabs.runCurrentFile();   // untitled: submits the raw buffer, same as EditorTabs::runCurrentFile()'s own branch
        QTRY_COMPARE_WITH_TIMEOUT(submitted.count(), 1, waitMs);

        QVERIFY(editor->hasFocus());
    }

    void runCurrentFileSourcesASavedFileByQuotedPath()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/script.Rev");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("a <- 1\n"));
        file.close();

        Session session;
        ConsoleWidget console(&session);
        QSignalSpy ready(&session, &Session::ready);
        QSignalSpy submitted(&session, &Session::submitFinished);
        session.start(mock);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, waitMs);

        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);
        QVERIFY(tabs.openFile(path));

        tabs.runCurrentFile();
        QTRY_COMPARE_WITH_TIMEOUT(submitted.count(), 1, waitMs);

        auto* output = console.findChild<QPlainTextEdit*>(QStringLiteral("consoleOutput"));
        QVERIFY(output->toPlainText().contains(QStringLiteral("source(\"%1\")").arg(path)));
    }

    void openFilePathsExcludesUntitledTabs()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/script.Rev");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);

        tabs.newDocument();
        QVERIFY(tabs.openFile(path));

        QCOMPARE(tabs.openFilePaths(), QStringList({path}));
    }

    void currentWordUnderCursorForwardsToTheCurrentEditor()
    {
        Session session;
        ConsoleWidget console(&session);
        QSettings backing(QStringLiteral("/dev/null"), QSettings::IniFormat);
        Settings settings(&backing);
        EditorTabs tabs(&console, &settings);

        QCOMPARE(tabs.currentWordUnderCursor(), QString());   // no tab open yet

        ScriptEditor* editor = tabs.newDocument();
        editor->textCursor().insertText(QStringLiteral("dnExponential"));
        QCOMPARE(tabs.currentWordUnderCursor(), QStringLiteral("dnExponential"));
    }
};

QTEST_MAIN(TstEditorTabs)
#include "tst_editortabs.moc"
