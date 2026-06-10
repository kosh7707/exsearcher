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
#include <QString>
#include <QVector>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

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
