#ifndef REVSTUDIO_SCRIPTEDITOR_H
#define REVSTUDIO_SCRIPTEDITOR_H

#include <QPlainTextEdit>

class QFileSystemWatcher;
class QFont;
class QPaintEvent;
class QRect;
class QResizeEvent;

namespace revstudio {

class FindBar;
class RevHighlighter;

/**
 * G6: one editor document (GUI_Implementation_Note.md, section 6.8). Plain `QPlainTextEdit` plus file I/O,
 * dirty tracking (QTextDocument::isModified(), already built in) and external-change detection -- NOT yet the
 * line-number gutter, bracket matching or syntax highlighting the class table's fuller description eventually
 * wants: those are explicitly phase 3 ("the editor feels like a small IDE"), per 12.2's phase table, not G6.
 *
 * One quirk worth knowing: UTF-8 BOM and the original line-ending style are remembered on load and reproduced on
 * save, but ONLY the line-ending style actually used in the loaded file at the time -- a file that mixes CRLF
 * and LF (itself already irregular) is normalised to LF internally like any Qt text widget and will come back
 * out as LF throughout, not reproduced mixed.
 *
 * Syntax highlighting (RevHighlighter), statement-aware run, the line-number gutter/current-line highlight/
 * bracket matching (Q5) and now find/replace (Q6: Ctrl+F finds, Ctrl+H adds the replace row, Escape closes it,
 * `QTextDocument::find` as 6.8 itself says) have all been pulled forward from phase 3 on request; completion is
 * still exactly where 12.2's phase table puts it.
 */
class ScriptEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit ScriptEditor(QWidget* parent = nullptr);

    /** Empty until the first successful load or save. */
    QString filePath() const { return filePath_; }
    bool    isUntitled() const { return filePath_.isEmpty(); }
    bool    isModified() const { return document()->isModified(); }

    bool loadFromFile(const QString& path, QString* error = nullptr);
    bool saveToFile(const QString& path, QString* error = nullptr);   // also updates filePath() on success

    /** Sets the font AND recomputes the tab stop distance from it (G7/Preferences) -- the plain QWidget::setFont()
     *  would leave tab stops sized for whatever font was current when the constructor ran. */
    void applyFont(const QFont& font);

    /** 1-based, matching how editors conventionally display them (and the status bar mockup, "Ln 3, Col 12"). */
    int cursorLine() const;
    int cursorColumn() const;

    /** Q7's error navigation: moves the caret to `line` (1-based, clamped to the document), scrolls it into
     *  view and focuses the editor. */
    void goToLine(int line);

    /** Q8's F1: the identifier touching the caret (Rev's own rules -- letters, digits, '_' -- not Qt's generic
     *  notion of a "word"), or an empty string between/outside identifiers. */
    QString wordUnderCursor() const;

    /** The complete Rev statement containing the cursor (6.8's "statement-aware run", pulled forward from phase
     *  3's Q7, RStudio/VSCode-R style): a single scan of the whole buffer tracks `(){}[]` depth, skipping
     *  characters inside strings and comments the same way RevHighlighter does (so a `{` in either does not
     *  count), to find where the enclosing statement starts and ends -- a multi-line `for(...) { ... }` runs as
     *  one submission no matter which of its lines the cursor is on, the same way RStudio's own Ctrl+Enter does.
     *  If the cursor sits on a blank or comment-only line outside any statement, both bounds are that one line
     *  and the returned text is empty or comment-only; the caller decides whether that is worth submitting.
     *  `endBlock`, if given, receives the statement's last zero-based block number, for the caller to advance
     *  the cursor past it afterward. */
    QString currentStatementText(int* endBlock = nullptr) const;

    /** Q5: the line-number gutter, painted by a small file-local `LineNumberArea` widget that just forwards its
     *  paintEvent() here. Public because that widget (not a subclass, a separate QWidget overlaid in the
     *  viewport margin -- the standard QPlainTextEdit pattern) needs to call it and ask for its width. */
    void lineNumberAreaPaintEvent(QPaintEvent* event);
    int  lineNumberAreaWidth() const;

    /** Q6: shows the in-editor find/replace bar (`withReplace` also shows the replace row -- Ctrl+F vs Ctrl+H),
     *  focusing and selecting its search field. Escape, while the bar has focus, hides it again. */
    void showFindBar(bool withReplace);

signals:
    void cursorPositionMoved(int line, int column);
    /** The file on disk changed since it was last loaded/saved here. Fires at most once per external change;
     *  call loadFromFile() again (or dismiss it) in response. */
    void externallyModified();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void toggleCommentOnSelectedLines();
    void indentNewLine();
    void currentStatementRange(int* startBlock, int* endBlock) const;
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect& rect, int dy);
    QFont lineNumberFont() const;
    void highlightCurrentLineAndMatchingBracket();
    int  matchingBracketPosition(int bracketPosition) const;
    void findNext(bool backward);
    void replaceCurrent();
    void replaceAllMatches();
    int  countMatches() const;

    QString            filePath_;
    bool               hadBom_ = false;
    QByteArray         lineEnding_ = "\n";   // "\n" or "\r\n", as found in the loaded file
    QFileSystemWatcher* watcher_;
    bool                suppressNextChange_ = false;   // true while saveToFile()'s own rewrite is in flight
    RevHighlighter*      highlighter_;
    QWidget*             lineNumberArea_;
    FindBar*             findBar_;

};

} // namespace revstudio

#endif
