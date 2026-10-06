#include "app/RevHighlighter.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPalette>

namespace revstudio {

RevHighlighter::RevHighlighter(QTextDocument* document) : QSyntaxHighlighter(document)
{
    buildRules();
    updateColors();
}

void RevHighlighter::buildRules()
{
    // Order matters: later rules in this list win over earlier ones wherever they overlap (highlightBlock()
    // applies them in order with plain setFormat() calls). Function-call detection has to come before keywords,
    // not after, because `if(x)`/`while(x)`/`return(x)` -- keywords with no space before `(` -- would otherwise
    // match the generic "identifier right before (" rule too; keywords and constants are listed last so they win
    // that tie.
    rules_.append({QRegularExpression(QStringLiteral(R"((?<![\w.])(?:[0-9]+\.[0-9]*|\.[0-9]+|[0-9]+)(?:[eE][+-]?[0-9]+)?\b)")),
                    &numberFormat_});

    rules_.append({QRegularExpression(QStringLiteral(R"(<-&|<-|:=|~|\+=|-=|\*=|/=|\+\+|--|&&|\|\||==|!=|<=|>=|\|>|=|&|\||>|<)")),
                    &operatorFormat_});
    rules_.append({QRegularExpression(QStringLiteral(R"(\b_\b)")), &operatorFormat_});   // the pipe placeholder, `|>`'s `_`

    // Adapted from revSyntax's own two-rule split (syntaxes/rev.tmLanguage.json): a qualified call ("obj.method(")
    // colors receiver and method as one token, same as its "variable.function.rev" scope does; a plain call colors
    // just the name. The Rev-specific dn/mv/mn/fn prefix (6.8; the same prefix Func_ls::matchToken filters on) is
    // not in revSyntax at all and gets its own rule and color, applied last so it wins over the plain-call color.
    rules_.append({QRegularExpression(QStringLiteral(R"(\b[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*\s*\()")),
                    &functionFormat_});
    rules_.append({QRegularExpression(QStringLiteral(R"(\b[A-Za-z_][A-Za-z0-9_]*\s*\()")), &functionFormat_});
    rules_.append({QRegularExpression(QStringLiteral(R"(\b(?:dn|mv|mn|fn)[A-Za-z0-9_]*\s*\()")), &builtinFormat_});

    // Keyword/constant sets come from src/grammar/lex.l (lines 66-139), not revSyntax, which only had
    // if/else/for/while/return and TRUE/FALSE -- missing function/procedure/class/next/break/null/Inf/tab and the
    // const/dynamic/stochastic/deterministic/protected modifiers entirely.
    rules_.append({QRegularExpression(QStringLiteral(
        R"(\b(?:function|procedure|class|for|in|if|else|while|next|break|return|const|dynamic|stochastic|deterministic|protected)\b)")),
        &keywordFormat_});
    rules_.append({QRegularExpression(QStringLiteral(R"(\b(?:null|true|false|NULL|TRUE|FALSE|Inf|tab|TAB)\b)")),
                    &constantFormat_});
}

void RevHighlighter::updateColors()
{
    // revSyntax's own theme (themes/rev-theme.json) is dark-only; RevStudio isn't (View > Theme: System/Light/
    // Dark), so its colors are reused for dark mode and a parallel, equally conventional light-mode set (the
    // same roles VS Code's bundled "Light+" theme uses) stands in for light mode, picked by the current palette
    // rather than RevStudio's own Theme::Mode, which does not apply to System when the platform itself is dark.
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;

    keywordFormat_ = QTextCharFormat();
    keywordFormat_.setForeground(dark ? QColor(0xFF, 0x79, 0xC6) : QColor(0xAF, 0x00, 0xDB));
    keywordFormat_.setFontWeight(QFont::Bold);

    constantFormat_ = QTextCharFormat();
    constantFormat_.setForeground(dark ? QColor(0xBD, 0x93, 0xF9) : QColor(0x00, 0x00, 0xFF));
    constantFormat_.setFontWeight(QFont::Bold);

    operatorFormat_ = QTextCharFormat();
    operatorFormat_.setForeground(dark ? QColor(0xFF, 0xAA, 0x00) : QColor(0xD2, 0x69, 0x1E));

    numberFormat_ = QTextCharFormat();
    numberFormat_.setForeground(dark ? QColor(0xBD, 0x93, 0xF9) : QColor(0x09, 0x86, 0x58));

    functionFormat_ = QTextCharFormat();
    functionFormat_.setForeground(dark ? QColor(0xFF, 0xD7, 0x00) : QColor(0x79, 0x5E, 0x26));

    builtinFormat_ = QTextCharFormat();
    builtinFormat_.setForeground(dark ? QColor(0x00, 0xFF, 0xFF) : QColor(0x26, 0x7F, 0x99));
    builtinFormat_.setFontWeight(QFont::Bold);

    stringFormat_ = QTextCharFormat();
    stringFormat_.setForeground(dark ? QColor(0x50, 0xFA, 0x7B) : QColor(0xA3, 0x15, 0x15));

    commentFormat_ = QTextCharFormat();
    commentFormat_.setForeground(dark ? QColor(0x62, 0x72, 0xA4) : QColor(0x00, 0x80, 0x00));
    commentFormat_.setFontItalic(true);

    rehighlight();
}

void RevHighlighter::highlightBlock(const QString& text)
{
    for (const Rule& rule : rules_)
    {
        QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
        while (it.hasNext())
        {
            const QRegularExpressionMatch match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), *rule.format);
        }
    }

    // Done last and outside the rule list on purpose -- see the class comment on why comments and strings have
    // to be resolved together rather than as two more independent regexes, and applied after everything above so
    // whatever the generic rules guessed inside a string or comment (a "return" in a comment, a number in a
    // string) is unconditionally overwritten with the right color for the characters it actually covers.
    highlightStringsAndComments(text);
}

void RevHighlighter::highlightStringsAndComments(const QString& text)
{
    bool inString = previousBlockState() == InString;
    int index = 0;
    int stringStart = inString ? 0 : -1;

    while (true)
    {
        if (inString)
        {
            int i = index;
            bool closed = false;
            while (i < text.length())
            {
                if (text.at(i) == QLatin1Char('\\') && i + 1 < text.length())
                {
                    i += 2;
                    continue;
                }
                if (text.at(i) == QLatin1Char('"'))
                {
                    closed = true;
                    break;
                }
                ++i;
            }
            const int end = closed ? i + 1 : text.length();
            setFormat(stringStart, end - stringStart, stringFormat_);
            if (!closed)
            {
                break;   // inString is already true; carries over via setCurrentBlockState() below
            }
            inString = false;
            index = end;
            if (index >= text.length())
            {
                break;
            }
            continue;
        }

        const int hash = text.indexOf(QLatin1Char('#'), index);
        const int quote = text.indexOf(QLatin1Char('"'), index);
        if (quote >= 0 && (hash < 0 || quote < hash))
        {
            stringStart = quote;
            inString = true;
            index = quote + 1;
            continue;
        }
        if (hash >= 0)
        {
            setFormat(hash, text.length() - hash, commentFormat_);
        }
        break;
    }

    setCurrentBlockState(inString ? InString : NotInString);
}

} // namespace revstudio
