#ifndef REVSTUDIO_REVHIGHLIGHTER_H
#define REVSTUDIO_REVHIGHLIGHTER_H

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

namespace revstudio {

/**
 * Rev syntax highlighting (GUI_Implementation_Note.md, section 6.8), pulled forward from phase 3 at the user's
 * request. The category list and the "an identifier right before `(` is a call" trick are adapted from the
 * MIT-licensed VSCode extension at /home/basanta/Code/revSyntax (syntaxes/rev.tmLanguage.json, themes/rev-theme.json);
 * the keyword/operator sets themselves come from the real lexer (src/grammar/lex.l) instead, since revSyntax's own
 * list was a smaller approximation (missing `function procedure class next break null Inf tab` and the `const
 * dynamic stochastic deterministic protected` modifiers, and claiming single-quoted strings that the grammar does
 * not have). Rev-specific extra, per 6.8: the systematic name prefixes `dn`/`mv`/`mn`/`fn` (the same two-letter
 * prefix `Func_ls::matchToken` filters `ls()` on) get their own color when called.
 *
 * Comments and strings are NOT just two more regex rules in the list below: unlike every other category, `#` has
 * no escape and a string can contain one, so matching them independently (as revSyntax's own grammar does) gets
 * `x <- "a # b"` wrong -- the rest of the line after that `#` would wrongly read as a comment. highlightBlock()
 * resolves both together in one left-to-right scan instead, carrying "still inside a string" across lines via
 * QSyntaxHighlighter's block state (lex.l's STRING token is one line in practice, but an unterminated `"` should
 * not derail highlighting for the rest of the file either).
 */
class RevHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit RevHighlighter(QTextDocument* document);

    /** Recomputes all colors from the current QApplication palette and rehighlights. Nothing calls this
     *  automatically on a theme change (View > Theme) yet -- an already-open tab keeps the colors current when
     *  it was opened, the same granularity Theme::apply() itself has for everything else in phase 2. */
    void updateColors();

protected:
    void highlightBlock(const QString& text) override;

private:
    enum BlockState { NotInString = 0, InString = 1 };

    struct Rule
    {
        QRegularExpression pattern;
        const QTextCharFormat* format;
    };

    void buildRules();
    void highlightStringsAndComments(const QString& text);

    QVector<Rule> rules_;

    QTextCharFormat keywordFormat_;
    QTextCharFormat constantFormat_;
    QTextCharFormat operatorFormat_;
    QTextCharFormat numberFormat_;
    QTextCharFormat functionFormat_;
    QTextCharFormat builtinFormat_;
    QTextCharFormat stringFormat_;
    QTextCharFormat commentFormat_;
};

} // namespace revstudio

#endif
