#include "app/ConsoleHistory.h"

namespace revstudio {

ConsoleHistory::ConsoleHistory(int capacity) : capacity_(capacity)
{
}

void ConsoleHistory::add(const QString& entry)
{
    resetBrowsing();
    if (entry.isEmpty() || (!entries_.isEmpty() && entries_.last() == entry))
    {
        return;
    }
    entries_.append(entry);
    while (entries_.size() > capacity_)
    {
        entries_.removeFirst();
    }
}

void ConsoleHistory::resetBrowsing()
{
    index_ = -1;
    draft_.clear();
}

std::optional<QString> ConsoleHistory::up(const QString& currentText)
{
    if (entries_.isEmpty())
    {
        return std::nullopt;
    }
    if (index_ < 0)
    {
        draft_ = currentText;
        index_ = entries_.size() - 1;
        return entries_.at(index_);
    }
    if (index_ > 0)
    {
        --index_;
        return entries_.at(index_);
    }
    return std::nullopt;   // already at the oldest entry
}

std::optional<QString> ConsoleHistory::down()
{
    if (index_ < 0)
    {
        return std::nullopt;   // not browsing: Down is not a history action right now
    }
    if (index_ < entries_.size() - 1)
    {
        ++index_;
        return entries_.at(index_);
    }
    const QString draft = draft_;
    resetBrowsing();
    return draft;   // back to the live position: restore what the user had typed before the first Up
}

void ConsoleHistory::setEntries(const QStringList& entries)
{
    entries_ = entries;
    while (entries_.size() > capacity_)
    {
        entries_.removeFirst();
    }
}

} // namespace revstudio
