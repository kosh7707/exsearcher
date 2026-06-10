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
#include <QDateTime>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QSet>
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

// Convert a Windows FILETIME (100ns ticks since 1601) to a yyyy-MM-dd string.
QString filetimeToDate(quint64 ft) {
    if (ft == 0)
        return QStringLiteral("?");
    // FILETIME epoch (1601) to Unix epoch (1970): 11644473600 seconds.
    const qint64 unixMs =
        static_cast<qint64>(ft / 10000ULL) - 11644473600000LL;
    return QDateTime::fromMSecsSinceEpoch(unixMs).toString(
        QStringLiteral("yyyy-MM-dd"));
}

// Elide the middle of a long path to at most maxChars characters.
QString elideMiddle(const QString& s, int maxChars) {
    if (s.size() <= maxChars)
        return s;
    const int keep = maxChars - 1;  // room for the ellipsis char
    const int head = keep / 2;
    const int tail = keep - head;
    return s.left(head) + QStringLiteral("…") + s.right(tail);
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

    // --- Controller init ---
    // The controller detects drives in its constructor and loads the snapshot
    // (below). No auto-crawl happens; the user picks drives via the chip bar.
    controller_ = new IndexController(this);
    model_ = new ResultsModel(this);
    model_->setIndex(&controller_->index());

    // --- Chip bar ---
    chipBar_ = new DriveChipBar(controller_->detectedDrives(), settings_,
                                searchPanel);
    panelLayout->addWidget(chipBar_);

    // --- Progress bar (hidden when idle) ---
    progress_ = new QProgressBar(searchPanel);
    progress_->setObjectName("crawlProgress");
    progress_->setRange(0, 100);
    progress_->setTextVisible(false);
    progress_->setVisible(false);
    panelLayout->addWidget(progress_);

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
    connect(chipBar_, &DriveChipBar::crawlRequested, this,
            &MainWindow::onCrawlRequested);
    connect(chipBar_, &DriveChipBar::recrawlDriveRequested, this,
            &MainWindow::onRecrawlDriveRequested);
    connect(chipBar_, &DriveChipBar::removeDriveRequested, this,
            &MainWindow::onRemoveDriveRequested);
    connect(chipBar_, &DriveChipBar::reindexAllRequested, this,
            &MainWindow::onReindexAllRequested);

    connect(controller_, &IndexController::progress, this,
            &MainWindow::onProgress, Qt::QueuedConnection);
    connect(controller_, &IndexController::indexingDone, this,
            &MainWindow::onIndexingDone, Qt::QueuedConnection);
    connect(controller_, &IndexController::searchReady, this,
            &MainWindow::onSearchReady, Qt::QueuedConnection);

    // --- Load snapshot (no auto-crawl) ---
    const bool loaded = controller_->loadSnapshot();
    indexReady_ = loaded;
    refreshIndexedState();

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
    if (chipBar_->allIndexedEnabled())
        return {};  // empty = no filter (every indexed drive is enabled)

    std::vector<uint32_t> roots;
    for (const QString& letter : chipBar_->enabledDrives()) {
        const quint32 idx = controller_->rootIndexForDrive(letter);
        if (idx != UINT32_MAX)
            roots.push_back(static_cast<uint32_t>(idx));
    }
    return roots;
}

void MainWindow::refreshIndexedState() {
    // Tell the chip bar which drives are indexed.
    QSet<QString> indexed;
    for (const IndexedRoot& ir : controller_->indexedRoots())
        indexed.insert(ir.letter);
    chipBar_->setIndexedDrives(indexed);
    // Normalize chip/button enabled state (re-disables reindex when nothing is
    // indexed). Safe here because refreshIndexedState only runs when idle.
    chipBar_->setChipsEnabled(true);

    // Compose the idle status line.
    if (indexed.isEmpty()) {
        status_->setText(QStringLiteral(
            "색인된 드라이브 없음 — 드라이브 칩을 눌러 색인하세요"));
        return;
    }

    // Sorted letters + newest crawl date.
    QStringList letters;
    quint64 newest = 0;
    quint64 fileCount = controller_->index().size();
    for (const IndexedRoot& ir : controller_->indexedRoots()) {
        letters.append(ir.letter.left(1));
        if (ir.crawledAtFiletime > newest)
            newest = ir.crawledAtFiletime;
    }
    letters.sort();
    status_->setText(
        QStringLiteral("저장된 색인 로드됨 — 파일 %1개, 드라이브: %2 "
                       "(마지막 색인 %3)")
            .arg(fileCount)
            .arg(letters.join(QStringLiteral(", ")))
            .arg(filetimeToDate(newest)));
}

void MainWindow::setBusyUi(const QString& statusText) {
    indexReady_ = false;
    chipBar_->setChipsEnabled(false);
    progress_->setValue(0);
    progress_->setVisible(true);
    status_->setText(statusText);
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
    if (!chipBar_->allIndexedEnabled() && roots.empty()) {
        lastShownSeq_ = seq;
        model_->setRows({});
        status_->setText(QStringLiteral("0개 일치 — 선택된 드라이브 없음"));
        return;
    }
    const std::vector<uint32_t>* rootsPtr = roots.empty() ? nullptr : &roots;
    controller_->requestSearch(search_->text(), seq, rootsPtr);
}

void MainWindow::onProgress(quint64 entries, quint64 dirsDone,
                            quint64 dirsPending, QString currentDir) {
    if (indexReady_)
        return;
    const quint64 totalDirs = dirsDone + dirsPending;
    int pct = 0;
    if (totalDirs > 0)
        pct = static_cast<int>((dirsDone * 100) / totalDirs);
    if (pct > 99)
        pct = 99;  // cap at 99 until done signal
    progress_->setValue(pct);

    const QString cur = elideMiddle(currentDir, 60);
    const QString prefix =
        crawlingLetter_.isEmpty() ? QString() : (crawlingLetter_ + QStringLiteral(": "));
    status_->setText(
        QStringLiteral("%1색인 중 — 항목 %2 · 폴더 %3/~%4 (%5%) · 현재: %6")
            .arg(prefix)
            .arg(entries)
            .arg(dirsDone)
            .arg(totalDirs)
            .arg(pct)
            .arg(cur));
}

void MainWindow::onIndexingDone(quint64 files, quint64 dirs,
                                quint64 elapsedMs) {
    indexReady_ = true;
    crawlingLetter_.clear();
    progress_->setValue(100);
    progress_->setVisible(false);
    chipBar_->setChipsEnabled(true);
    refreshIndexedState();

    if (files != 0 || dirs != 0) {
        // A crawl completed — append a one-line summary after the idle status.
        const double secs = static_cast<double>(elapsedMs) / 1000.0;
        status_->setText(
            status_->text() +
            QStringLiteral("  ·  방금 색인: 파일 %1개, 폴더 %2개 (%3초)")
                .arg(files)
                .arg(dirs)
                .arg(QString::number(secs, 'f', 1)));
    }
    // Run any query already typed during indexing.
    runSearch();
}

void MainWindow::onSearchReady(quint64 seq, QVector<quint32> indices,
                               quint64 total, bool capped, quint64 elapsedMs) {
    if (seq < lastShownSeq_)
        return;  // superseded by a newer query
    lastShownSeq_ = seq;

    model_->setRows(std::move(indices));

    if (!indexReady_)
        return;  // a crawl is running; leave the progress status intact
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
    // An indexed drive's checked state toggled — just re-run the query under the
    // new filter. (Crawling a not-indexed drive goes through onCrawlRequested.)
    if (!indexReady_)
        return;
    runSearch();
}

void MainWindow::onCrawlRequested(const QString& letter) {
    if (controller_->busy())
        return;
    crawlingLetter_ = letter;
    setBusyUi(QStringLiteral("%1: 색인 중…").arg(letter));
    controller_->crawlDrive(letter);
}

void MainWindow::onRecrawlDriveRequested(const QString& letter) {
    if (controller_->busy())
        return;
    crawlingLetter_ = letter;
    setBusyUi(QStringLiteral("%1: 재색인 중…").arg(letter));
    controller_->recrawlDrives({letter});
}

void MainWindow::onRemoveDriveRequested(const QString& letter) {
    if (controller_->busy())
        return;
    crawlingLetter_.clear();
    // removeDrive does not crawl; show a brief busy state without a percent bar.
    indexReady_ = false;
    chipBar_->setChipsEnabled(false);
    status_->setText(QStringLiteral("%1: 색인에서 제거 중…").arg(letter));
    controller_->removeDrive(letter);
}

void MainWindow::onReindexAllRequested() {
    if (controller_->busy())
        return;
    // Recrawl every currently-indexed drive.
    QVector<QString> indexed;
    for (const IndexedRoot& ir : controller_->indexedRoots())
        indexed.push_back(ir.letter);
    if (indexed.isEmpty())
        return;
    crawlingLetter_.clear();
    setBusyUi(QStringLiteral("전체 재색인 중…"));
    controller_->recrawlDrives(indexed);
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
