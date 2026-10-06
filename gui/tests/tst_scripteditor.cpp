#include "app/ScriptEditor.h"

#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QToolButton>
#include <QtTest>

using namespace revstudio;

class TstScriptEditor : public QObject
{
    Q_OBJECT

private slots:
    void loadAndSaveRoundTripPlainUtf8()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/script.Rev");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("a <- 1\nb <- 2\n"));
        file.close();

        ScriptEditor editor;
        QVERIFY(editor.loadFromFile(path));
        QCOMPARE(editor.toPlainText(), QStringLiteral("a <- 1\nb <- 2\n"));
        QVERIFY(!editor.isModified());
        QCOMPARE(editor.filePath(), path);
        QVERIFY(!editor.isUntitled());

        // A real edit, not setPlainText(): that resets the modified flag unconditionally (it is meant for
        // *loading* content, which is exactly why loadFromFile() relies on that to start clean).
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.movePosition(QTextCursor::PreviousCharacter);   // before the trailing newline
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        cursor.insertText(QStringLiteral("b <- 3"));
        QVERIFY(editor.isModified());
        QVERIFY(editor.saveToFile(path));
        QVERIFY(!editor.isModified());

        QFile reread(path);
        QVERIFY(reread.open(QIODevice::ReadOnly));
        QCOMPARE(reread.readAll(), QByteArrayLiteral("a <- 1\nb <- 3\n"));
    }

    void bomAndCrlfAreDetectedAndReproducedOnSave()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/windows.Rev");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray("\xEF\xBB\xBF") + QByteArrayLiteral("a <- 1\r\nb <- 2\r\n"));
        file.close();

        ScriptEditor editor;
        QVERIFY(editor.loadFromFile(path));
        QCOMPARE(editor.toPlainText(), QStringLiteral("a <- 1\nb <- 2\n"));   // normalised internally, like any Qt text widget

        editor.setPlainText(QStringLiteral("a <- 1\nb <- 9\n"));
        QVERIFY(editor.saveToFile(path));

        QFile reread(path);
        QVERIFY(reread.open(QIODevice::ReadOnly));
        const QByteArray bytes = reread.readAll();
        QVERIFY(bytes.startsWith("\xEF\xBB\xBF"));
        QCOMPARE(bytes, QByteArray("\xEF\xBB\xBF") + QByteArrayLiteral("a <- 1\r\nb <- 9\r\n"));
    }

    void cursorPositionMovedReportsOneBasedLineAndColumn()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("aaa\nbb\nc"));

        QSignalSpy moved(&editor, &ScriptEditor::cursorPositionMoved);
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::Down);     // start of line 2 ("bb")
        cursor.movePosition(QTextCursor::Right);    // one char into it
        editor.setTextCursor(cursor);

        QVERIFY(moved.count() >= 1);
        QCOMPARE(editor.cursorLine(), 2);
        QCOMPARE(editor.cursorColumn(), 2);
    }

    void enterAutoIndentsAndAddsALevelAfterAnOpenBrace()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("    if (x) {"));
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);

        QTest::keyClick(&editor, Qt::Key_Return);

        const QStringList lines = editor.toPlainText().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.at(1), QStringLiteral("        "));   // 4 (copied) + 4 (one more level)
    }

    void ctrlSlashTogglesLineComments()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("a <- 1\nb <- 2"));
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
        cursor.movePosition(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
        editor.setTextCursor(cursor);

        QTest::keyClick(&editor, Qt::Key_Slash, Qt::ControlModifier);
        QCOMPARE(editor.toPlainText(), QStringLiteral("# a <- 1\n# b <- 2"));

        // selection survived the edit (both lines still spanned), so toggling again uncomments both
        QTest::keyClick(&editor, Qt::Key_Slash, Qt::ControlModifier);
        QCOMPARE(editor.toPlainText(), QStringLiteral("a <- 1\nb <- 2"));
    }

    // Q8's F1: Rev's own identifier rule (src/grammar/lex.l's ID, which allows a leading underscore), not Qt's
    // generic "word" concept.
    void wordUnderCursorFindsTheIdentifierTouchingTheCaretAnywhereWithinIt()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("mu ~ dnExponential(1.0)"));

        for (int column : {5, 8, 17})   // start, middle and end of "dnExponential"
        {
            QTextCursor cursor = editor.textCursor();
            cursor.setPosition(column);
            editor.setTextCursor(cursor);
            QCOMPARE(editor.wordUnderCursor(), QStringLiteral("dnExponential"));
        }
    }

    void wordUnderCursorIsEmptyBetweenIdentifiers()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("a <- 1"));
        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(2);   // the space after "a"
        editor.setTextCursor(cursor);
        QVERIFY(editor.wordUnderCursor().isEmpty());
    }

    void currentStatementTextReturnsJustTheLineForASimpleStatement()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("a <- 1\nb <- 2\nc <- 3"));
        editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(1)));

        int endBlock = -1;
        QCOMPARE(editor.currentStatementText(&endBlock), QStringLiteral("b <- 2"));
        QCOMPARE(endBlock, 1);
    }

    // RStudio/VSCode-R style statement-aware run (6.8/Q7): the SAME multi-line statement comes back whichever
    // of its lines the cursor is on, not just its first one.
    void currentStatementTextSpansAMultiLineBraceRegardlessOfWhereTheCursorIs()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("for (i in 1:3) {\n  x <- i\n}\ny <- 1"));
        const QString wholeLoop = QStringLiteral("for (i in 1:3) {\n  x <- i\n}");

        for (int line = 0; line <= 2; ++line)
        {
            editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(line)));
            int endBlock = -1;
            QCOMPARE(editor.currentStatementText(&endBlock), wholeLoop);
            QCOMPARE(endBlock, 2);
        }
    }

    void currentStatementTextIgnoresBracketsInsideStringsAndComments()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("x <- \"{\" # a stray } too\ny <- 2"));
        editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(0)));

        int endBlock = -1;
        QCOMPARE(editor.currentStatementText(&endBlock), QStringLiteral("x <- \"{\" # a stray } too"));
        QCOMPARE(endBlock, 0);   // a real (unescaped) brace would have pulled in "y <- 2" too
    }

    void currentStatementTextOnABlankOrCommentLineHasNothingToRun()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("a <- 1\n\n# a comment\nb <- 2"));

        editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(1)));
        int blankEndBlock = -1;
        QCOMPARE(editor.currentStatementText(&blankEndBlock), QString());
        QCOMPARE(blankEndBlock, 1);

        editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(2)));
        int commentEndBlock = -1;
        QCOMPARE(editor.currentStatementText(&commentEndBlock), QStringLiteral("# a comment"));
        QCOMPARE(commentEndBlock, 2);
    }

    // Q5, pulled forward from phase 3: the gutter widens as the document grows past 9, 99, ... lines, since a
    // fixed width would either waste space for a short file or truncate the numbers of a long one.
    void lineNumberAreaWidthGrowsWithMoreDigits()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("a"));
        const int widthForOneDigit = editor.lineNumberAreaWidth();

        QString manyLines;
        for (int i = 0; i < 150; ++i)
        {
            manyLines += QStringLiteral("x\n");
        }
        editor.setPlainText(manyLines);   // 150+ blocks -> 3-digit line numbers
        QVERIFY(editor.lineNumberAreaWidth() > widthForOneDigit);
    }

    void currentLineIsHighlightedAsTheCursorMoves()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("a\nb\nc"));
        editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(1)));

        bool foundFullWidthSelectionOnLine1 = false;
        for (const auto& selection : editor.extraSelections())
        {
            if (!selection.cursor.hasSelection() && selection.cursor.blockNumber() == 1
                && selection.format.property(QTextFormat::FullWidthSelection).toBool())
            {
                foundFullWidthSelectionOnLine1 = true;
            }
        }
        QVERIFY(foundFullWidthSelectionOnLine1);
    }

    // Q5: matching brackets are both highlighted, with the SAME format, the moment the cursor sits next to
    // either one of them -- not just the one the cursor happens to be touching.
    void matchingBracketsAreHighlightedOnBothSides()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("f(a, b)"));
        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(2);   // right after the '('
        editor.setTextCursor(cursor);

        bool foundOpen = false;
        bool foundClose = false;
        QTextCharFormat openFormat;
        QTextCharFormat closeFormat;
        for (const auto& selection : editor.extraSelections())
        {
            if (selection.cursor.selectionStart() == 1 && selection.cursor.selectionEnd() == 2)
            {
                foundOpen = true;
                openFormat = selection.format;
            }
            if (selection.cursor.selectionStart() == 6 && selection.cursor.selectionEnd() == 7)
            {
                foundClose = true;
                closeFormat = selection.format;
            }
        }
        QVERIFY(foundOpen);
        QVERIFY(foundClose);
        QCOMPARE(openFormat.background(), closeFormat.background());
    }

    void unmatchedBracketsAreNotHighlighted()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("f(a, b"));   // no closing paren
        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(2);
        editor.setTextCursor(cursor);

        for (const auto& selection : editor.extraSelections())
        {
            QVERIFY(!selection.cursor.hasSelection());   // only the (selection-less) current-line highlight
        }
    }

    // The same "skip strings and comments" rule RevHighlighter and currentStatementText() both apply: a
    // bracket mentioned inside either must not count as a real one to match against.
    void bracketsInsideAStringOrCommentAreIgnoredWhenMatching()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("f(\"(\") # )"));   // real "(" at 1 pairs with real ")" at 5
        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(2);   // right after the real '('
        editor.setTextCursor(cursor);

        bool foundMatchAtRealClose = false;
        for (const auto& selection : editor.extraSelections())
        {
            if (selection.cursor.hasSelection() && selection.cursor.selectionStart() == 5)
            {
                foundMatchAtRealClose = true;
            }
        }
        QVERIFY(foundMatchAtRealClose);
    }

    // Q6, pulled forward from phase 3: Ctrl+F shows the bar focused on its search field with the replace row
    // hidden; Ctrl+H shows the same bar with the replace row too.
    void ctrlFShowsTheFindBarFocusedWithoutReplace()
    {
        ScriptEditor editor;
        editor.resize(400, 300);
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        editor.setPlainText(QStringLiteral("alpha"));
        editor.setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(editor.hasFocus(), 10000);

        QTest::keyClick(&editor, Qt::Key_F, Qt::ControlModifier);

        auto* searchEdit = editor.findChild<QLineEdit*>(QStringLiteral("findBarSearch"));
        QVERIFY(searchEdit && searchEdit->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(searchEdit->hasFocus(), 10000);
        QVERIFY(!editor.findChild<QLineEdit*>(QStringLiteral("findBarReplace"))->isVisible());
    }

    void ctrlHAlsoShowsTheReplaceRow()
    {
        ScriptEditor editor;
        editor.resize(400, 300);
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        editor.setPlainText(QStringLiteral("alpha"));

        QTest::keyClick(&editor, Qt::Key_H, Qt::ControlModifier);

        QVERIFY(editor.findChild<QLineEdit*>(QStringLiteral("findBarReplace"))->isVisible());
    }

    void findNextSelectsTheMatchAndWrapsAroundAtTheEnd()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("alpha beta alpha"));
        editor.showFindBar(false);

        auto* searchEdit = editor.findChild<QLineEdit*>(QStringLiteral("findBarSearch"));
        auto* nextButton = editor.findChild<QToolButton*>(QStringLiteral("findBarNext"));
        searchEdit->setText(QStringLiteral("alpha"));   // textChanged already finds the first match

        QCOMPARE(editor.textCursor().selectedText(), QStringLiteral("alpha"));
        QCOMPARE(editor.textCursor().selectionStart(), 0);

        QTest::mouseClick(nextButton, Qt::LeftButton);
        QCOMPARE(editor.textCursor().selectionStart(), 11);   // the second "alpha"

        QTest::mouseClick(nextButton, Qt::LeftButton);        // past the last match: wraps back to the first
        QCOMPARE(editor.textCursor().selectionStart(), 0);
    }

    void caseSensitiveToggleAffectsMatching()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("Alpha"));
        editor.showFindBar(false);

        auto* searchEdit = editor.findChild<QLineEdit*>(QStringLiteral("findBarSearch"));
        auto* caseCheck = editor.findChild<QToolButton*>(QStringLiteral("findBarCaseSensitive"));
        caseCheck->setChecked(true);
        searchEdit->setText(QStringLiteral("alpha"));

        QVERIFY(!editor.textCursor().hasSelection());   // case-sensitive "alpha" does not match "Alpha"

        caseCheck->setChecked(false);
        auto* nextButton = editor.findChild<QToolButton*>(QStringLiteral("findBarNext"));
        QTest::mouseClick(nextButton, Qt::LeftButton);
        QCOMPARE(editor.textCursor().selectedText(), QStringLiteral("Alpha"));
    }

    void regexToggleEnablesPatternMatching()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("x <- 42"));
        editor.showFindBar(false);

        auto* searchEdit = editor.findChild<QLineEdit*>(QStringLiteral("findBarSearch"));
        editor.findChild<QToolButton*>(QStringLiteral("findBarRegex"))->setChecked(true);
        searchEdit->setText(QStringLiteral("[0-9]+"));

        QCOMPARE(editor.textCursor().selectedText(), QStringLiteral("42"));
    }

    void replaceOneReplacesOnlyTheCurrentMatchAndMovesToTheNext()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("alpha alpha alpha"));
        editor.showFindBar(true);

        editor.findChild<QLineEdit*>(QStringLiteral("findBarSearch"))->setText(QStringLiteral("alpha"));
        editor.findChild<QLineEdit*>(QStringLiteral("findBarReplace"))->setText(QStringLiteral("beta"));
        QCOMPARE(editor.textCursor().selectionStart(), 0);

        QTest::mouseClick(editor.findChild<QToolButton*>(QStringLiteral("findBarReplaceOne")), Qt::LeftButton);

        QCOMPARE(editor.toPlainText(), QStringLiteral("beta alpha alpha"));
        QCOMPARE(editor.textCursor().selectedText(), QStringLiteral("alpha"));   // moved on to the next match
    }

    void replaceAllReplacesEveryMatch()
    {
        ScriptEditor editor;
        editor.setPlainText(QStringLiteral("alpha alpha alpha"));
        editor.showFindBar(true);

        editor.findChild<QLineEdit*>(QStringLiteral("findBarSearch"))->setText(QStringLiteral("alpha"));
        editor.findChild<QLineEdit*>(QStringLiteral("findBarReplace"))->setText(QStringLiteral("beta"));
        QTest::mouseClick(editor.findChild<QToolButton*>(QStringLiteral("findBarReplaceAll")), Qt::LeftButton);

        QCOMPARE(editor.toPlainText(), QStringLiteral("beta beta beta"));
        QVERIFY(editor.findChild<QLabel*>(QStringLiteral("findBarStatus"))->text().contains(QStringLiteral("3")));
    }

    void escapeClosesTheFindBarAndReturnsFocusToTheEditor()
    {
        ScriptEditor editor;
        editor.resize(400, 300);
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        editor.setPlainText(QStringLiteral("alpha"));
        editor.showFindBar(false);

        auto* searchEdit = editor.findChild<QLineEdit*>(QStringLiteral("findBarSearch"));
        QTRY_VERIFY_WITH_TIMEOUT(searchEdit->hasFocus(), 10000);

        QTest::keyClick(searchEdit, Qt::Key_Escape);

        QVERIFY(!searchEdit->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(editor.hasFocus(), 10000);
    }

    void externalChangeIsReportedButOwnSaveIsNot()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/script.Rev");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("a <- 1\n"));
        file.close();

        ScriptEditor editor;
        QVERIFY(editor.loadFromFile(path));

        QSignalSpy externallyModified(&editor, &ScriptEditor::externallyModified);

        editor.setPlainText(QStringLiteral("a <- 2\n"));
        QVERIFY(editor.saveToFile(path));
        QTest::qWait(200);
        QCOMPARE(externallyModified.count(), 0);   // our own save must not look like an external change

        QFile rewrite(path);
        QVERIFY(rewrite.open(QIODevice::WriteOnly | QIODevice::Truncate));
        rewrite.write(QByteArrayLiteral("a <- 3\n"));
        rewrite.close();
        QTRY_COMPARE_WITH_TIMEOUT(externallyModified.count(), 1, 10000);
    }
};

QTEST_MAIN(TstScriptEditor)
#include "tst_scripteditor.moc"
