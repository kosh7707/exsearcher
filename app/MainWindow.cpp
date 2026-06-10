#include "MainWindow.h"
#include "IndexController.h"
#include "ResultsModel.h"

#include "exsearcher/Index.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr int kDebounceMs = 30;

std::wstring entryPathW(const exsearcher::Index& idx, quint32 id) {
    return QString::fromStdString(idx.fullPath(id)).toStdWString();
}
} // namespace

MainWindow::MainWindow(const QVector<QString>& roots, QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("exsearcher"));
    resize(1000, 650);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    search_ = new QLineEdit(central);
    search_->setPlaceholderText(
        QStringLiteral("검색어 입력… (공백 = AND)"));
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);
    layout->addWidget(search_);

    table_ = new QTableView(central);
    model_ = new ResultsModel(this);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSortingEnabled(false);
    table_->horizontalHeader()->setSortIndicatorShown(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(20);  // compact rows
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setColumnWidth(ResultsModel::ColName, 280);
    table_->setColumnWidth(ResultsModel::ColPath, 420);
    table_->setColumnWidth(ResultsModel::ColSize, 90);
    table_->setColumnWidth(ResultsModel::ColModified, 140);
    layout->addWidget(table_);

    setCentralWidget(central);

    status_ = new QLabel(QStringLiteral("색인 중…"), this);
    statusBar()->addWidget(status_);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kDebounceMs);

    controller_ = new IndexController(this);
    model_->setIndex(&controller_->index());

    connect(search_, &QLineEdit::textChanged, this,
            &MainWindow::onSearchTextChanged);
    connect(debounce_, &QTimer::timeout, this,
            &MainWindow::onDebounceTimeout);
    connect(table_, &QTableView::activated, this, &MainWindow::onActivated);
    connect(table_, &QTableView::customContextMenuRequested, this,
            &MainWindow::onContextMenu);

    connect(controller_, &IndexController::progress, this,
            &MainWindow::onProgress, Qt::QueuedConnection);
    connect(controller_, &IndexController::indexingDone, this,
            &MainWindow::onIndexingDone, Qt::QueuedConnection);
    connect(controller_, &IndexController::searchReady, this,
            &MainWindow::onSearchReady, Qt::QueuedConnection);

    search_->setFocus();
    controller_->start(roots);
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
    // Cancel + join the crawl thread before the controller (and Index) die.
    controller_->shutdown();
    event->accept();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == search_ && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            search_->clear();
            return true;
        }
        if (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Return ||
            ke->key() == Qt::Key_Enter) {
            if (model_->rowCount() > 0) {
                table_->setFocus();
                if (!table_->currentIndex().isValid())
                    table_->setCurrentIndex(model_->index(0, 0));
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onSearchTextChanged() {
    debounce_->start();  // single-shot restart
}

void MainWindow::onDebounceTimeout() {
    runSearch();
}

void MainWindow::runSearch() {
    if (!indexReady_)
        return;  // status area already shows the indexing hint
    const quint64 seq = ++searchSeq_;
    controller_->requestSearch(search_->text(), seq);
}

void MainWindow::onProgress(quint64 entries) {
    if (indexReady_)
        return;
    status_->setText(
        QStringLiteral("색인 중… %1개 항목").arg(entries));
}

void MainWindow::onIndexingDone(quint64 files, quint64 dirs,
                                quint64 elapsedMs) {
    indexReady_ = true;
    const double secs = static_cast<double>(elapsedMs) / 1000.0;
    status_->setText(QStringLiteral("파일 %1개, 폴더 %2개 색인 완료 "
                                    "(%3초) — 준비됨")
                         .arg(files)
                         .arg(dirs)
                         .arg(QString::number(secs, 'f', 1)));
    // Run any query already typed during indexing.
    runSearch();
}

void MainWindow::onSearchReady(quint64 seq, QVector<quint32> indices,
                               quint64 total, bool capped, quint64 elapsedMs) {
    if (seq < lastShownSeq_)
        return;  // superseded by a newer query
    lastShownSeq_ = seq;

    model_->setRows(std::move(indices));

    if (!indexReady_) {
        status_->setText(QStringLiteral("색인 중…"));
        return;
    }
    QString msg = QStringLiteral("%1개 일치 (%2 ms)")
                      .arg(total)
                      .arg(elapsedMs);
    if (capped)
        msg += QStringLiteral(" (상위 100,000개 표시)");
    status_->setText(msg);
}

void MainWindow::onActivated(const QModelIndex& index) {
    if (!index.isValid())
        return;
    openEntry(model_->entryIndex(index.row()));
}

void MainWindow::onContextMenu(const QPoint& pos) {
    QModelIndex idx = table_->indexAt(pos);
    if (!idx.isValid())
        return;
    const quint32 id = model_->entryIndex(idx.row());

    QMenu menu(this);
    QAction* open = menu.addAction(QStringLiteral("열기"));
    QAction* reveal =
        menu.addAction(QStringLiteral("폴더에서 열기"));
    QAction* copy = menu.addAction(QStringLiteral("전체 경로 복사"));

    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (chosen == open)
        openEntry(id);
    else if (chosen == reveal)
        openContainingFolder(id);
    else if (chosen == copy)
        copyFullPath(id);
}

void MainWindow::openEntry(quint32 entryId) {
    if (entryId == UINT32_MAX)
        return;
    std::wstring path = entryPathW(controller_->index(), entryId);
    ShellExecuteW(reinterpret_cast<HWND>(winId()), L"open", path.c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::openContainingFolder(quint32 entryId) {
    if (entryId == UINT32_MAX)
        return;
    std::wstring path = entryPathW(controller_->index(), entryId);
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(reinterpret_cast<HWND>(winId()), L"open", L"explorer.exe",
                  args.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::copyFullPath(quint32 entryId) {
    if (entryId == UINT32_MAX)
        return;
    QString path =
        QString::fromStdString(controller_->index().fullPath(entryId));
    QApplication::clipboard()->setText(path);
}
