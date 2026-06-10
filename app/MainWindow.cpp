#include "MainWindow.h"
#include "DriveChipBar.h"
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
#include <dwmapi.h>
#include <shellapi.h>

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>

namespace {
constexpr int kDebounceMs = 30;

std::wstring entryPathW(const exsearcher::Index& idx, quint32 id) {
    return QString::fromStdString(idx.fullPath(id)).toStdWString();
}
} // namespace

MainWindow::MainWindow(const QVector<QString>& roots, QWidget* parent)
    : QMainWindow(parent) {
    // roots from the command line are superseded by the chip bar drive selection.
    // The chip bar auto-detects all DRIVE_FIXED/DRIVE_REMOTE volumes and persists
    // the enabled set in settings.ini.
    Q_UNUSED(roots);
    setWindowTitle(QStringLiteral("exsearcher"));
    resize(1100, 680);

    // --- Settings (portable ini next to exe) ---
    const QString iniPath =
        QCoreApplication::applicationDirPath() + QStringLiteral("/settings.ini");
    settings_ = new QSettings(iniPath, QSettings::IniFormat, this);

    // Restore geometry.
    const QByteArray geom = settings_->value("window/geometry").toByteArray();
    if (!geom.isEmpty())
        restoreGeometry(geom);

    // --- Central widget ---
    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // --- Search panel (elevated bar) ---
    auto* searchPanel = new QWidget(central);
    searchPanel->setObjectName("searchPanel");
    auto* panelLayout = new QVBoxLayout(searchPanel);
    panelLayout->setContentsMargins(12, 10, 12, 8);
    panelLayout->setSpacing(8);

    // Search box with magnifying-glass label overlay.
    auto* searchRow = new QWidget(searchPanel);
    auto* searchRowLayout = new QHBoxLayout(searchRow);
    searchRowLayout->setContentsMargins(0, 0, 0, 0);
    searchRowLayout->setSpacing(0);

    search_ = new QLineEdit(searchRow);
    search_->setObjectName("searchBox");
    search_->setPlaceholderText(
        QStringLiteral("  \U0001F50D  검색어 입력… 공백으로 AND 검색"));
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);
    searchRowLayout->addWidget(search_);

    panelLayout->addWidget(searchRow);

    // --- Controller init (detect drives first, before chip bar) ---
    controller_ = new IndexController(this);
    model_ = new ResultsModel(this);
    model_->setIndex(&controller_->index());

    // Start the controller to detect drives (does NOT start crawl yet).
    // We call start() after building the chip bar so we can pass enabled drives.
    // Actually, start() both detects and crawls — so we detect drives separately
    // via IndexController::detectDrives which is private. We need to detect drives
    // before start() so the chip bar can be shown immediately.
    // Solution: call start() with the roots from chips after chip bar is built.

    // --- Chip bar ---
    // Use a temporary detectDrives approach: call start() with empty roots to
    // trigger auto-detect, but we need the chip bar BEFORE that.
    // We need to detect drives early. Since detectDrives is private, we rebuild
    // the detection logic here inline for the chip bar, then start() will
    // re-detect internally. Both detections call GetLogicalDrives which is
    // instant and idempotent.
    {
        QVector<DriveInfo> earlyDrives;
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i)))
                continue;
            wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
            UINT type = GetDriveTypeW(root);
            if (type == DRIVE_FIXED || type == DRIVE_REMOTE) {
                DriveInfo di;
                di.letter = QString::fromWCharArray(root, 2);
                di.isRemote = (type == DRIVE_REMOTE);
                earlyDrives.push_back(di);
            }
        }
        chipBar_ = new DriveChipBar(earlyDrives, settings_, searchPanel);
    }
    panelLayout->addWidget(chipBar_);

    rootLayout->addWidget(searchPanel);

    // --- Table ---
    table_ = new QTableView(central);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSortingEnabled(false);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSortIndicatorShown(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(22);
    table_->setShowGrid(false);
    table_->setFrameShape(QFrame::NoFrame);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setColumnWidth(ResultsModel::ColName, 280);
    table_->setColumnWidth(ResultsModel::ColPath, 440);
    table_->setColumnWidth(ResultsModel::ColSize, 90);
    table_->setColumnWidth(ResultsModel::ColModified, 140);
    table_->setIconSize(QSize(16, 16));
    rootLayout->addWidget(table_);

    setCentralWidget(central);

    // --- Status bar ---
    status_ = new QLabel(QStringLiteral("색인 중…"), this);
    statusBar()->addWidget(status_);
    statusBar()->setSizeGripEnabled(false);

    // --- Debounce timer ---
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kDebounceMs);

    // --- Connections ---
    connect(search_, &QLineEdit::textChanged, this,
            &MainWindow::onSearchTextChanged);
    connect(debounce_, &QTimer::timeout, this,
            &MainWindow::onDebounceTimeout);
    connect(table_, &QTableView::activated, this, &MainWindow::onActivated);
    connect(table_, &QTableView::customContextMenuRequested, this,
            &MainWindow::onContextMenu);
    connect(chipBar_, &DriveChipBar::filterChanged, this,
            &MainWindow::onDriveFilterChanged);
    connect(chipBar_, &DriveChipBar::reindexRequested, this,
            &MainWindow::onReindexRequested);

    connect(controller_, &IndexController::progress, this,
            &MainWindow::onProgress, Qt::QueuedConnection);
    connect(controller_, &IndexController::indexingDone, this,
            &MainWindow::onIndexingDone, Qt::QueuedConnection);
    connect(controller_, &IndexController::searchReady, this,
            &MainWindow::onSearchReady, Qt::QueuedConnection);

    // --- Start crawl ---
    const QVector<QString> enabledDrives = chipBar_->enabledDrives();
    crawledDrives_ = enabledDrives;
    chipBar_->setChipsEnabled(false);

    // Pass enabled drives explicitly so only checked drives are crawled.
    controller_->start(enabledDrives);

    search_->setFocus();

    // Apply dark titlebar after the window handle exists.
    applyDarkTitlebar();
}

MainWindow::~MainWindow() = default;

void MainWindow::applyDarkTitlebar() {
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 11 Build 22000+).
    // Value 19 works on older Win10 insider builds; try 20 first, fall back to 19.
    HWND hwnd = reinterpret_cast<HWND>(winId());
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    settings_->setValue("window/geometry", saveGeometry());
    // Cancel + join the crawl thread before the controller (and Index) die.
    controller_->shutdown();
    event->accept();
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message,
                             qintptr* result) {
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
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

std::vector<uint32_t> MainWindow::buildAllowedRoots() const {
    if (chipBar_->allEnabled())
        return {};  // empty = no filter

    std::vector<uint32_t> roots;
    for (const QString& letter : chipBar_->enabledDrives()) {
        const quint32 idx = controller_->rootIndexForDrive(letter);
        if (idx != UINT32_MAX)
            roots.push_back(static_cast<uint32_t>(idx));
    }
    return roots;
}

void MainWindow::onSearchTextChanged() {
    debounce_->start();  // single-shot restart
}

void MainWindow::onDebounceTimeout() {
    runSearch();
}

void MainWindow::runSearch() {
    if (!indexReady_)
        return;
    const quint64 seq = ++searchSeq_;
    std::vector<uint32_t> roots = buildAllowedRoots();
    // An empty root set while chips are filtering means "exclude everything";
    // it must not collapse into nullptr ("no filter").
    if (!chipBar_->allEnabled() && roots.empty()) {
        lastShownSeq_ = seq;
        model_->setRows({});
        status_->setText(QStringLiteral("0개 일치 — 선택된 드라이브 없음"));
        return;
    }
    const std::vector<uint32_t>* rootsPtr = roots.empty() ? nullptr : &roots;
    controller_->requestSearch(search_->text(), seq, rootsPtr);
}

void MainWindow::onProgress(quint64 entries) {
    if (sender() != controller_)
        return;  // queued leftover from a replaced controller
    if (indexReady_)
        return;
    status_->setText(
        QStringLiteral("색인 중… %1개 항목").arg(entries));
}

void MainWindow::onIndexingDone(quint64 files, quint64 dirs,
                                quint64 elapsedMs) {
    if (sender() != controller_)
        return;  // queued leftover from a replaced controller
    indexReady_ = true;
    chipBar_->setChipsEnabled(true);
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
    // Results from a replaced controller index entries of a destroyed Index;
    // applying them to the model would render dangling rows.
    if (sender() != controller_)
        return;
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

void MainWindow::onDriveFilterChanged() {
    if (!indexReady_)
        return;

    // Check if any newly enabled drive was not crawled this session.
    const QVector<QString> enabled = chipBar_->enabledDrives();
    bool needsRecrawl = false;
    for (const QString& letter : enabled) {
        if (!crawledDrives_.contains(letter)) {
            needsRecrawl = true;
            break;
        }
    }

    if (needsRecrawl) {
        onReindexRequested();
    } else {
        // Just re-run the current query with updated root filter.
        runSearch();
    }
}

void MainWindow::onReindexRequested() {
    // Shutdown current crawl + search state, then restart.
    indexReady_ = false;
    chipBar_->setChipsEnabled(false);
    status_->setText(QStringLiteral("재색인 중…"));

    controller_->shutdown();

    // Reset controller state by replacing it. Disconnect first so nothing
    // else arrives from the old instance (sender() guards catch the queued
    // calls that were already posted).
    disconnect(controller_, nullptr, this, nullptr);
    controller_->deleteLater();
    controller_ = new IndexController(this);
    model_->setIndex(&controller_->index());

    connect(controller_, &IndexController::progress, this,
            &MainWindow::onProgress, Qt::QueuedConnection);
    connect(controller_, &IndexController::indexingDone, this,
            &MainWindow::onIndexingDone, Qt::QueuedConnection);
    connect(controller_, &IndexController::searchReady, this,
            &MainWindow::onSearchReady, Qt::QueuedConnection);

    const QVector<QString> enabledDrives = chipBar_->enabledDrives();
    crawledDrives_ = enabledDrives;
    controller_->start(enabledDrives);
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
