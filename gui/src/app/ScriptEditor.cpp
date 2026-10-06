#include "app/ScriptEditor.h"

#include "app/RevHighlighter.h"

#include <QFile>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>

namespace revstudio {

namespace {

constexpr int lineNumberInterval = 5;   // gutter clutter reduction, on request: only every 5th line gets a number

/** Calls `onCodeChar(k)` for every index `k` of `text` NOT inside a string or a comment, continuing an
 *  in-string flag across lines; returns whether the line ends still inside an unterminated string. The same
 *  scan RevHighlighter uses for the same reason (a bracket or quote inside either must not count), shared here
 *  because `currentStatementRange()` and matchingBracketPosition()'s mask both need exactly this.
 */
template <typename F>
bool scanCodeCharacters(const QString& text, bool inString, F&& onCodeChar)
{
    int index = 0;
    while (true)
    {
        if (inString)
        {
            int j = index;
            bool closed = false;
            while (j < text.length())
            {
                if (text.at(j) == QLatin1Char('\\') && j + 1 < text.length())
                {
                    j += 2;
                    continue;
                }
                if (text.at(j) == QLatin1Char('"'))
                {
                    closed = true;
                    break;
                }
                ++j;
            }
            if (!closed)
            {
                return true;
            }
            inString = false;
            index = j + 1;
            if (index >= text.length())
            {
                return false;
            }
            continue;
        }

        const int hash  = text.indexOf(QLatin1Char('#'), index);
        const int quote = text.indexOf(QLatin1Char('"'), index);
        const bool quoteFirst = quote >= 0 && (hash < 0 || quote < hash);
        const int codeEnd = quoteFirst ? quote : (hash >= 0 ? hash : text.length());

        for (int k = index; k < codeEnd; ++k)
        {
            onCodeChar(k);
        }

        if (quoteFirst)
        {
            inString = true;
            index = quote + 1;
            continue;
        }
        return false;
    }
}

} // namespace

/** The line-number gutter (Q5): a separate QWidget overlaid in the viewport margin, the standard QPlainTextEdit
 *  pattern (Qt's own "Code Editor" example uses exactly this), rather than painting numbers into the document
 *  itself. It owns no logic of its own -- both overrides just forward into the editor, which has everything
 *  (font metrics, block geometry) already.
 */
class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(ScriptEditor* editor) : QWidget(editor), editor_(editor) {}

    QSize sizeHint() const override { return QSize(editor_->lineNumberAreaWidth(), 0); }

protected:
    void paintEvent(QPaintEvent* event) override { editor_->lineNumberAreaPaintEvent(event); }

private:
    ScriptEditor* editor_;
};

/** Q6's in-editor find/replace bar: a floating overlay positioned by ScriptEditor::resizeEvent() (the same
 *  technique LineNumberArea uses to stay put), not a dock or a dialog, so showing or hiding it never disturbs
 *  the surrounding layout. Owns no search logic of its own -- it just reads back as plain data (searchTerm(),
 *  isRegex(), ...) and emits a signal per action; ScriptEditor does the actual `QTextDocument::find()` work.
 *  Escape closes it via ordinary Qt key-event propagation: QLineEdit does not consume it, so it bubbles up to
 *  this widget's own keyPressEvent() without any extra plumbing.
 */
class FindBar : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(QWidget* parent) : QWidget(parent)
    {
        setAutoFillBackground(true);

        searchEdit_ = new QLineEdit(this);
        searchEdit_->setObjectName(QStringLiteral("findBarSearch"));
        searchEdit_->setPlaceholderText(tr("Find"));

        replaceEdit_ = new QLineEdit(this);
        replaceEdit_->setObjectName(QStringLiteral("findBarReplace"));
        replaceEdit_->setPlaceholderText(tr("Replace"));

        // Compact, checkable icon-style buttons (text glyphs standing in for icons, VSCode's own find widget
        // convention) rather than a checkbox-plus-label each, so the search field keeps most of the bar's
        // width instead of giving a third of it to three spelled-out labels.
        caseCheck_ = new QToolButton(this);
        caseCheck_->setObjectName(QStringLiteral("findBarCaseSensitive"));
        caseCheck_->setText(QStringLiteral("Aa"));
        caseCheck_->setToolTip(tr("Match case"));
        wholeWordCheck_ = new QToolButton(this);
        wholeWordCheck_->setObjectName(QStringLiteral("findBarWholeWord"));
        wholeWordCheck_->setText(QStringLiteral("|ab|"));
        wholeWordCheck_->setToolTip(tr("Whole word"));
        regexCheck_ = new QToolButton(this);
        regexCheck_->setObjectName(QStringLiteral("findBarRegex"));
        regexCheck_->setText(QStringLiteral(".*"));
        regexCheck_->setToolTip(tr("Regular expression"));
        for (QToolButton* toggle : {caseCheck_, wholeWordCheck_, regexCheck_})
        {
            toggle->setCheckable(true);
            toggle->setAutoRaise(true);
            toggle->setFixedWidth(toggle->fontMetrics().horizontalAdvance(toggle->text()) + 14);
        }

        statusLabel_ = new QLabel(this);
        statusLabel_->setObjectName(QStringLiteral("findBarStatus"));

        prevButton_ = new QToolButton(this);
        prevButton_->setObjectName(QStringLiteral("findBarPrevious"));
        prevButton_->setText(QStringLiteral("▲"));
        prevButton_->setToolTip(tr("Previous match"));
        nextButton_ = new QToolButton(this);
        nextButton_->setObjectName(QStringLiteral("findBarNext"));
        nextButton_->setText(QStringLiteral("▼"));
        nextButton_->setToolTip(tr("Next match"));
        closeButton_ = new QToolButton(this);
        closeButton_->setText(QStringLiteral("✕"));
        closeButton_->setObjectName(QStringLiteral("findBarClose"));
        closeButton_->setToolTip(tr("Close (Esc)"));

        replaceButton_ = new QToolButton(this);
        replaceButton_->setObjectName(QStringLiteral("findBarReplaceOne"));
        replaceButton_->setText(tr("Replace"));
        replaceAllButton_ = new QToolButton(this);
        replaceAllButton_->setObjectName(QStringLiteral("findBarReplaceAll"));
        replaceAllButton_->setText(tr("Replace All"));

        auto* findRow = new QHBoxLayout;
        findRow->addWidget(searchEdit_);
        findRow->addWidget(caseCheck_);
        findRow->addWidget(wholeWordCheck_);
        findRow->addWidget(regexCheck_);
        findRow->addWidget(prevButton_);
        findRow->addWidget(nextButton_);
        findRow->addWidget(statusLabel_);
        findRow->addWidget(closeButton_);

        replaceRow_ = new QWidget(this);
        auto* replaceRowLayout = new QHBoxLayout(replaceRow_);
        replaceRowLayout->setContentsMargins(0, 0, 0, 0);
        replaceRowLayout->addWidget(replaceEdit_);
        replaceRowLayout->addWidget(replaceButton_);
        replaceRowLayout->addWidget(replaceAllButton_);
        replaceRow_->hide();

        auto* layout = new QVBoxLayout(this);
        layout->addLayout(findRow);
        layout->addWidget(replaceRow_);

        connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString&) { emit findRequested(false); });
        connect(searchEdit_, &QLineEdit::returnPressed, this, [this] { emit findRequested(false); });
        connect(prevButton_, &QToolButton::clicked, this, [this] { emit findRequested(true); });
        connect(nextButton_, &QToolButton::clicked, this, [this] { emit findRequested(false); });
        connect(replaceButton_, &QToolButton::clicked, this, &FindBar::replaceRequested);
        connect(replaceAllButton_, &QToolButton::clicked, this, &FindBar::replaceAllRequested);
        connect(closeButton_, &QToolButton::clicked, this, &FindBar::closeRequested);
    }

    QString searchTerm() const { return searchEdit_->text(); }
    QString replaceText() const { return replaceEdit_->text(); }
    bool isCaseSensitive() const { return caseCheck_->isChecked(); }
    bool isWholeWord() const { return wholeWordCheck_->isChecked(); }
    bool isRegex() const { return regexCheck_->isChecked(); }

    void setSearchTerm(const QString& text) { searchEdit_->setText(text); }
    void setReplaceVisible(bool show) { replaceRow_->setVisible(show); }

    void focusSearchField()
    {
        searchEdit_->setFocus();
        searchEdit_->selectAll();
    }

    void setStatus(const QString& text, bool isError)
    {
        statusLabel_->setText(text);
        QPalette p = statusLabel_->palette();
        p.setColor(QPalette::WindowText, isError ? QColor(Qt::red) : QColor(Qt::darkGray));
        statusLabel_->setPalette(p);
    }

signals:
    void findRequested(bool backward);
    void replaceRequested();
    void replaceAllRequested();
    void closeRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape)
        {
            emit closeRequested();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    QLineEdit*   searchEdit_;
    QLineEdit*   replaceEdit_;
    QToolButton* caseCheck_;
    QToolButton* wholeWordCheck_;
    QToolButton* regexCheck_;
    QLabel*      statusLabel_;
    QToolButton* prevButton_;
    QToolButton* nextButton_;
    QToolButton* replaceButton_;
    QToolButton* replaceAllButton_;
    QToolButton* closeButton_;
    QWidget*     replaceRow_;
};

ScriptEditor::ScriptEditor(QWidget* parent)
    : QPlainTextEdit(parent), watcher_(new QFileSystemWatcher(this)), highlighter_(new RevHighlighter(document())),
      lineNumberArea_(new LineNumberArea(this)), findBar_(new FindBar(this))
{
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setTabStopDistance(4 * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    setLineWrapMode(QPlainTextEdit::NoWrap);

    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]
    {
        emit cursorPositionMoved(cursorLine(), cursorColumn());
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &ScriptEditor::highlightCurrentLineAndMatchingBracket);
    connect(this, &QPlainTextEdit::blockCountChanged, this, &ScriptEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &ScriptEditor::updateLineNumberArea);

    updateLineNumberAreaWidth();
    highlightCurrentLineAndMatchingBracket();

    findBar_->hide();
    connect(findBar_, &FindBar::findRequested, this, &ScriptEditor::findNext);
    connect(findBar_, &FindBar::replaceRequested, this, &ScriptEditor::replaceCurrent);
    connect(findBar_, &FindBar::replaceAllRequested, this, &ScriptEditor::replaceAllMatches);
    connect(findBar_, &FindBar::closeRequested, this, [this]
    {
        findBar_->hide();
        setFocus();
    });

    connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString&)
    {
        if (suppressNextChange_)
        {
            suppressNextChange_ = false;
        }
        else
        {
            emit externallyModified();
        }
        // QFileSystemWatcher can drop a path after an atomic (rename-based) rewrite of the file, including our
        // own saveToFile(): re-arm so the next change -- external or, after another save, suppressed -- is
        // still caught.
        if (!filePath_.isEmpty())
        {
            watcher_->addPath(filePath_);
        }
    });
}

bool ScriptEditor::loadFromFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
        {
            *error = file.errorString();
        }
        return false;
    }
    QByteArray bytes = file.readAll();
    file.close();

    hadBom_ = bytes.startsWith("\xEF\xBB\xBF");
    if (hadBom_)
    {
        bytes.remove(0, 3);
    }
    lineEnding_ = bytes.contains("\r\n") ? "\r\n" : "\n";

    QString text = QString::fromUtf8(bytes);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));   // a lone CR (old Mac style): still a line break

    setPlainText(text);
    document()->setModified(false);

    if (!filePath_.isEmpty() && filePath_ != path)
    {
        watcher_->removePath(filePath_);
    }
    filePath_ = path;
    if (!watcher_->files().contains(path))
    {
        watcher_->addPath(path);
    }
    return true;
}

bool ScriptEditor::saveToFile(const QString& path, QString* error)
{
    QString text = toPlainText();
    if (lineEnding_ == "\r\n")
    {
        text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    }
    QByteArray bytes = text.toUtf8();
    if (hadBom_)
    {
        bytes.prepend("\xEF\xBB\xBF");
    }

    suppressNextChange_ = true;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) < 0 || !file.commit())
    {
        suppressNextChange_ = false;
        if (error)
        {
            *error = file.errorString();
        }
        return false;
    }

    if (!filePath_.isEmpty() && filePath_ != path)
    {
        watcher_->removePath(filePath_);
    }
    filePath_ = path;
    document()->setModified(false);
    if (!watcher_->files().contains(path))
    {
        watcher_->addPath(path);
    }
    return true;
}

void ScriptEditor::applyFont(const QFont& font)
{
    setFont(font);
    setTabStopDistance(4 * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    updateLineNumberAreaWidth();   // digit width changed along with everything else
}

int ScriptEditor::cursorLine() const
{
    return textCursor().blockNumber() + 1;
}

int ScriptEditor::cursorColumn() const
{
    return textCursor().positionInBlock() + 1;
}

void ScriptEditor::goToLine(int line)
{
    const int blockNumber = qBound(0, line - 1, document()->blockCount() - 1);
    setTextCursor(QTextCursor(document()->findBlockByNumber(blockNumber)));
    ensureCursorVisible();
    setFocus();
}

QString ScriptEditor::wordUnderCursor() const
{
    // A manual scan against Rev's own identifier characters (src/grammar/lex.l's ID rule allows a leading
    // underscore too), not QTextCursor::WordUnderCursor: Qt's own notion of a "word" (Unicode text-boundary
    // rules) does not treat '_' as part of one, which a Rev identifier like `my_var` very much is.
    const QTextCursor cursor = textCursor();
    const QString lineText = cursor.block().text();
    const int pos = cursor.positionInBlock();

    const auto isIdentifierChar = [](QChar ch) { return ch.isLetterOrNumber() || ch == QLatin1Char('_'); };

    int start = pos;
    while (start > 0 && isIdentifierChar(lineText.at(start - 1)))
    {
        --start;
    }
    int end = pos;
    while (end < lineText.length() && isIdentifierChar(lineText.at(end)))
    {
        ++end;
    }
    return lineText.mid(start, end - start);
}

QString ScriptEditor::currentStatementText(int* endBlock) const
{
    int start = 0;
    int end = 0;
    currentStatementRange(&start, &end);
    if (endBlock)
    {
        *endBlock = end;
    }

    QStringList lines;
    for (int i = start; i <= end; ++i)
    {
        lines << document()->findBlockByNumber(i).text();
    }
    return lines.join(QLatin1Char('\n'));
}

void ScriptEditor::currentStatementRange(int* startBlock, int* endBlock) const
{
    const int cursorBlock = textCursor().blockNumber();
    const int blockCount = document()->blockCount();

    // depthBefore[i]: ( ) { } [ ] nesting depth at the START of block i, found by scanning every block from the
    // top of the document -- cheap enough for a script-sized buffer, and the only way to know, for a line deep
    // inside a multi-line statement, where that statement actually began.
    QVector<int> depthBefore(blockCount);
    int depth = 0;
    bool inString = false;
    for (int i = 0; i < blockCount; ++i)
    {
        depthBefore[i] = depth;
        const QString text = document()->findBlockByNumber(i).text();
        inString = scanCodeCharacters(text, inString, [&](int k)
        {
            const QChar ch = text.at(k);
            if (ch == QLatin1Char('(') || ch == QLatin1Char('{') || ch == QLatin1Char('['))
            {
                ++depth;
            }
            else if (ch == QLatin1Char(')') || ch == QLatin1Char('}') || ch == QLatin1Char(']'))
            {
                depth = depth > 0 ? depth - 1 : 0;
            }
        });
    }

    int start = cursorBlock;
    while (start > 0 && depthBefore[start] != 0)
    {
        --start;
    }
    int end = start > cursorBlock ? start : cursorBlock;
    while (end + 1 < blockCount && depthBefore[end + 1] != 0)
    {
        ++end;
    }

    if (startBlock)
    {
        *startBlock = start;
    }
    if (endBlock)
    {
        *endBlock = end;
    }
}

int ScriptEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    for (int blocks = qMax(1, document()->blockCount()); blocks >= 10; blocks /= 10)
    {
        ++digits;
    }
    return 8 + QFontMetrics(lineNumberFont()).horizontalAdvance(QLatin1Char('9')) * digits;
}

QFont ScriptEditor::lineNumberFont() const
{
    // Smaller than the code itself, on request -- numbers are a reference, not something to read character by
    // character, and take up less of the gutter's (already narrow) width this way.
    QFont smaller = font();
    if (smaller.pointSize() > 0)
    {
        smaller.setPointSize(qMax(6, smaller.pointSize() - 2));
    }
    else if (smaller.pixelSize() > 0)
    {
        smaller.setPixelSize(qMax(8, static_cast<int>(smaller.pixelSize() * 0.8)));
    }
    return smaller;
}

void ScriptEditor::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void ScriptEditor::updateLineNumberArea(const QRect& rect, int dy)
{
    if (dy)
    {
        lineNumberArea_->scroll(0, dy);
    }
    else
    {
        lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
    }
    if (rect.contains(viewport()->rect()))
    {
        updateLineNumberAreaWidth();
    }
}

void ScriptEditor::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect area = contentsRect();
    lineNumberArea_->setGeometry(QRect(area.left(), area.top(), lineNumberAreaWidth(), area.height()));

    // The find bar floats over the top-right of the text area rather than taking a layout row of its own, so
    // showing or hiding it never shifts anything else.
    const int barWidth = qMax(220, qMin(420, area.width() - 16));
    findBar_->setGeometry(area.right() - barWidth - 8, area.top() + 4, barWidth, findBar_->sizeHint().height());
    findBar_->raise();
}

void ScriptEditor::lineNumberAreaPaintEvent(QPaintEvent* event)
{
    QPainter painter(lineNumberArea_);
    painter.fillRect(event->rect(), palette().color(QPalette::Window));
    painter.setFont(lineNumberFont());

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    painter.setPen(palette().color(QPalette::WindowText));
    while (block.isValid() && top <= event->rect().bottom())
    {
        if (block.isVisible() && bottom >= event->rect().top() && (blockNumber + 1) % lineNumberInterval == 0)
        {
            painter.drawText(0, top, lineNumberArea_->width() - 4, bottom - top, Qt::AlignRight,
                              QString::number(blockNumber + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void ScriptEditor::highlightCurrentLineAndMatchingBracket()
{
    QList<QTextEdit::ExtraSelection> selections;

    QTextEdit::ExtraSelection currentLine;
    currentLine.format.setBackground(palette().color(QPalette::AlternateBase));
    currentLine.format.setProperty(QTextFormat::FullWidthSelection, true);
    currentLine.cursor = textCursor();
    currentLine.cursor.clearSelection();
    selections.append(currentLine);

    // Bracket matching (Q5): only when the cursor is actually next to one, so the whole-buffer scan below runs
    // on the (much rarer) occasions that need it, not on every cursor move through plain text.
    static const QString brackets = QStringLiteral("()[]{}");
    const QString text = toPlainText();
    const int pos = textCursor().position();
    int bracketPos = -1;
    if (pos < text.length() && brackets.contains(text.at(pos)))
    {
        bracketPos = pos;
    }
    else if (pos > 0 && brackets.contains(text.at(pos - 1)))
    {
        bracketPos = pos - 1;
    }

    if (bracketPos >= 0)
    {
        const int matchPos = matchingBracketPosition(bracketPos);
        if (matchPos >= 0)
        {
            QTextCharFormat matchFormat;
            QColor highlight = palette().color(QPalette::Highlight);
            highlight.setAlpha(110);   // a tint, not a full selection -- this is not something the user selected
            matchFormat.setBackground(highlight);
            matchFormat.setFontWeight(QFont::Bold);

            for (const int p : {bracketPos, matchPos})
            {
                QTextEdit::ExtraSelection selection;
                selection.format = matchFormat;
                selection.cursor = QTextCursor(document());
                selection.cursor.setPosition(p);
                selection.cursor.setPosition(p + 1, QTextCursor::KeepAnchor);
                selections.append(selection);
            }
        }
    }

    setExtraSelections(selections);
}

int ScriptEditor::matchingBracketPosition(int bracketPosition) const
{
    const QString text = toPlainText();
    if (bracketPosition < 0 || bracketPosition >= text.length())
    {
        return -1;
    }

    static const QString openers = QStringLiteral("([{");
    static const QString closers = QStringLiteral(")]}");
    const QChar ch = text.at(bracketPosition);
    const int openerIndex = openers.indexOf(ch);
    const int closerIndex = closers.indexOf(ch);
    if (openerIndex < 0 && closerIndex < 0)
    {
        return -1;
    }
    const bool forward = openerIndex >= 0;
    const QChar openChar = forward ? ch : openers.at(closerIndex);
    const QChar closeChar = forward ? closers.at(openerIndex) : ch;

    // A mask of positions NOT inside a string or a comment (same scan currentStatementRange() uses), so a
    // bracket mentioned in either is correctly ignored when looking for the real match.
    QVector<bool> isCode(text.length(), false);
    bool inString = false;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next())
    {
        const QString blockText = block.text();
        const int base = block.position();
        inString = scanCodeCharacters(blockText, inString, [&](int k)
        {
            if (base + k < isCode.size())
            {
                isCode[base + k] = true;
            }
        });
    }

    int depth = 0;
    if (forward)
    {
        for (int i = bracketPosition; i < text.length(); ++i)
        {
            if (!isCode[i])
            {
                continue;
            }
            if (text.at(i) == openChar)
            {
                ++depth;
            }
            else if (text.at(i) == closeChar && --depth == 0)
            {
                return i;
            }
        }
    }
    else
    {
        for (int i = bracketPosition; i >= 0; --i)
        {
            if (!isCode[i])
            {
                continue;
            }
            if (text.at(i) == closeChar)
            {
                ++depth;
            }
            else if (text.at(i) == openChar && --depth == 0)
            {
                return i;
            }
        }
    }
    return -1;
}

void ScriptEditor::showFindBar(bool withReplace)
{
    findBar_->setReplaceVisible(withReplace);
    if (textCursor().hasSelection())
    {
        findBar_->setSearchTerm(textCursor().selectedText());   // the common convention: select a word, Ctrl+F
    }
    findBar_->show();
    findBar_->raise();
    findBar_->focusSearchField();
}

namespace {
QTextDocument::FindFlags findFlagsFor(const FindBar& bar, bool backward)
{
    QTextDocument::FindFlags flags;
    if (bar.isCaseSensitive())
    {
        flags |= QTextDocument::FindCaseSensitively;
    }
    if (bar.isWholeWord())
    {
        flags |= QTextDocument::FindWholeWords;
    }
    if (backward)
    {
        flags |= QTextDocument::FindBackward;
    }
    return flags;
}
} // namespace

void ScriptEditor::findNext(bool backward)
{
    const QString term = findBar_->searchTerm();
    if (term.isEmpty())
    {
        findBar_->setStatus(QString(), false);
        return;
    }

    const QTextDocument::FindFlags flags = findFlagsFor(*findBar_, backward);
    QTextCursor found;
    if (findBar_->isRegex())
    {
        const QRegularExpression re(term, findBar_->isCaseSensitive() ? QRegularExpression::NoPatternOption
                                                                        : QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid())
        {
            findBar_->setStatus(tr("Invalid regex"), true);
            return;
        }
        found = document()->find(re, textCursor(), flags);
        if (found.isNull())
        {
            QTextCursor wrap(document());
            if (backward)
            {
                wrap.movePosition(QTextCursor::End);
            }
            found = document()->find(re, wrap, flags);
        }
    }
    else
    {
        found = document()->find(term, textCursor(), flags);
        if (found.isNull())
        {
            QTextCursor wrap(document());
            if (backward)
            {
                wrap.movePosition(QTextCursor::End);
            }
            found = document()->find(term, wrap, flags);
        }
    }

    if (found.isNull())
    {
        findBar_->setStatus(tr("No matches"), true);
        return;
    }
    setTextCursor(found);
    ensureCursorVisible();
    findBar_->setStatus(tr("%1 match(es)").arg(countMatches()), false);
}

void ScriptEditor::replaceCurrent()
{
    const QString term = findBar_->searchTerm();
    if (term.isEmpty())
    {
        return;
    }

    QTextCursor cursor = textCursor();
    bool selectionMatches = false;
    if (cursor.hasSelection())
    {
        const QString selected = cursor.selectedText();
        if (findBar_->isRegex())
        {
            const QRegularExpression re(term, findBar_->isCaseSensitive()
                                                   ? QRegularExpression::NoPatternOption
                                                   : QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch match = re.match(selected);
            selectionMatches = re.isValid() && match.hasMatch() && match.capturedStart() == 0
                                && match.capturedLength() == selected.length();
        }
        else
        {
            selectionMatches = findBar_->isCaseSensitive() ? selected == term
                                                             : selected.compare(term, Qt::CaseInsensitive) == 0;
        }
    }

    // Only replaces a selection that is actually the match just found (not whatever the user happened to have
    // selected some other way); either way, moves on to the next occurrence, matching "Replace" buttons
    // elsewhere that always leave you positioned at the next thing to look at.
    if (selectionMatches)
    {
        cursor.insertText(findBar_->replaceText());
        setTextCursor(cursor);
    }
    findNext(false);
}

void ScriptEditor::replaceAllMatches()
{
    const QString term = findBar_->searchTerm();
    if (term.isEmpty())
    {
        return;
    }

    const QTextDocument::FindFlags flags = findFlagsFor(*findBar_, false);
    QRegularExpression re;
    if (findBar_->isRegex())
    {
        re = QRegularExpression(term, findBar_->isCaseSensitive() ? QRegularExpression::NoPatternOption
                                                                    : QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid())
        {
            findBar_->setStatus(tr("Invalid regex"), true);
            return;
        }
    }

    QTextCursor cursor(document());
    QTextCursor editBlock(document());
    editBlock.beginEditBlock();
    int count = 0;
    while (true)
    {
        QTextCursor found = findBar_->isRegex() ? document()->find(re, cursor, flags) : document()->find(term, cursor, flags);
        if (found.isNull())
        {
            break;
        }
        found.insertText(findBar_->replaceText());
        cursor = found;   // insertText() collapses the selection to just after the replacement, so the next
                           // find() starts there -- never re-matching what was just inserted.
        ++count;
    }
    editBlock.endEditBlock();

    findBar_->setStatus(count == 0 ? tr("No matches") : tr("Replaced %1").arg(count), count == 0);
}

int ScriptEditor::countMatches() const
{
    const QString term = findBar_->searchTerm();
    if (term.isEmpty())
    {
        return 0;
    }

    const QTextDocument::FindFlags flags = findFlagsFor(*findBar_, false);
    QRegularExpression re;
    if (findBar_->isRegex())
    {
        re = QRegularExpression(term, findBar_->isCaseSensitive() ? QRegularExpression::NoPatternOption
                                                                    : QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid())
        {
            return 0;
        }
    }

    int count = 0;
    QTextCursor cursor(document());
    while (true)
    {
        QTextCursor found = findBar_->isRegex() ? document()->find(re, cursor, flags) : document()->find(term, cursor, flags);
        if (found.isNull())
        {
            break;
        }
        ++count;
        cursor = found;
    }
    return count;
}

void ScriptEditor::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F && event->modifiers() == Qt::ControlModifier)
    {
        showFindBar(false);
        return;
    }
    if (event->key() == Qt::Key_H && event->modifiers() == Qt::ControlModifier)
    {
        showFindBar(true);
        return;
    }
    if (event->key() == Qt::Key_Slash && event->modifiers() == Qt::ControlModifier)
    {
        toggleCommentOnSelectedLines();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && event->modifiers() == Qt::NoModifier)
    {
        indentNewLine();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

void ScriptEditor::indentNewLine()
{
    QTextCursor cursor = textCursor();
    const QString lineText = cursor.block().text();

    QString leadingWhitespace;
    for (const QChar ch : lineText)
    {
        if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t'))
        {
            leadingWhitespace += ch;
        }
        else
        {
            break;
        }
    }
    if (lineText.trimmed().endsWith(QLatin1Char('{')))
    {
        leadingWhitespace += QStringLiteral("    ");
    }

    cursor.insertText(QLatin1Char('\n') + leadingWhitespace);
    setTextCursor(cursor);
}

void ScriptEditor::toggleCommentOnSelectedLines()
{
    const QTextCursor selection = textCursor();
    const int startBlock = document()->findBlock(selection.selectionStart()).blockNumber();
    const int endBlock   = document()->findBlock(selection.selectionEnd()).blockNumber();

    const bool shouldUncomment = document()->findBlockByNumber(startBlock).text().trimmed().startsWith(QLatin1Char('#'));

    QTextCursor editCursor(document());
    editCursor.beginEditBlock();
    for (int blockNumber = startBlock; blockNumber <= endBlock; ++blockNumber)
    {
        const QTextBlock block = document()->findBlockByNumber(blockNumber);
        const QString text = block.text();

        if (shouldUncomment)
        {
            const int hashPos = text.indexOf(QLatin1Char('#'));
            if (hashPos >= 0 && text.left(hashPos).trimmed().isEmpty())
            {
                QTextCursor lineCursor(block);
                lineCursor.setPosition(block.position() + hashPos);
                int removeCount = 1;
                if (hashPos + 1 < text.size() && text.at(hashPos + 1) == QLatin1Char(' '))
                {
                    removeCount = 2;   // the conventional "# " this toggle itself inserts
                }
                lineCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, removeCount);
                lineCursor.removeSelectedText();
            }
        }
        else
        {
            QTextCursor lineCursor(block);
            lineCursor.insertText(QStringLiteral("# "));
        }
    }
    editCursor.endEditBlock();
}

} // namespace revstudio

#include "ScriptEditor.moc"
