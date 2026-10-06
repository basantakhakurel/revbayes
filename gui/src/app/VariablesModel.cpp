#include "app/VariablesModel.h"

#include <QBrush>
#include <QColor>
#include <QJsonObject>
#include <QSet>

#include <functional>

namespace revstudio {

namespace {
constexpr int highlightFadeMs = 1200;   // GUI_Implementation_Note.md 6.6: "faded by a 1.2 s timer"
constexpr int fadeTickMs      = 100;
}

VariablesModel::VariablesModel(QObject* parent) : QAbstractItemModel(parent)
{
    elapsedTimer_.start();
    fadeTimer_.setInterval(fadeTickMs);
    connect(&fadeTimer_, &QTimer::timeout, this, &VariablesModel::fadeTick);
}

VariablesModel::~VariablesModel() = default;

VariablesModel::Row VariablesModel::rowFromJson(const QJsonObject& object)
{
    Row row;
    row.name      = object.value(QStringLiteral("name")).toString();
    row.type      = object.value(QStringLiteral("type")).toString();
    row.kind      = object.value(QStringLiteral("kind")).toString();
    row.summary   = object.value(QStringLiteral("summary")).toString();
    row.truncated = object.value(QStringLiteral("summary_truncated")).toBool(false);
    for (const QJsonValue& flag : object.value(QStringLiteral("flags")).toArray())
    {
        row.flags.append(flag.toString());
    }
    return row;
}

bool VariablesModel::updateFields(Row& row, const Row& incoming)
{
    const bool changed = row.type != incoming.type || row.kind != incoming.kind ||
                          row.summary != incoming.summary || row.truncated != incoming.truncated ||
                          row.flags != incoming.flags;
    row.name      = incoming.name;
    row.type      = incoming.type;
    row.kind      = incoming.kind;
    row.summary   = incoming.summary;
    row.truncated = incoming.truncated;
    row.flags     = incoming.flags;
    return changed;
}

void VariablesModel::applySnapshot(const QJsonArray& rowsJson)
{
    QVector<QJsonObject> topLevelJson;
    QMap<QString, QVector<QJsonObject>> childrenByParent;

    for (const QJsonValue& value : rowsJson)
    {
        const QJsonObject object = value.toObject();
        if (object.contains(QStringLiteral("parent")))
        {
            childrenByParent[object.value(QStringLiteral("parent")).toString()].append(object);
        }
        else
        {
            topLevelJson.append(object);
        }
    }

    applyDiff(topLevel_, topLevelJson, nullptr, QModelIndex());

    // Second pass: children are diffed against each top-level row AFTER the top level itself is settled, so a
    // newly-inserted parent already exists to attach its own newly-inserted children to.
    for (auto& rowPtr : topLevel_)
    {
        Row* row = rowPtr.get();
        applyDiff(row->children, childrenByParent.value(row->name), row, indexForRow(row, 0));
    }
}

void VariablesModel::clear()
{
    if (topLevel_.empty())
    {
        return;
    }
    beginResetModel();
    topLevel_.clear();
    endResetModel();
    fadeTimer_.stop();
}

void VariablesModel::applyDiff(std::vector<std::unique_ptr<Row>>& existing, const QVector<QJsonObject>& incomingRows,
                               Row* parentRow, const QModelIndex& parentIndex)
{
    std::vector<Row> incoming;
    incoming.reserve(static_cast<std::size_t>(incomingRows.size()));
    for (const QJsonObject& object : incomingRows)
    {
        incoming.push_back(rowFromJson(object));
    }

    QSet<QString> incomingNames;
    for (const Row& row : incoming)
    {
        incomingNames.insert(row.name);
    }
    QSet<QString> existingNames;
    for (const auto& row : existing)
    {
        existingNames.insert(row->name);
    }

    std::size_t oldIndex = 0;
    std::size_t newIndex = 0;

    while (oldIndex < existing.size() && newIndex < incoming.size())
    {
        if (existing[oldIndex]->name == incoming[newIndex].name)
        {
            Row* row = existing[oldIndex].get();
            if (updateFields(*row, incoming[newIndex]))
            {
                row->highlightStartMs = elapsedTimer_.elapsed();
                if (!fadeTimer_.isActive())
                {
                    fadeTimer_.start();
                }
                const int r = static_cast<int>(oldIndex);
                emit dataChanged(index(r, 0, parentIndex), index(r, ColumnCount - 1, parentIndex));
            }
            ++oldIndex;
            ++newIndex;
        }
        else if (!incomingNames.contains(existing[oldIndex]->name))
        {
            const int r = static_cast<int>(oldIndex);
            beginRemoveRows(parentIndex, r, r);
            existing.erase(existing.begin() + static_cast<long>(oldIndex));
            endRemoveRows();
            // oldIndex is not advanced: the next surviving row has shifted into this position.
        }
        else
        {
            const int r = static_cast<int>(oldIndex);
            beginInsertRows(parentIndex, r, r);
            auto newRow = std::make_unique<Row>();
            updateFields(*newRow, incoming[newIndex]);
            newRow->parentRow = parentRow;
            existing.insert(existing.begin() + static_cast<long>(oldIndex), std::move(newRow));
            endInsertRows();
            ++oldIndex;
            ++newIndex;
        }
    }

    while (oldIndex < existing.size())
    {
        const int r = static_cast<int>(oldIndex);
        beginRemoveRows(parentIndex, r, r);
        existing.erase(existing.begin() + static_cast<long>(oldIndex));
        endRemoveRows();
    }

    while (newIndex < incoming.size())
    {
        const int r = static_cast<int>(existing.size());
        beginInsertRows(parentIndex, r, r);
        auto newRow = std::make_unique<Row>();
        updateFields(*newRow, incoming[newIndex]);
        newRow->parentRow = parentRow;
        existing.push_back(std::move(newRow));
        endInsertRows();
        ++newIndex;
    }
}

VariablesModel::Row* VariablesModel::rowAt(const QModelIndex& index) const
{
    if (!index.isValid())
    {
        return nullptr;
    }
    Row* parentRow = static_cast<Row*>(index.internalPointer());
    const std::vector<std::unique_ptr<Row>>& siblings = parentRow ? parentRow->children : topLevel_;
    if (index.row() < 0 || static_cast<std::size_t>(index.row()) >= siblings.size())
    {
        return nullptr;
    }
    return siblings[static_cast<std::size_t>(index.row())].get();
}

int VariablesModel::rowPositionAmongSiblings(Row* row) const
{
    const std::vector<std::unique_ptr<Row>>& siblings = row->parentRow ? row->parentRow->children : topLevel_;
    for (std::size_t i = 0; i < siblings.size(); ++i)
    {
        if (siblings[i].get() == row)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QModelIndex VariablesModel::indexForRow(Row* row, int column) const
{
    if (!row)
    {
        return QModelIndex();
    }
    const int position = rowPositionAmongSiblings(row);
    if (position < 0)
    {
        return QModelIndex();
    }
    return createIndex(position, column, row->parentRow);
}

QModelIndex VariablesModel::index(int row, int column, const QModelIndex& parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount)
    {
        return QModelIndex();
    }
    Row* parentRow = rowAt(parent);
    const std::vector<std::unique_ptr<Row>>& siblings = parentRow ? parentRow->children : topLevel_;
    if (static_cast<std::size_t>(row) >= siblings.size())
    {
        return QModelIndex();
    }
    return createIndex(row, column, parentRow);
}

QModelIndex VariablesModel::parent(const QModelIndex& child) const
{
    if (!child.isValid())
    {
        return QModelIndex();
    }
    Row* parentRow = static_cast<Row*>(child.internalPointer());
    return indexForRow(parentRow, 0);
}

int VariablesModel::rowCount(const QModelIndex& parent) const
{
    if (parent.column() > 0)
    {
        return 0;
    }
    Row* parentRow = rowAt(parent);
    const std::vector<std::unique_ptr<Row>>& siblings = parentRow ? parentRow->children : topLevel_;
    return static_cast<int>(siblings.size());
}

int VariablesModel::columnCount(const QModelIndex&) const
{
    return ColumnCount;
}

QVariant VariablesModel::data(const QModelIndex& modelIndex, int role) const
{
    Row* row = rowAt(modelIndex);
    if (!row)
    {
        return QVariant();
    }

    if (role == Qt::DisplayRole)
    {
        switch (modelIndex.column())
        {
        case NameColumn:  return row->name;
        case TypeColumn:  return row->type;
        case ValueColumn: return row->summary;   // already carries its own "…" when truncated (WorkspaceSnapshot)
        default: return QVariant();
        }
    }
    if (role == Qt::BackgroundRole)
    {
        if (row->highlightStartMs < 0)
        {
            return QVariant();
        }
        const qint64 elapsed = elapsedTimer_.elapsed() - row->highlightStartMs;
        if (elapsed >= highlightFadeMs)
        {
            return QVariant();
        }
        QColor color(255, 230, 120);
        color.setAlphaF(1.0 - static_cast<double>(elapsed) / static_cast<double>(highlightFadeMs));
        return QBrush(color);
    }
    if (role == KindRole)
    {
        return row->kind;
    }
    if (role == FlagsRole)
    {
        return row->flags;
    }
    if (role == TruncatedRole)
    {
        return row->truncated;
    }
    return QVariant();
}

QVariant VariablesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    {
        return QAbstractItemModel::headerData(section, orientation, role);
    }
    switch (section)
    {
    case NameColumn:  return tr("Name");
    case TypeColumn:  return tr("Type");
    case ValueColumn: return tr("Value");
    default: return QVariant();
    }
}

void VariablesModel::fadeTick()
{
    bool anyStillFading = false;

    std::function<void(std::vector<std::unique_ptr<Row>>&, const QModelIndex&)> visit =
        [&](std::vector<std::unique_ptr<Row>>& rows, const QModelIndex& parentIndex)
    {
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            Row* row = rows[i].get();
            if (row->highlightStartMs >= 0)
            {
                const qint64 elapsed = elapsedTimer_.elapsed() - row->highlightStartMs;
                if (elapsed >= highlightFadeMs)
                {
                    row->highlightStartMs = -1;
                }
                else
                {
                    anyStillFading = true;
                }
                const int r = static_cast<int>(i);
                emit dataChanged(index(r, 0, parentIndex), index(r, ColumnCount - 1, parentIndex), {Qt::BackgroundRole});
            }
            if (!row->children.empty())
            {
                visit(row->children, index(static_cast<int>(i), 0, parentIndex));
            }
        }
    };
    visit(topLevel_, QModelIndex());

    if (!anyStillFading)
    {
        fadeTimer_.stop();
    }
}

} // namespace revstudio
