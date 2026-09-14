#include "ui/app_theme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace hlm {

void applyAppTheme(QApplication &app)
{
    // Deliberately NOT calling QStyleHints::setColorScheme(): it only exists
    // since Qt 6.8 (the colorScheme getter is 6.5), while this project supports
    // Qt >= 6.4. It is also redundant here: Fusion is forced and every palette
    // role the app renders is set explicitly below, so the OS color scheme no
    // longer reaches any widget surface.

    // Fusion honours the palette below and does not follow the Windows dark
    // color scheme, unlike some native styles. theme.qss still supplies the
    // app-specific look on top.
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);

    QPalette p = app.style()->standardPalette();
    p.setColor(QPalette::Window, QColor(0xed, 0xf2, 0xf6));
    p.setColor(QPalette::WindowText, QColor(0x17, 0x28, 0x3b));
    p.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::AlternateBase, QColor(0xf4, 0xf7, 0xfa));
    p.setColor(QPalette::Text, QColor(0x17, 0x28, 0x3b));
    p.setColor(QPalette::Button, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::ButtonText, QColor(0x19, 0x34, 0x4d));
    p.setColor(QPalette::ToolTipBase, QColor(0x15, 0x2c, 0x42));
    p.setColor(QPalette::ToolTipText, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::Highlight, QColor(0x08, 0x78, 0xb8));
    p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::Disabled, QPalette::WindowText,
               QColor(0x89, 0x98, 0xa7));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x89, 0x98, 0xa7));
    p.setColor(QPalette::Disabled, QPalette::ButtonText,
               QColor(0x89, 0x98, 0xa7));
    app.setPalette(p);
}

} // namespace hlm
