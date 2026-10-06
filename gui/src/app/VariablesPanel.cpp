#include "app/VariablesPanel.h"

#include "app/ConsoleWidget.h"
#include "app/VariablesModel.h"
#include "backend/Session.h"

#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTreeView>
#include <QVBoxLayout>

namespace revstudio {

/**
 * Text filtering (recursive: a matching descendant keeps its ancestors visible) plus the two toggles section
 * 6.6 asks for, which plain text filtering cannot express: "show workspace objects" and "show hidden/system
 * variables" (off by default, matching the spec -- `args` is `system`).
 */
class VariablesFilterProxyModel : public QSortFilterProxyModel
{
public:
    explicit VariablesFilterProxyModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent)
    {
        setRecursiveFilteringEnabled(true);
        setFilterCaseSensitivity(Qt::CaseInsensitive);
        setFilterKeyColumn(VariablesModel::NameColumn);
    }

    void setShowWorkspace(bool show)
    {
        showWorkspace_ = show;
        beginFilterChange();
        endFilterChange(Direction::Rows);
    }

    void setShowHidden(bool show)
    {
        showHidden_ = show;
        beginFilterChange();
        endFilterChange(Direction::Rows);
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        const QModelIndex nameIndex = sourceModel()->index(sourceRow, VariablesModel::NameColumn, sourceParent);
        if (!showWorkspace_ && sourceModel()->data(nameIndex, VariablesModel::KindRole).toString() == QLatin1String("workspace"))
        {
            return false;
        }
        const QStringList flags = sourceModel()->data(nameIndex, VariablesModel::FlagsRole).toStringList();
        if (!showHidden_ && (flags.contains(QStringLiteral("hidden")) || flags.contains(QStringLiteral("system"))))
        {
            return false;
        }
        return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
    }

private:
    bool showWorkspace_ = true;    // "hidden by default" (6.6) is specified for hidden/system only, not these
    bool showHidden_    = false;
};

VariablesPanel::VariablesPanel(Session* session, ConsoleWidget* console, QWidget* parent)
    : QWidget(parent), session_(session), console_(console), model_(new VariablesModel(this)),
      proxy_(new VariablesFilterProxyModel(this)), tree_(new QTreeView(this)), filterEdit_(new QLineEdit(this)),
      showWorkspaceCheck_(new QCheckBox(tr("Workspace"), this)), showHiddenCheck_(new QCheckBox(tr("Hidden/system"), this)),
      details_(new QPlainTextEdit(this)), functionsModel_(new QStandardItemModel(0, 2, this))
{
    proxy_->setSourceModel(model_);
    tree_->setObjectName(QStringLiteral("variablesTree"));
    tree_->setModel(proxy_);
    tree_->setUniformRowHeights(true);         // GUI_Implementation_Note.md 6.6: keeps 10^4 rows smooth
    tree_->setAlternatingRowColors(true);
    tree_->header()->setSectionResizeMode(VariablesModel::NameColumn, QHeaderView::ResizeToContents);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);

    filterEdit_->setObjectName(QStringLiteral("variablesFilter"));
    filterEdit_->setPlaceholderText(tr("Filter"));
    filterEdit_->setClearButtonEnabled(true);
    showWorkspaceCheck_->setObjectName(QStringLiteral("variablesShowWorkspace"));
    showWorkspaceCheck_->setChecked(true);
    showHiddenCheck_->setObjectName(QStringLiteral("variablesShowHidden"));
    showHiddenCheck_->setChecked(false);

    details_->setObjectName(QStringLiteral("variablesDetails"));
    details_->setReadOnly(true);
    details_->setPlaceholderText(tr("Select a variable to inspect it."));

    auto* toolbarRow = new QHBoxLayout;
    toolbarRow->addWidget(filterEdit_);
    toolbarRow->addWidget(showWorkspaceCheck_);
    toolbarRow->addWidget(showHiddenCheck_);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(tree_);
    splitter->addWidget(details_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    auto* variablesTab = new QWidget(this);
    auto* variablesLayout = new QVBoxLayout(variablesTab);
    variablesLayout->setContentsMargins(4, 4, 4, 4);
    variablesLayout->addLayout(toolbarRow);
    variablesLayout->addWidget(splitter);

    auto* functionsView = new QTreeView(this);
    functionsView->setObjectName(QStringLiteral("functionsTree"));
    functionsModel_->setHorizontalHeaderLabels({tr("Name"), tr("Signature")});
    functionsView->setModel(functionsModel_);
    functionsView->setRootIsDecorated(false);
    functionsView->setAlternatingRowColors(true);
    functionsView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(variablesTab, tr("Variables"));
    tabs->addTab(functionsView, tr("Functions"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs);

    connect(filterEdit_, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);
    connect(showWorkspaceCheck_, &QCheckBox::toggled, proxy_, &VariablesFilterProxyModel::setShowWorkspace);
    connect(showHiddenCheck_, &QCheckBox::toggled, proxy_, &VariablesFilterProxyModel::setShowHidden);

    connect(tree_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex&, const QModelIndex&)
    {
        onSelectionChanged();
    });
    connect(tree_, &QTreeView::doubleClicked, this, &VariablesPanel::onDoubleClicked);
    connect(tree_, &QTreeView::customContextMenuRequested, this, &VariablesPanel::showContextMenu);

    connect(session_, &Session::ready, this, [this](const QString&)
    {
        session_->requestSnapshot(QStringLiteral("all"));
    });
    connect(session_, &Session::variablesChanged, this, [this](const QJsonArray& rows, const QString&)
    {
        model_->applySnapshot(rows);
    });
    connect(session_, &Session::submitFinished, this, [this](qint64, const QString&, const QJsonObject&)
    {
        session_->requestSnapshot(QStringLiteral("functions"));
    });
    connect(session_, &Session::functionsChanged, this, &VariablesPanel::refreshFunctions);
    connect(session_, &Session::inspected, this, [this](qint64 requestId, const QString&, const QString& text, bool truncated)
    {
        if (requestId != pendingInspectId_)
        {
            return;   // the selection moved on before this reply arrived
        }
        details_->setPlainText(truncated ? text + tr("\n[...truncated...]") : text);
    });
}

void VariablesPanel::onSelectionChanged()
{
    const QModelIndex current = tree_->selectionModel()->currentIndex();
    if (!current.isValid())
    {
        return;
    }
    const QString name = nameAt(current);
    if (name.isEmpty())
    {
        return;
    }
    pendingInspectId_ = session_->requestInspect(name);
    if (pendingInspectId_ < 0)
    {
        details_->setPlainText(tr("(backend busy; selection will not update the details until it is idle)"));
    }
}

void VariablesPanel::onDoubleClicked(const QModelIndex& proxyIndex)
{
    const QString name = nameAt(proxyIndex);
    if (!name.isEmpty())
    {
        console_->insertText(name);
    }
}

void VariablesPanel::showContextMenu(const QPoint& position)
{
    const QModelIndex index = tree_->indexAt(position);
    const QString name = nameAt(index);
    if (name.isEmpty())
    {
        return;
    }

    QMenu menu(this);
    menu.addAction(tr("Copy Name"), this, [name] { QGuiApplication::clipboard()->setText(name); });
    menu.addAction(tr("Print"), this, [this, name] { console_->submitText(QStringLiteral("print(%1)").arg(name)); });
    menu.addAction(tr("Structure"), this, [this, name] { console_->submitText(QStringLiteral("structure(%1)").arg(name)); });
    menu.addSeparator();
    menu.addAction(tr("Remove"), this, [this, name] { console_->submitText(QStringLiteral("clear(%1)").arg(name)); });
    menu.exec(tree_->viewport()->mapToGlobal(position));
}

void VariablesPanel::refreshFunctions(const QJsonArray& rows)
{
    functionsModel_->removeRows(0, functionsModel_->rowCount());
    for (const QJsonValue& value : rows)
    {
        const QJsonObject object = value.toObject();
        functionsModel_->appendRow({new QStandardItem(object.value(QStringLiteral("name")).toString()),
                                    new QStandardItem(object.value(QStringLiteral("signature")).toString())});
    }
}

QString VariablesPanel::nameAt(const QModelIndex& proxyIndex) const
{
    if (!proxyIndex.isValid())
    {
        return QString();
    }
    const QModelIndex nameIndex = proxyIndex.sibling(proxyIndex.row(), VariablesModel::NameColumn);
    return proxy_->data(nameIndex, Qt::DisplayRole).toString();
}

} // namespace revstudio
