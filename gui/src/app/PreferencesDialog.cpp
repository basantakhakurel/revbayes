#include "app/PreferencesDialog.h"

#include "app/Settings.h"
#include "app/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace revstudio {

PreferencesDialog::PreferencesDialog(Settings* settings, QWidget* parent)
    : QDialog(parent), settings_(settings), themeCombo_(new QComboBox(this)),
      fontFamilyCombo_(new QFontComboBox(this)), fontSizeSpin_(new QSpinBox(this)),
      backendPathEdit_(new QLineEdit(this)), watchForChangesCheck_(new QCheckBox(tr("Watch for external changes"), this))
{
    setWindowTitle(tr("Preferences"));

    themeCombo_->setObjectName(QStringLiteral("preferencesTheme"));
    fontFamilyCombo_->setObjectName(QStringLiteral("preferencesFontFamily"));
    fontSizeSpin_->setObjectName(QStringLiteral("preferencesFontSize"));
    backendPathEdit_->setObjectName(QStringLiteral("preferencesBackendPath"));
    watchForChangesCheck_->setObjectName(QStringLiteral("preferencesWatchForChanges"));

    themeCombo_->addItem(tr("System"), static_cast<int>(Theme::Mode::System));
    themeCombo_->addItem(tr("Light"), static_cast<int>(Theme::Mode::Light));
    themeCombo_->addItem(tr("Dark"), static_cast<int>(Theme::Mode::Dark));
    themeCombo_->setCurrentIndex(static_cast<int>(settings_->themeMode()));

    auto* appearanceBox = new QGroupBox(tr("Appearance"), this);
    auto* appearanceForm = new QFormLayout(appearanceBox);
    appearanceForm->addRow(tr("Theme:"), themeCombo_);

    const QFont currentFont = settings_->editorFont();
    fontFamilyCombo_->setCurrentFont(currentFont);
    fontSizeSpin_->setRange(6, 72);
    fontSizeSpin_->setValue(currentFont.pointSize());

    auto* fontBox = new QGroupBox(tr("Editor and Console Font"), this);
    auto* fontForm = new QFormLayout(fontBox);
    fontForm->addRow(tr("Family:"), fontFamilyCombo_);
    fontForm->addRow(tr("Size:"), fontSizeSpin_);

    backendPathEdit_->setText(settings_->backendPath());
    backendPathEdit_->setPlaceholderText(tr("(auto-detect)"));
    backendPathEdit_->setClearButtonEnabled(true);
    auto* browseButton = new QPushButton(tr("Browse..."), this);
    connect(browseButton, &QPushButton::clicked, this, [this]
    {
        const QString path = QFileDialog::getOpenFileName(this, tr("Select rb Executable"), backendPathEdit_->text());
        if (!path.isEmpty())
        {
            backendPathEdit_->setText(path);
        }
    });
    auto* backendPathRow = new QHBoxLayout;
    backendPathRow->addWidget(backendPathEdit_);
    backendPathRow->addWidget(browseButton);

    auto* backendNote = new QLabel(
        tr("Leave empty to search automatically. Takes effect the next time RevStudio starts."), this);
    backendNote->setWordWrap(true);
    QFont noteFont = backendNote->font();
    noteFont.setItalic(true);
    backendNote->setFont(noteFont);

    auto* backendBox = new QGroupBox(tr("Backend (rb)"), this);
    auto* backendLayout = new QVBoxLayout(backendBox);
    backendLayout->addLayout(backendPathRow);
    backendLayout->addWidget(backendNote);

    watchForChangesCheck_->setChecked(settings_->explorerWatchForChanges());
    watchForChangesCheck_->setToolTip(
        tr("Turn off on slow or network drives, where a file system watch can be expensive."));

    auto* explorerBox = new QGroupBox(tr("File Explorer"), this);
    auto* explorerLayout = new QVBoxLayout(explorerBox);
    explorerLayout->addWidget(watchForChangesCheck_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(appearanceBox);
    layout->addWidget(fontBox);
    layout->addWidget(backendBox);
    layout->addWidget(explorerBox);
    layout->addWidget(buttons);
}

void PreferencesDialog::accept()
{
    settings_->setThemeMode(static_cast<Theme::Mode>(themeCombo_->currentData().toInt()));

    QFont font(fontFamilyCombo_->currentFont().family());
    font.setPointSize(fontSizeSpin_->value());
    settings_->setEditorFont(font);

    settings_->setBackendPath(backendPathEdit_->text().trimmed());
    settings_->setExplorerWatchForChanges(watchForChangesCheck_->isChecked());

    QDialog::accept();
}

} // namespace revstudio
