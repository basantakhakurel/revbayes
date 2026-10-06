#ifndef REVSTUDIO_VARIABLESMODEL_H
#define REVSTUDIO_VARIABLESMODEL_H

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTimer>

#include <memory>
#include <vector>

namespace revstudio {

/**
 * G4: the variables tree (GUI_Implementation_Note.md, section 6.6). Two levels: top-level variables, with
 * element variables such as `x[1]` nested under their parent `x` (the backend already reports which is which,
 * via each row's `parent` field -- see doc/server-protocol.md's "Variable row").
 *
 * applySnapshot() diffs the incoming rows against the current tree by name (at each level) rather than
 * resetting the model, so a view's selection, scroll position and expanded/collapsed state survive a refresh --
 * the point of doing this at all, since a refresh happens after every single `submit`. A row whose `summary`
 * changed is flagged for BackgroundRole's highlight, which fades back to nothing over about 1.2 seconds.
 */
class VariablesModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column { NameColumn = 0, TypeColumn = 1, ValueColumn = 2, ColumnCount = 3 };

    // Qt::DisplayRole/BackgroundRole on the columns above cover the tree view itself; these are for the
    // delegate (the kind glyph) and for code (not the view) that wants the raw fields, e.g. a context menu
    // building `clear(x)` from a selected name.
    enum Role
    {
        KindRole = Qt::UserRole + 1,    // QString: "constant"/"deterministic"/"stochastic"/"clamped"/"workspace"/"unknown"
        FlagsRole,                       // QStringList
        TruncatedRole,                   // bool: summary was cut short
    };

    explicit VariablesModel(QObject* parent = nullptr);
    ~VariablesModel() override;

    void applySnapshot(const QJsonArray& rows);
    void clear();

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    struct Row
    {
        QString name;
        QString type;
        QString kind;
        QString summary;
        bool    truncated = false;
        QStringList flags;
        qint64  highlightStartMs = -1;   // elapsedTimer_.elapsed() value when summary last changed, or -1
        Row*    parentRow = nullptr;     // null for a top-level row
        std::vector<std::unique_ptr<Row>> children;
    };

    static Row rowFromJson(const QJsonObject& object);
    // Returns true if `incoming`'s fields differ from `row`'s in a way the view should see (used to decide
    // whether to emit dataChanged and start the highlight fade).
    static bool updateFields(Row& row, const Row& incoming);

    void applyDiff(std::vector<std::unique_ptr<Row>>& existing, const QVector<QJsonObject>& incomingRows,
                   Row* parentRow, const QModelIndex& parentIndex);
    Row* rowAt(const QModelIndex& index) const;
    QModelIndex indexForRow(Row* row, int column) const;
    int rowPositionAmongSiblings(Row* row) const;

    void fadeTick();

    std::vector<std::unique_ptr<Row>> topLevel_;
    QTimer        fadeTimer_;
    QElapsedTimer elapsedTimer_;
};

} // namespace revstudio

#endif
