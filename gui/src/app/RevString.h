#ifndef REVSTUDIO_REVSTRING_H
#define REVSTUDIO_REVSTRING_H

#include <QString>

namespace revstudio::RevString {

/**
 * Turns `text` into a Rev string literal, including the surrounding quotes, that `submit`s back to exactly
 * `text` (GUI_Implementation_Note.md, section 6.7, defect D15 -- the old GTK code's two copy-pasted handlers
 * escaped `\` on Windows but never `"`, so a path containing a quote broke the generated command).
 *
 * Escapes only `\` and `"`. Verified empirically against the real interpreter (not assumed from the grammar)
 * that these are the only two characters that need it: Rev's lexer recognises `\\`, `\"`, `\n` and `\t` as real
 * escapes, but any OTHER `\<char>` -- including seemingly ordinary ones like `\r`, `\f`, `\U` -- silently
 * vanishes, backslash and all, rather than being kept literally. A raw, un-escaped control character (a literal
 * newline or tab byte in a path) needs no escaping of its own: the lexer's string rule only treats `\` and `"`
 * specially, so anything else passes through unchanged.
 */
inline QString quote(const QString& text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));   // must run before the '"' replacement below
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

} // namespace revstudio::RevString

#endif
