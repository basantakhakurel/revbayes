#include "app/Theme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace revstudio::Theme {

namespace {

QPalette darkPalette()
{
    // A standard, widely-copied Fusion dark palette (the same colors show up in most Qt dark-theme write-ups);
    // nothing specific to RevStudio here, which is the point -- G7's Preferences dialog is where customizing
    // this belongs, not here.
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(53, 53, 53));
    palette.setColor(QPalette::WindowText, Qt::white);
    palette.setColor(QPalette::Base, QColor(35, 35, 35));
    palette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
    palette.setColor(QPalette::ToolTipBase, Qt::white);
    palette.setColor(QPalette::ToolTipText, Qt::white);
    palette.setColor(QPalette::Text, Qt::white);
    palette.setColor(QPalette::Button, QColor(53, 53, 53));
    palette.setColor(QPalette::ButtonText, Qt::white);
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, QColor(42, 130, 218));
    palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    palette.setColor(QPalette::HighlightedText, Qt::black);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(127, 127, 127));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
    return palette;
}

} // namespace

void apply(Mode mode)
{
    // Captured on first use, before anything here has had a chance to change it: the platform's own default
    // style name, so System can restore it exactly rather than guessing a name like "windows" or "macos".
    static const QString platformDefaultStyle = qApp->style()->objectName();

    if (mode == Mode::System)
    {
        qApp->setStyle(QStyleFactory::create(platformDefaultStyle));
        qApp->setPalette(qApp->style()->standardPalette());
        return;
    }

    qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    qApp->setPalette(mode == Mode::Dark ? darkPalette() : qApp->style()->standardPalette());
}

} // namespace revstudio::Theme
