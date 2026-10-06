#include "app/RevHighlighter.h"

#include <QApplication>
#include <QPalette>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>

using namespace revstudio;

namespace {

/** QSyntaxHighlighter formats live in the block's text layout, not the document's own QTextCharFormat storage
 *  (that would only reflect rich-text editing, which never happens here) -- this is the usual way tests reach in
 *  to see what a highlighter actually set for one character. An uncovered position returns a default-constructed
 *  QTextCharFormat (foreground() has style Qt::NoBrush), which is how "this rule did not touch this character"
 *  is told apart from "a rule touched it".
 */
QTextCharFormat formatAt(const QTextDocument& doc, int blockNumber, int column)
{
    const QTextBlock block = doc.findBlockByNumber(blockNumber);
    const QList<QTextLayout::FormatRange> ranges = block.layout()->formats();
    for (const auto& range : ranges)
    {
        if (column >= range.start && column < range.start + range.length)
        {
            return range.format;
        }
    }
    return QTextCharFormat();
}

} // namespace

class TstRevHighlighter : public QObject
{
    Q_OBJECT

private slots:
    void keywordsAreBoldAndColoredButPlainIdentifiersAreNot()
    {
        // The document has to belong to a real widget: QSyntaxHighlighter only reacts to setPlainText() through
        // QTextDocument's detailed contentsChange(int, int, int) signal, and a bare, widget-less QTextDocument
        // (confirmed with a standalone diagnostic while building this test) never emits that signal for ANY edit,
        // not just setPlainText() -- highlightBlock() is simply never called. ScriptEditor's own document always
        // belongs to a shown QPlainTextEdit, so this is purely a test-setup requirement, not a production concern.
        QPlainTextEdit edit;
        RevHighlighter highlighter(edit.document());
        const QString text = QStringLiteral("if (x) { return 1 }");
        edit.setPlainText(text);

        const QTextCharFormat ifFormat = formatAt(*edit.document(), 0, text.indexOf(QStringLiteral("if")));
        QVERIFY(ifFormat.foreground().style() != Qt::NoBrush);
        QCOMPARE(ifFormat.fontWeight(), static_cast<int>(QFont::Bold));

        const QTextCharFormat returnFormat = formatAt(*edit.document(), 0, text.indexOf(QStringLiteral("return")));
        QCOMPARE(returnFormat.foreground(), ifFormat.foreground());

        const QTextCharFormat xFormat = formatAt(*edit.document(), 0, text.indexOf(QLatin1Char('x')));
        QCOMPARE(xFormat.foreground().style(), Qt::NoBrush);
    }

    void aHashInsideAStringDoesNotStartAComment()
    {
        QPlainTextEdit edit;
        RevHighlighter highlighter(edit.document());
        const QString text = QStringLiteral("x <- \"a # b\" # real comment");
        edit.setPlainText(text);

        const int innerHash = text.indexOf(QLatin1Char('#'));
        const int outerHash = text.indexOf(QLatin1Char('#'), innerHash + 1);
        QVERIFY(innerHash >= 0 && outerHash > innerHash);

        const QTextCharFormat inside = formatAt(*edit.document(), 0, innerHash);
        const QTextCharFormat outside = formatAt(*edit.document(), 0, outerHash);
        QVERIFY(!inside.fontItalic());    // string color, not the comment's italic
        QVERIFY(outside.fontItalic());
        QVERIFY(inside.foreground() != outside.foreground());

        // The closing quote right after "b" is still part of the string, not the comment that follows it.
        const QTextCharFormat closingQuote = formatAt(*edit.document(), 0, text.indexOf(QLatin1Char('"'), innerHash));
        QCOMPARE(closingQuote.foreground(), inside.foreground());
    }

    void anUnterminatedStringCarriesOverToTheNextBlock()
    {
        QPlainTextEdit edit;
        RevHighlighter highlighter(edit.document());
        edit.setPlainText(QStringLiteral("x <- \"abc\ndef\" + 1"));

        const QTextCharFormat insideLine0 = formatAt(*edit.document(), 0, 7);    // inside "abc", still open at EOL
        QVERIFY(insideLine0.foreground().style() != Qt::NoBrush);
        QCOMPARE(edit.document()->findBlockByNumber(0).userState(), 1);         // InString, carried into the next block

        const QTextCharFormat insideLine1 = formatAt(*edit.document(), 1, 0);    // "d" of "def", before the closing quote
        QCOMPARE(insideLine1.foreground(), insideLine0.foreground());

        const QTextCharFormat afterClose =
            formatAt(*edit.document(), 1, QStringLiteral("def\" + 1").indexOf(QLatin1Char('+')));
        QVERIFY(afterClose.foreground() != insideLine1.foreground());
    }

    void revBuiltinPrefixesGetAColorDistinctFromOtherCalls()
    {
        QPlainTextEdit edit;
        RevHighlighter highlighter(edit.document());
        const QString text = QStringLiteral("x ~ dnNormal(0, 1); y <- foo(1)");
        edit.setPlainText(text);

        const QTextCharFormat builtin = formatAt(*edit.document(), 0, text.indexOf(QStringLiteral("dnNormal")));
        const QTextCharFormat plain = formatAt(*edit.document(), 0, text.indexOf(QStringLiteral("foo")));
        QVERIFY(builtin.foreground().style() != Qt::NoBrush);
        QVERIFY(plain.foreground().style() != Qt::NoBrush);
        QVERIFY(builtin.foreground() != plain.foreground());
    }

    void aDottedCallColorsReceiverAndMethodAsOneToken()
    {
        QPlainTextEdit edit;
        RevHighlighter highlighter(edit.document());
        const QString text = QStringLiteral("obj.method(1)");
        edit.setPlainText(text);

        const QTextCharFormat receiver = formatAt(*edit.document(), 0, text.indexOf(QLatin1Char('o')));
        const QTextCharFormat method = formatAt(*edit.document(), 0, text.indexOf(QStringLiteral("method")));
        QVERIFY(receiver.foreground().style() != Qt::NoBrush);
        QCOMPARE(receiver.foreground(), method.foreground());
    }

    void numbersAndOperatorsAreDistinctCategories()
    {
        QPlainTextEdit edit;
        RevHighlighter highlighter(edit.document());
        const QString text = QStringLiteral("y <- 3.14 + 2");
        edit.setPlainText(text);

        const QTextCharFormat number = formatAt(*edit.document(), 0, text.indexOf(QStringLiteral("3.14")));
        const QTextCharFormat assign = formatAt(*edit.document(), 0, text.indexOf(QStringLiteral("<-")));
        QVERIFY(number.foreground().style() != Qt::NoBrush);
        QVERIFY(assign.foreground().style() != Qt::NoBrush);
        QVERIFY(number.foreground() != assign.foreground());
    }

    void updateColorsAdaptsToTheCurrentPalette()
    {
        const QPalette original = qApp->palette();

        QPlainTextEdit edit;
        RevHighlighter highlighter(edit.document());
        edit.setPlainText(QStringLiteral("# a comment"));
        const QColor lightCommentColor = formatAt(*edit.document(), 0, 0).foreground().color();

        QPalette dark;
        dark.setColor(QPalette::Base, QColor(35, 35, 35));
        qApp->setPalette(dark);
        highlighter.updateColors();
        const QColor darkCommentColor = formatAt(*edit.document(), 0, 0).foreground().color();

        qApp->setPalette(original);   // restore -- other tests in this binary share this QApplication
        highlighter.updateColors();

        QVERIFY(lightCommentColor != darkCommentColor);
    }
};

QTEST_MAIN(TstRevHighlighter)
#include "tst_revhighlighter.moc"
