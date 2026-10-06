#ifndef REVSTUDIO_CONSOLEWIDGET_H
#define REVSTUDIO_CONSOLEWIDGET_H

#include "app/ConsoleHistory.h"
#include "backend/Session.h"

#include <QHash>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QColor;
class QCompleter;
class QFont;
class QLabel;
class QPlainTextEdit;
class QStringListModel;
class QTabWidget;

namespace revstudio {

class ConsoleInputEdit;

/**
 * G3: the console -- output view, input line, history, busy state, the `ask` dialog, and the "Backend" tab for
 * stderr/log/protocol activity (GUI_Implementation_Note.md, section 6.9). Owns no Session (not its lifetime);
 * it only connects to one passed in, the same pattern MainWindow already uses.
 *
 * Q3 (pulled forward from phase 3): Tab (idle only) sends `complete` and shows the results in a `QCompleter`
 * popup anchored at the caret, Tab or Enter inserts -- the manual "QCompleter on a plain text edit" wiring from
 * Qt's own Custom Completer example, since QPlainTextEdit (unlike QLineEdit) has no setCompleter() of its own.
 * A signature tooltip on `(` reads from a function-name-to-signature map, filled from whatever `functions`
 * snapshot reply comes along on Session::functionsChanged() -- this class deliberately does not request its
 * own: VariablesPanel already asks for one on ready() (and on its own refresh), and the signal reaches every
 * listener on the shared Session regardless of who asked, so a second, independent request here would just be
 * redundant traffic (and broke tests asserting an exact request sequence/count before this was caught). A
 * `help` request per keystroke was the other option (section 6.10's "signature tooltips reuse the same request"
 * is about the help dock's own tooltip, not this one) but is slower and heavier for no benefit.
 *
 * requestStop() (Ctrl+C / the Stop toolbar action, MainWindow) is a best effort, not the full Q2/C4 story: sending
 * `interrupt` only gets back an acknowledgement that the backend was busy, not a cancelled computation, since
 * there is no core interrupt flag yet (phase 3's C4) -- so it reliably stops something idle between statements,
 * but a true long-running analysis (MCMC, etc.) keeps going. If the backend has not gone back to Ready within
 * stopTimeoutMs_, this offers to kill and restart it instead (Session::restart()), so Stop always eventually
 * un-sticks the GUI even without C4, at the cost of the current Rev session state (open editor tabs are
 * unaffected).
 *
 * Q7's error navigation: a `source()` failure prints "Problem processing line N in file \"F\"" (RevClient.cpp,
 * `execute_file()`; the quotes are `std::filesystem::path`'s own stream insertion, not something this class
 * adds) as ordinary output -- appendFormatted() turns that one substring into a real link (QTextCharFormat
 * anchor) wherever it lands in `output_`, and a click emits errorLinkActivated() for MainWindow to act on. Only
 * `output_`, deliberately: `QPlainTextEdit` has no built-in link-click handling the way `QTextBrowser` does (and
 * switching to one risks exactly the large-output performance this view's 100000-block cap exists for), so
 * ConsoleOutputEdit (private, in the .cpp) replicates just the bit it needs -- cursorForPosition() plus the
 * format's own isAnchor()/anchorHref() -- instead.
 */
class ConsoleWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ConsoleWidget(Session* session, QWidget* parent = nullptr);

    void clearOutput();

    /** For diagnostics that happen before the Session has anything to say for itself (for example "no backend
     *  found", from main.cpp's own search before Session::start() is ever called). Shows up on the Backend tab,
     *  same as stderr/protocol-warning text. */
    void logBackend(const QString& text) { appendBackendLog(text); }

    /** Submits `text` exactly as if the user had typed it and pressed Enter (echo, history, the lot). For other
     *  panels' actions that run Rev code on the user's behalf -- G4's variables context menu (`clear(x)`), G5's
     *  file explorer ("Set as working directory", section 6.7) -- so those actions show up in the console and
     *  history "exactly as if typed", not as a side channel the user cannot see or recall. */
    void submitText(const QString& text);

    /** Inserts `text` at the input's caret, for G4's "double-click inserts the name... in the console" (there is
     *  no editor yet to offer as the other destination). Does not submit. */
    void insertText(const QString& text);

    /** Ctrl+C / the Stop toolbar action; see the class comment. A no-op unless the backend is actually Busy. */
    void requestStop();

    /** G7/Preferences: applies the font to the output, input and Backend-tab views (not `prompt_`, a small label
     *  whose own fixed width is sized from its own font regardless). */
    void applyFont(const QFont& font);

    /** Default 5000 (GUI_Implementation_Note.md's own fallback table); a setter, not a constant, so tests do not
     *  have to wait out a real 5 s to reach the kill-and-restart prompt. */
    void setStopTimeoutMs(int ms) { stopTimer_.setInterval(ms); }

signals:
    /** A "Problem processing line N in file F" link was clicked; `path` is resolved against the session's cwd
     *  already if it was not absolute to begin with. MainWindow opens the file and jumps to `line` (1-based). */
    void errorLinkActivated(const QString& path, int line);

private:
    struct PendingChunk { QString stream; QString text; };

    void appendOutputChunk(const QString& stream, const QString& text);
    void flushPendingOutput();
    void appendEcho(const QString& text);
    void appendBanner(const QString& reason);
    void appendBackendLog(const QString& text);
    void appendFormatted(QPlainTextEdit* target, const QString& text, const QColor& color, bool italic);
    void linkifyErrorLines(int insertStart, const QString& insertedText);

    void submitCurrentInput();
    void submit(const QString& text);
    void updateInputState();
    void onStopTimeout();

    void requestCompletion();
    void onCompletionsReceived(qint64 requestId, int replaceFrom, const QJsonArray& items);
    void insertCompletion(const QString& text);
    void onFunctionsSnapshot(const QJsonArray& rows);
    void maybeShowSignatureTooltip();

    void loadHistory();
    void saveHistory();
    static QString historyFilePath();

    Session* session_;

    QTabWidget*       tabs_;
    QPlainTextEdit*   output_;
    QLabel*           prompt_;
    ConsoleInputEdit* input_;
    QPlainTextEdit*   backendLog_;

    ConsoleHistory        history_;
    QTimer                flushTimer_;
    QVector<PendingChunk> pendingOutput_;
    QTimer                stopTimer_;   // single-shot; see requestStop()/onStopTimeout()

    // Q3 completion: the manual QCompleter-on-QPlainTextEdit wiring. completionModel_ is re-filled with each
    // completions reply's item texts; pendingCompletion* remembers the request that is currently in flight (and
    // the exact buffer/cursor it was sent for) so a stale reply -- or completionReplaceFromChar_'s conversion
    // from the UTF-8 byte offset the protocol uses -- is never applied against text that has since changed.
    QCompleter*       completer_;
    QStringListModel* completionModel_;
    qint64            pendingCompletionRequestId_ = -1;
    QString           pendingCompletionBuffer_;
    int               pendingCompletionCursorChar_ = 0;
    int               completionReplaceFromChar_ = 0;

    // Q3 signature tooltip: name -> one or more signatures (overloads), from whatever `functions` snapshot
    // comes in (see the class comment on why this does not request its own). A local lookup, not a `help`
    // request per keystroke.
    QHash<QString, QStringList> functionSignatures_;

    bool continuation_ = false;
    // Whether the busy->ready transition that follows should give the console input focus back. True at
    // construction (so the very first Starting->Ready transition still focuses it, matching the old
    // unconditional behavior) and recomputed in submit() from whether the input had focus at submit time -- so a
    // submission that came from the editor (Ctrl+Enter/F5) or another panel (G4's "clear(x)", G5's "Set as
    // working directory") leaves whatever had focus alone instead of yanking it to the console on completion.
    bool restoreFocusOnReady_ = true;
};

} // namespace revstudio

#endif
