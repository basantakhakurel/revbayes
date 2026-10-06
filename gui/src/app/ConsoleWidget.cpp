#include "app/ConsoleWidget.h"

#include "app/RevString.h"

#include <QAbstractItemView>
#include <QColor>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QStandardPaths>
#include <QStringListModel>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextStream>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <optional>

namespace revstudio {

namespace {

constexpr int flushIntervalMs = 16;    // GUI_Implementation_Note.md 6.9: coalesce rapid output onto one timer tick
constexpr int maxInputLines   = 6;
constexpr int stopTimeoutMs   = 5000;   // the fallback table's own number for "interrupt not honoured"

// Q7 error links: the line number an anchor's QTextCharFormat carries alongside its anchorHref() (the path),
// set in ConsoleWidget::linkifyErrorLines() and read back in ConsoleOutputEdit::lineAt().
constexpr int errorLinkLineProperty = QTextFormat::UserProperty + 1;

/** Persisted history entries are one per line; a submitted statement may itself contain newlines (Shift+Enter),
 *  so they are escaped on the way to disk and restored on the way back. Does not handle a literal "\n" the user
 *  typed as two characters -- an acceptable corner case for a history file, not a protocol. */
QString escapeForHistoryFile(const QString& text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return escaped;
}

QString unescapeFromHistoryFile(const QString& text)
{
    QString unescaped = text;
    unescaped.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    return unescaped;
}

/** `complete`'s `cursor`/`replace_from` are UTF-8 byte offsets (doc/server-protocol.md), not QString character
 *  indices -- this converts a byte offset back to one, against the exact buffer it was computed from. Rev
 *  identifiers are ASCII (src/grammar/lex.l's ID rule), so this is exact for everything completion actually
 *  completes; a non-ASCII character elsewhere in the input could only make it land one QChar off, never crash. */
int charIndexForUtf8Offset(const QString& text, int utf8Offset)
{
    if (utf8Offset <= 0)
    {
        return 0;
    }
    for (int i = 1; i <= text.length(); ++i)
    {
        if (text.left(i).toUtf8().size() >= utf8Offset)
        {
            return i;
        }
    }
    return text.length();
}

/** `std::filesystem::path`'s stream insertion operator (RevClient.cpp's "Problem processing line N in file F")
 *  quotes the path and escapes `"` as `\"` and `\` as `\\` -- the inverse of `std::quoted`, for the path the
 *  error-link regex below captures still escaped. */
QString unescapeQuotedPath(const QString& text)
{
    QString result;
    result.reserve(text.length());
    for (int i = 0; i < text.length(); ++i)
    {
        if (text.at(i) == QLatin1Char('\\') && i + 1 < text.length())
        {
            ++i;
        }
        result.append(text.at(i));
    }
    return result;
}

} // namespace

/**
 * The input editor. Plain C++ callbacks rather than Qt signals: it needs no signal/slot machinery of its own,
 * just to tell ConsoleWidget "submit now" or "give me the history entry in this direction", so a private class
 * with no Q_OBJECT (and so no moc step) is enough -- it still inherits QPlainTextEdit's own QObject-ness, which
 * is all connect() below needs for lifetime tracking.
 *
 * Q3: setCompleter() wires up a QCompleter the manual way -- the "Custom Completer" pattern from Qt's own
 * examples, needed because QPlainTextEdit (unlike QLineEdit) has no setCompleter() of its own. Enter/Return/
 * Escape/Tab/Backtab are left to the completer while its popup is visible, matching that same example, rather
 * than being claimed by onSubmit/onTabComplete below.
 */
class ConsoleInputEdit : public QPlainTextEdit
{
public:
    explicit ConsoleInputEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent)
    {
        setTabChangesFocus(false);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        connect(document(), &QTextDocument::contentsChanged, this, [this] { updateHeight(); });
        updateHeight();
    }

    void setCompleter(QCompleter* completer)
    {
        completer_ = completer;
        if (completer_)
        {
            completer_->setWidget(this);
        }
    }

    std::function<void()>                           onSubmit;
    std::function<void()>                           onTabComplete;
    std::function<std::optional<QString>(QString)>   onHistoryUp;     // argument: the text currently shown
    std::function<std::optional<QString>()>          onHistoryDown;

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (completer_ && completer_->popup()->isVisible())
        {
            switch (event->key())
            {
            case Qt::Key_Enter:
            case Qt::Key_Return:
            case Qt::Key_Escape:
            case Qt::Key_Tab:
            case Qt::Key_Backtab:
                event->ignore();   // the completer's own event filter (installed by setWidget()) handles these
                return;
            default:
                break;
            }
        }

        const bool isReturn = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        if (isReturn && !(event->modifiers() & Qt::ShiftModifier))
        {
            if (onSubmit)
            {
                onSubmit();
            }
            return;
        }

        if (event->key() == Qt::Key_Tab && onTabComplete)
        {
            onTabComplete();
            return;
        }

        if (event->key() == Qt::Key_Up && textCursor().blockNumber() == 0 && onHistoryUp)
        {
            if (const auto replacement = onHistoryUp(toPlainText()))
            {
                setPlainTextAndMoveToEnd(*replacement);
                return;
            }
        }
        if (event->key() == Qt::Key_Down && textCursor().blockNumber() == document()->blockCount() - 1 && onHistoryDown)
        {
            if (const auto replacement = onHistoryDown())
            {
                setPlainTextAndMoveToEnd(*replacement);
                return;
            }
        }

        QPlainTextEdit::keyPressEvent(event);
    }

    // GUI_Implementation_Note.md 6.7: "Dragging a file into the console... inserts its quoted path." A file
    // drag carries text/uri-list, not text/plain, so the default QPlainTextEdit behaviour (which only accepts
    // plain text) needs widening -- the usual override pair for customising what a Qt text edit accepts.
    bool canInsertFromMimeData(const QMimeData* source) const override
    {
        return source->hasUrls() || QPlainTextEdit::canInsertFromMimeData(source);
    }

    void insertFromMimeData(const QMimeData* source) override
    {
        if (source->hasUrls())
        {
            QStringList quoted;
            for (const QUrl& url : source->urls())
            {
                if (url.isLocalFile())
                {
                    quoted.append(RevString::quote(url.toLocalFile()));
                }
            }
            if (!quoted.isEmpty())
            {
                QTextCursor cursor = textCursor();
                cursor.insertText(quoted.join(QStringLiteral(", ")));
                setTextCursor(cursor);
                return;
            }
        }
        QPlainTextEdit::insertFromMimeData(source);
    }

private:
    void setPlainTextAndMoveToEnd(const QString& text)
    {
        setPlainText(text);
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::End);
        setTextCursor(cursor);
    }

    void updateHeight()
    {
        const int lines = qBound(1, document()->blockCount(), maxInputLines);
        const QFontMetrics metrics(font());
        const QMargins margins = contentsMargins();
        setFixedHeight(metrics.lineSpacing() * lines + margins.top() + margins.bottom() + 10);
    }

    QCompleter* completer_ = nullptr;
};

/**
 * Q7's error navigation: `QPlainTextEdit` has no built-in link-click handling (that is `QTextBrowser`'s job),
 * so this replicates just the part needed -- cursorForPosition() to find the character under the mouse, then
 * the format's own isAnchor()/anchorHref() at that position, the standard technique for a plain text edit that
 * still has to support clickable anchors applied via QTextCharFormat.
 */
class ConsoleOutputEdit : public QPlainTextEdit
{
public:
    explicit ConsoleOutputEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent)
    {
        viewport()->setMouseTracking(true);
    }

    std::function<void(const QString&, int)> onLinkActivated;   // href, line

protected:
    void mouseMoveEvent(QMouseEvent* event) override
    {
        viewport()->setCursor(anchorAt(event->pos()).isEmpty() ? Qt::IBeamCursor : Qt::PointingHandCursor);
        QPlainTextEdit::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        const QString href = anchorAt(event->pos());
        if (!href.isEmpty() && onLinkActivated)
        {
            onLinkActivated(href, lineAt(event->pos()));
            return;
        }
        QPlainTextEdit::mouseReleaseEvent(event);
    }

private:
    QString anchorAt(const QPoint& pos) const
    {
        const QTextCharFormat format = cursorForPosition(pos).charFormat();
        return format.isAnchor() ? format.anchorHref() : QString();
    }

    int lineAt(const QPoint& pos) const
    {
        return cursorForPosition(pos).charFormat().property(errorLinkLineProperty).toInt();
    }
};

ConsoleWidget::ConsoleWidget(Session* session, QWidget* parent)
    : QWidget(parent), session_(session), output_(new ConsoleOutputEdit(this)), prompt_(new QLabel(this)),
      input_(new ConsoleInputEdit(this)), backendLog_(new QPlainTextEdit(this))
{
    static_cast<ConsoleOutputEdit*>(output_)->onLinkActivated = [this](const QString& path, int line)
    {
        const QString resolved = QFileInfo(path).isAbsolute() ? path : QDir(session_->cwd()).filePath(path);
        emit errorLinkActivated(resolved, line);
    };

    output_->setObjectName(QStringLiteral("consoleOutput"));
    output_->setReadOnly(true);
    output_->setMaximumBlockCount(100000);
    output_->setPlaceholderText(tr("Output appears here once the backend is ready."));

    input_->setObjectName(QStringLiteral("consoleInput"));

    backendLog_->setObjectName(QStringLiteral("consoleBackendLog"));
    backendLog_->setReadOnly(true);
    backendLog_->setMaximumBlockCount(100000);
    backendLog_->setPlaceholderText(tr("stderr and protocol activity appear here."));

    prompt_->setFixedWidth(QFontMetrics(prompt_->font()).horizontalAdvance(QStringLiteral("+++")));

    auto* inputRow = new QHBoxLayout;
    inputRow->addWidget(prompt_);
    inputRow->addWidget(input_);

    auto* consoleTab = new QWidget(this);
    auto* consoleLayout = new QVBoxLayout(consoleTab);
    consoleLayout->setContentsMargins(0, 0, 0, 0);
    consoleLayout->addWidget(output_);
    consoleLayout->addLayout(inputRow);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(consoleTab, tr("Console"));
    tabs_->addTab(backendLog_, tr("Backend"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs_);

    flushTimer_.setSingleShot(true);
    flushTimer_.setInterval(flushIntervalMs);
    connect(&flushTimer_, &QTimer::timeout, this, &ConsoleWidget::flushPendingOutput);

    stopTimer_.setSingleShot(true);
    stopTimer_.setInterval(stopTimeoutMs);
    connect(&stopTimer_, &QTimer::timeout, this, &ConsoleWidget::onStopTimeout);

    input_->onSubmit = [this] { submitCurrentInput(); };
    input_->onHistoryUp = [this](const QString& current) { return history_.up(current); };
    input_->onHistoryDown = [this] { return history_.down(); };
    input_->onTabComplete = [this] { requestCompletion(); };

    // Q3: the model is re-filled with each completions reply (onCompletionsReceived()); the completer itself
    // trusts the backend's own filtering completely (replace_from/text already account for whatever prefix
    // matching happened server-side), so it is never given a prefix of its own to filter against on top of that.
    completionModel_ = new QStringListModel(this);
    completer_ = new QCompleter(completionModel_, this);
    completer_->setCaseSensitivity(Qt::CaseSensitive);
    input_->setCompleter(completer_);
    connect(completer_, QOverload<const QString&>::of(&QCompleter::activated), this, &ConsoleWidget::insertCompletion);
    connect(input_, &QPlainTextEdit::textChanged, this, &ConsoleWidget::maybeShowSignatureTooltip);

    auto* clearShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), output_);
    connect(clearShortcut, &QShortcut::activated, this, &ConsoleWidget::clearOutput);

    connect(session_, &Session::stateChanged, this, [this](Session::State state)
    {
        if (state != Session::State::Busy)
        {
            stopTimer_.stop();   // it recovered (or the backend is gone) before the kill-and-restart prompt fired
        }
        updateInputState();
    });
    connect(session_, &Session::ready, this, [this](const QString& version)
    {
        appendBackendLog(tr("Backend ready: %1 (working directory %2)").arg(version, session_->cwd()));
        updateInputState();
    });
    connect(session_, &Session::completionsReceived, this, &ConsoleWidget::onCompletionsReceived);
    connect(session_, &Session::functionsChanged, this, &ConsoleWidget::onFunctionsSnapshot);
    connect(session_, &Session::output, this, [this](const QString& stream, const QString& text, qint64)
    {
        appendOutputChunk(stream, text);
    });
    connect(session_, &Session::submitFinished, this, [this](qint64, const QString& status, const QJsonObject&)
    {
        continuation_ = (status == QLatin1String("incomplete"));
        updateInputState();
    });
    connect(session_, &Session::backendError, this, [this](qint64, const QString& code, const QString& message)
    {
        appendOutputChunk(QStringLiteral("stderr"), tr("[%1] %2\n").arg(code, message));
    });
    connect(session_, &Session::askRequested, this, [this](qint64 askId, const QString& question)
    {
        const auto choice = QMessageBox::question(this, tr("RevBayes"), question, QMessageBox::Yes | QMessageBox::No);
        session_->answer(askId, choice == QMessageBox::Yes);
    });
    connect(session_, &Session::backendQuit, this, [this](const QString& reason)
    {
        appendBackendLog(tr("[quit] %1").arg(reason));
    });
    connect(session_, &Session::backendStderr, this, [this](const QString& text)
    {
        appendBackendLog(QStringLiteral("[stderr] ") + text);
    });
    connect(session_, &Session::protocolWarning, this, [this](const QString& text)
    {
        appendBackendLog(QStringLiteral("[protocol] ") + text);
    });
    connect(session_, &Session::failed, this, [this](const QString& reason)
    {
        appendBanner(reason);
    });

    loadHistory();
    updateInputState();
}

void ConsoleWidget::clearOutput()
{
    output_->clear();
}

void ConsoleWidget::appendOutputChunk(const QString& stream, const QString& text)
{
    pendingOutput_.append({stream, text});
    if (!flushTimer_.isActive())
    {
        flushTimer_.start();
    }
}

void ConsoleWidget::flushPendingOutput()
{
    if (pendingOutput_.isEmpty())
    {
        return;
    }
    QScrollBar* bar = output_->verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum() - 2;

    for (const auto& chunk : pendingOutput_)
    {
        const QColor color = chunk.stream == QLatin1String("stderr") ? QColor(200, 70, 70) : QColor();
        appendFormatted(output_, chunk.text, color, false);
    }
    pendingOutput_.clear();

    if (wasAtBottom)
    {
        bar->setValue(bar->maximum());
    }
}

void ConsoleWidget::appendEcho(const QString& text)
{
    appendFormatted(output_, text + QLatin1Char('\n'), QColor(140, 140, 140), true);
}

void ConsoleWidget::appendBanner(const QString& reason)
{
    appendFormatted(output_, QStringLiteral("\n--- %1 ---\n").arg(reason), QColor(200, 70, 70), false);
}

void ConsoleWidget::appendBackendLog(const QString& text)
{
    backendLog_->appendPlainText(text);
}

void ConsoleWidget::appendFormatted(QPlainTextEdit* target, const QString& text, const QColor& color, bool italic)
{
    QTextCursor cursor(target->document());
    cursor.movePosition(QTextCursor::End);
    const int insertStart = cursor.position();
    QTextCharFormat format;
    if (color.isValid())
    {
        format.setForeground(color);
    }
    format.setFontItalic(italic);
    cursor.insertText(text, format);

    if (target == output_)
    {
        linkifyErrorLines(insertStart, text);
    }
}

void ConsoleWidget::linkifyErrorLines(int insertStart, const QString& insertedText)
{
    // RevClient.cpp's execute_file(): `throw RbException() << "Problem processing line " << lineNumber << " in
    // file " << filename;` (or the same text via RBOUT with continue_on_error) -- filename is a
    // std::filesystem::path, whose own operator<< quotes it and escapes '"' and '\' (see unescapeQuotedPath()).
    static const QRegularExpression pattern(
        QStringLiteral(R"RX(Problem processing line (\d+) in file "((?:[^"\\]|\\.)*)")RX"));

    QRegularExpressionMatchIterator it = pattern.globalMatch(insertedText);
    while (it.hasNext())
    {
        const QRegularExpressionMatch match = it.next();

        QTextCursor cursor(output_->document());
        cursor.setPosition(insertStart + match.capturedStart());
        cursor.setPosition(insertStart + match.capturedEnd(), QTextCursor::KeepAnchor);

        QTextCharFormat format = cursor.charFormat();
        format.setAnchor(true);
        format.setAnchorHref(unescapeQuotedPath(match.captured(2)));
        format.setProperty(errorLinkLineProperty, match.captured(1).toInt());
        format.setForeground(palette().color(QPalette::Link));
        format.setFontUnderline(true);
        cursor.setCharFormat(format);
    }
}

void ConsoleWidget::submitCurrentInput()
{
    const QString text = input_->toPlainText();
    if (text.trimmed().isEmpty())
    {
        return;
    }
    submit(text);
    input_->clear();
}

void ConsoleWidget::submitText(const QString& text)
{
    if (text.trimmed().isEmpty())
    {
        return;
    }
    submit(text);
}

void ConsoleWidget::insertText(const QString& text)
{
    QTextCursor cursor = input_->textCursor();
    cursor.insertText(text);
    input_->setTextCursor(cursor);   // belt and suspenders: QTextCursor already keeps the widget's own cursor in
                                      // sync on edits at its position (verified -- a repeated insertText() lands
                                      // after the previous one with or without this line), but this is the
                                      // documented, explicit way to do it and costs nothing extra.
    input_->setFocus();
}

void ConsoleWidget::requestStop()
{
    if (session_->state() != Session::State::Busy)
    {
        return;
    }
    session_->interrupt();
    appendBackendLog(tr("Stop requested. If the backend does not respond, you will be offered the option to kill "
                         "and restart it."));
    stopTimer_.start();
}

void ConsoleWidget::onStopTimeout()
{
    if (session_->state() != Session::State::Busy)
    {
        return;   // it finished (or the backend is already gone) between the timer firing and this running
    }
    const auto choice = QMessageBox::question(this, tr("Backend Not Responding"),
        tr("The backend has not responded to Stop.\n\nKill and restart it? This discards the current Rev "
           "session (variables, loaded models). Open editor tabs are not affected."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice == QMessageBox::Yes)
    {
        appendBackendLog(tr("Killing and restarting the backend..."));
        session_->restart();
    }
}

void ConsoleWidget::requestCompletion()
{
    if (session_->state() != Session::State::Ready)
    {
        return;   // "Idle only" (doc/server-protocol.md); Tab is still consumed (see ConsoleInputEdit), just
                  // quietly does nothing while Busy rather than inserting a literal tab character.
    }
    const QString buffer = input_->toPlainText();
    const int cursorChar = input_->textCursor().position();

    pendingCompletionBuffer_ = buffer;
    pendingCompletionCursorChar_ = cursorChar;
    pendingCompletionRequestId_ = session_->requestComplete(buffer, buffer.left(cursorChar).toUtf8().size());
}

void ConsoleWidget::onCompletionsReceived(qint64 requestId, int replaceFrom, const QJsonArray& items)
{
    if (requestId != pendingCompletionRequestId_ || items.isEmpty())
    {
        return;   // stale (the user asked again, or typed more, before this reply arrived) or nothing to offer
    }

    // replaceFrom is a UTF-8 byte offset (doc/server-protocol.md); convert it against the exact buffer it was
    // computed from, not whatever the input shows now -- see charIndexForUtf8Offset()'s own comment on why an
    // identical conversion the other way round is used to build the request in the first place.
    completionReplaceFromChar_ = charIndexForUtf8Offset(pendingCompletionBuffer_, replaceFrom);

    QStringList texts;
    for (const QJsonValue& value : items)
    {
        texts.append(value.toObject().value(QStringLiteral("text")).toString());
    }

    if (texts.size() == 1)
    {
        insertCompletion(texts.first());   // nothing to choose between: no popup needed
        return;
    }
    completionModel_->setStringList(texts);
    completer_->setCompletionPrefix(QString());
    completer_->complete(input_->cursorRect());
}

void ConsoleWidget::insertCompletion(const QString& text)
{
    QTextCursor cursor = input_->textCursor();
    cursor.setPosition(completionReplaceFromChar_);
    cursor.setPosition(pendingCompletionCursorChar_, QTextCursor::KeepAnchor);
    cursor.insertText(text.mid(completionReplaceFromChar_));
    input_->setTextCursor(cursor);
    input_->setFocus();
}

void ConsoleWidget::onFunctionsSnapshot(const QJsonArray& rows)
{
    functionSignatures_.clear();
    for (const QJsonValue& value : rows)
    {
        const QJsonObject row = value.toObject();
        const QString name = row.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
        {
            functionSignatures_[name].append(row.value(QStringLiteral("signature")).toString());
        }
    }
}

void ConsoleWidget::maybeShowSignatureTooltip()
{
    const QTextCursor cursor = input_->textCursor();
    const QString beforeCursor = cursor.block().text().left(cursor.positionInBlock());
    if (!beforeCursor.endsWith(QLatin1Char('(')))
    {
        QToolTip::hideText();
        return;
    }

    int end = beforeCursor.length() - 1;   // the '(' itself
    int start = end;
    while (start > 0 && (beforeCursor.at(start - 1).isLetterOrNumber() || beforeCursor.at(start - 1) == QLatin1Char('_')))
    {
        --start;
    }
    const QString name = beforeCursor.mid(start, end - start);

    const auto signatures = functionSignatures_.constFind(name);
    if (name.isEmpty() || signatures == functionSignatures_.constEnd())
    {
        QToolTip::hideText();
        return;
    }
    const QPoint pos = input_->viewport()->mapToGlobal(input_->cursorRect().bottomLeft());
    QToolTip::showText(pos, signatures.value().join(QLatin1Char('\n')), input_);
}

void ConsoleWidget::applyFont(const QFont& font)
{
    output_->setFont(font);
    input_->setFont(font);
    backendLog_->setFont(font);
}

void ConsoleWidget::submit(const QString& text)
{
    const QString trimmed = text.trimmed();

    restoreFocusOnReady_ = input_->hasFocus();

    if (trimmed == QLatin1String("clear") || trimmed == QLatin1String("clr"))
    {
        // Handled locally, as in the terminal client (RevClient.cpp): never sent to the backend.
        clearOutput();
        history_.add(trimmed);
        saveHistory();
        return;
    }

    appendEcho((continuation_ ? QStringLiteral("+ ") : QStringLiteral("> ")) + text);
    history_.add(text);
    saveHistory();
    session_->submit(text);
}

void ConsoleWidget::updateInputState()
{
    const bool ready = session_->state() == Session::State::Ready;
    const bool wasEnabled = input_->isEnabled();
    input_->setEnabled(ready);
    // Qt moves focus off a widget the instant it is disabled (Busy, below) and never restores it on its own;
    // without restoreFocusOnReady_, the console would silently stop taking Enter/typing after the very first
    // command it submitted itself -- but a submission that came from somewhere else (the editor's Ctrl+Enter/F5,
    // another panel's action) must not steal focus back to the console just because this input was also disabled
    // for the same busy period.
    if (ready && !wasEnabled && restoreFocusOnReady_)
    {
        input_->setFocus();
    }
    prompt_->setText(continuation_ ? QStringLiteral("+") : QStringLiteral(">"));

    switch (session_->state())
    {
    case Session::State::Ready:        input_->setPlaceholderText(QString()); break;
    case Session::State::Busy:         input_->setPlaceholderText(tr("Running...")); break;
    case Session::State::Starting:     input_->setPlaceholderText(tr("Starting backend...")); break;
    case Session::State::Closing:      input_->setPlaceholderText(tr("Backend shutting down...")); break;
    case Session::State::Closed:       input_->setPlaceholderText(tr("Backend closed.")); break;
    case Session::State::Failed:       input_->setPlaceholderText(tr("Backend failed.")); break;
    case Session::State::NotStarted:   input_->setPlaceholderText(tr("No backend.")); break;
    }
}

QString ConsoleWidget::historyFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/history.txt");
}

void ConsoleWidget::loadHistory()
{
    QFile file(historyFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return;
    }
    QStringList entries;
    QTextStream stream(&file);
    while (!stream.atEnd())
    {
        const QString line = stream.readLine();
        if (!line.isEmpty())
        {
            entries.append(unescapeFromHistoryFile(line));
        }
    }
    history_.setEntries(entries);
}

void ConsoleWidget::saveHistory()
{
    QFile file(historyFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        return;
    }
    QTextStream stream(&file);
    for (const QString& entry : history_.entries())
    {
        stream << escapeForHistoryFile(entry) << '\n';
    }
}

} // namespace revstudio
