#include "MainWindow.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QPalette>
#include <QString>
#include <QVector>

namespace {

// Build the light QPalette matching the Fluent-inspired QSS color tokens.
// Fusion style reads QPalette for widget backgrounds/text where QSS doesn't
// reach (spin boxes, some edges). Keep it consistent with style.qss.
QPalette buildLightPalette() {
    QPalette p;

    const QColor window{0xFA, 0xFA, 0xF8};      // #FAFAF8 warm off-white
    const QColor panel{0xF4, 0xF4, 0xF2};        // #F4F4F2 panel/titlebar
    const QColor surface{0xFF, 0xFF, 0xFF};       // #FFFFFF input/elevated
    const QColor text{0x1A, 0x1A, 0x1A};          // #1A1A1A primary
    const QColor muted{0x6B, 0x6B, 0x68};         // #6B6B68 secondary
    const QColor accent{0x0F, 0x6C, 0xBD};        // #0F6CBD Fluent blue
    const QColor accentSel{0xCC, 0xE4, 0xF6};     // #CCE4F6 selection bg
    const QColor border{0xE3, 0xE3, 0xE0};        // #E3E3E0 hairline
    const QColor disabled{0xC0, 0xC0, 0xBC};      // #C0C0BC disabled text
    const QColor disabledBg{0xEB, 0xEB, 0xEA};    // #EBEBEA disabled bg

    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, surface);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panel);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(Qt::white));
    p.setColor(QPalette::Highlight, accentSel);
    p.setColor(QPalette::HighlightedText, text);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::LinkVisited, accent);
    p.setColor(QPalette::ToolTipBase, surface);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, muted);

    // Disabled state.
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Base, disabledBg);
    p.setColor(QPalette::Disabled, QPalette::Button, disabledBg);

    // Mid / Midlight / Dark / Shadow — used by Fusion for 3-D edge rendering.
    p.setColor(QPalette::Mid, border);
    p.setColor(QPalette::Midlight, surface);
    p.setColor(QPalette::Dark, QColor(0xC0, 0xC0, 0xBC));
    p.setColor(QPalette::Shadow, QColor(0xA0, 0xA0, 0x9C));

    return p;
}

} // namespace

int main(int argc, char** argv) {
    // Fusion style before QApplication construction so the style engine
    // initialises with the correct base style.
    QApplication::setStyle("Fusion");

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("exsearcher"));
    app.setOrganizationName(QStringLiteral("kosh"));

    // Apply light palette globally.
    app.setPalette(buildLightPalette());

    // Load QSS from the embedded resource.
    {
        QFile f(QStringLiteral(":/style.qss"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            app.setStyleSheet(QString::fromUtf8(f.readAll()));
    }

    // Window/taskbar icon from embedded resource.
    app.setWindowIcon(QIcon(QStringLiteral(":/icon256.png")));

    // Pull roots from the native (wide) command line so non-ASCII paths and
    // drive args survive. Everything past argv[0] is treated as a root.
    QVector<QString> roots;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 1; i < wargc; ++i)
            roots.push_back(QString::fromWCharArray(wargv[i]));
        LocalFree(wargv);
    }

    MainWindow win(roots);
    win.show();
    return app.exec();
}
