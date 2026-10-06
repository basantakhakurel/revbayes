#ifndef REVSTUDIO_CONSOLEHISTORY_H
#define REVSTUDIO_CONSOLEHISTORY_H

#include <QString>
#include <QStringList>

#include <optional>

namespace revstudio {

/**
 * Up/Down navigation through submitted statements (GUI_Implementation_Note.md, section 6.9, defect D9).
 *
 * D9: the old GTK console's Up/Down was wrong -- with history `a b c`, Up gave `c`, `b`, then Down gave `a`
 * (older) instead of `c` (back toward the newest). The fix is the usual shell-history model: a browse index into
 * `entries_` (oldest first), `-1` meaning "not browsing". The first Up stashes the in-progress draft so Down can
 * restore it after returning to the live position; a direction change does not lose that draft.
 *
 * Pure QtCore logic, deliberately with no QWidget/QPlainTextEdit dependency, so it is unit-testable without a
 * display (unlike the input widget that will call it, which needs to decide which key presses mean "navigate
 * history" versus "move the cursor" -- a decision that belongs with the widget's cursor, not here).
 */
class ConsoleHistory
{
public:
    explicit ConsoleHistory(int capacity = 1000);

    /** Records a submitted statement and ends any in-progress browsing. Ignores a blank entry, and an entry
     *  identical to the most recent one (typing the same command twice in a row should not clutter Up/Down). */
    void add(const QString& entry);

    /** Ends browsing without recording anything, e.g. because the user edited the input by hand. */
    void resetBrowsing();

    /** `currentText` is what the input shows right now; the first call in a browsing sequence stashes it so
     *  down() can eventually restore it. Returns the text to show, or nullopt if there is nothing to move to
     *  (no history at all, or already at the oldest entry) -- the caller should leave the input unchanged. */
    std::optional<QString> up(const QString& currentText);

    /** Returns the text to show (the next-newer entry, or the stashed draft once browsing ends), or nullopt if
     *  not currently browsing (Down is then not a history action at all; the widget's normal key handling,
     *  if any, applies instead). */
    std::optional<QString> down();

    bool isBrowsing() const { return index_ >= 0; }
    int  size() const { return entries_.size(); }

    /** The stored entries, oldest first -- for persistence (GUI_Implementation_Note.md 6.9's history.txt) and
     *  for tests. */
    QStringList entries() const { return entries_; }

    /** Replaces the stored entries (for loading a persisted history.txt), oldest first. Does not end browsing by
     *  itself, but this is only ever meaningful before any browsing has started (right after construction). */
    void setEntries(const QStringList& entries);

private:
    QStringList entries_;
    int         capacity_;
    int         index_ = -1;    // -1 == not browsing; otherwise a valid index into entries_
    QString     draft_;
};

} // namespace revstudio

#endif
