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

// Build the dark QPalette that matches the QSS color scheme.
// Fusion style reads QPalette for widget backgrounds / text colors for elements
// QSS doesn't reach (e.g. spinboxes, some edges). Keep it consistent.
QPalette buildDarkPalette() {
    QPalette p;

    const QColor bg{0x11, 0x13, 0x18};          // #111318
    const QColor panel{0x1C, 0x1F, 0x26};        // #1C1F26
    const QColor elevated{0x1E, 0x22, 0x30};     // #1E2230
    const QColor text{0xE2, 0xE8, 0xF0};         // #E2E8F0
    const QColor muted{0x4A, 0x55, 0x68};        // #4A5568
    const QColor accent{0x7C, 0x3A, 0xED};       // #7C3AED
    const QColor accentLight{0xC4, 0xB5, 0xFD};  // #C4B5FD
    const QColor border{0x2E, 0x33, 0x40};       // #2E3340
    const QColor disabled{0x3D, 0x43, 0x58};     // #3D4358
    const QColor selection{0x3B, 0x1F, 0x7A};    // #3B1F7A

    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, bg);
    p.setColor(QPalette::AlternateBase, panel);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panel);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(Qt::white));
    p.setColor(QPalette::Highlight, selection);
    p.setColor(QPalette::HighlightedText, text);
    p.setColor(QPalette::Link, accentLight);
    p.setColor(QPalette::LinkVisited, accent);
    p.setColor(QPalette::ToolTipBase, panel);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, muted);

    // Disabled state.
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);

    // Mid / Midlight / Dark — used by Fusion for 3D edge rendering.
    p.setColor(QPalette::Mid, border);
    p.setColor(QPalette::Midlight, elevated);
    p.setColor(QPalette::Dark, QColor(0x0A, 0x0C, 0x10));
    p.setColor(QPalette::Shadow, QColor(0x06, 0x07, 0x09));

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

    // Apply dark palette globally.
    app.setPalette(buildDarkPalette());

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
