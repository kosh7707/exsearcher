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
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

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
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QWindow>
#include <QScreen>

namespace {
constexpr int kDebounceMs = 30;

// Resize border thickness in logical pixels.
constexpr int kResizeBorder = 8;

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

// Convert physical screen pixels to logical pixels using device pixel ratio.
int physToLog(int phys, qreal dpr) {
    return (dpr > 0.0) ? static_cast<int>(phys / dpr) : phys;
}

} // namespace

// ============================================================
// TitleBar implementation
// ============================================================

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName("titleBar");
    setFixedHeight(40);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 0, 0);
    layout->setSpacing(0);

    // App icon (16x16).
    auto* iconLabel = new QLabel(this);
    iconLabel->setPixmap(QIcon(QStringLiteral(":/icon256.png")).pixmap(16, 16));
    iconLabel->setFixedSize(16, 16);
    layout->addWidget(iconLabel);

    layout->addSpacing(8);

    // App name label.
    auto* titleLabel = new QLabel(QStringLiteral("exsearcher"), this);
    titleLabel->setObjectName("titleLabel");
    layout->addWidget(titleLabel);

    layout->addStretch(1);

    // Caption buttons — use Segoe MDL2 Assets glyphs.
    // U+E921 minimize, U+E922 maximize, U+E923 restore, U+E8BB close
    btnMin_ = new QPushButton(QString(QChar(0xE921)), this);
    btnMin_->setObjectName("btnMinimize");
    btnMin_->setFocusPolicy(Qt::NoFocus);
    btnMin_->setToolTip(QStringLiteral("최소화"));

    btnMax_ = new QPushButton(QString(QChar(0xE922)), this);
    btnMax_->setObjectName("btnMaximize");
    btnMax_->setFocusPolicy(Qt::NoFocus);
    btnMax_->setToolTip(QStringLiteral("최대화"));

    btnClose_ = new QPushButton(QString(QChar(0xE8BB)), this);
    btnClose_->setObjectName("btnClose");
    btnClose_->setFocusPolicy(Qt::NoFocus);
    btnClose_->setToolTip(QStringLiteral("닫기"));

    layout->addWidget(btnMin_);
    layout->addWidget(btnMax_);
    layout->addWidget(btnClose_);
}

void TitleBar::updateMaxGlyph(bool isMaximized) {
    // U+E923 restore, U+E922 maximize
    btnMax_->setText(QString(QChar(isMaximized ? 0xE923 : 0xE922)));
    btnMax_->setToolTip(isMaximized ? QStringLiteral("복원") : QStringLiteral("최대화"));
}

// ============================================================
// MainWindow implementation
// ============================================================

MainWindow::MainWindow(const QVector<QString>& roots, QWidget* parent)
    : QMainWindow(parent) {
    Q_UNUSED(roots);
    setWindowTitle(QStringLiteral("exsearcher"));
    resize(1100, 680);

    // --- Frameless window: remove OS chrome, keep DWM shadow + Win11 corners.
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);

    // Extend DWM frame into client area by 1px at the top.
    // This keeps the DWM shadow (and rounded Win11 corners) alive on a
    // frameless window. MARGINS{left, right, top, bottom}.
    HWND hwnd = reinterpret_cast<HWND>(winId());
    MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

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

    // --- Custom title bar ---
    titleBar_ = new TitleBar(central);
    rootLayout->addWidget(titleBar_);

    connect(titleBar_->btnMinimize(), &QPushButton::clicked,
            this, &MainWindow::onMinimizeClicked);
    connect(titleBar_->btnMaximize(), &QPushButton::clicked,
            this, &MainWindow::onMaximizeClicked);
    connect(titleBar_->btnClose(), &QPushButton::clicked,
            this, &MainWindow::onCloseClicked);

    // --- Search panel (elevated bar) ---
    auto* searchPanel = new QWidget(central);
    searchPanel->setObjectName("searchPanel");
    auto* panelLayout = new QVBoxLayout(searchPanel);
    panelLayout->setContentsMargins(16, 12, 16, 10);
    panelLayout->setSpacing(10);

    // Search box.
    auto* searchRow = new QWidget(searchPanel);
    auto* searchRowLayout = new QHBoxLayout(searchRow);
    searchRowLayout->setContentsMargins(0, 0, 0, 0);
    searchRowLayout->setSpacing(0);

    search_ = new QLineEdit(searchRow);
    search_->setObjectName("searchBox");
    search_->setPlaceholderText(
        QStringLiteral("검색어 입력… 공백으로 AND 검색"));
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);
    searchRowLayout->addWidget(search_);

    panelLayout->addWidget(searchRow);

    // --- Controller init ---
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
}

MainWindow::~MainWindow() = default;

int MainWindow::titleBarHeight() const {
    // titleBar_ is 40 logical px.  For the NCHITTEST calculations in physical
    // pixels we return the logical value; the caller scales as needed.
    return titleBar_ ? titleBar_->height() : 40;
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange && titleBar_) {
        titleBar_->updateMaxGlyph(isMaximized());
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    settings_->setValue("window/geometry", saveGeometry());
    controller_->shutdown();
    event->accept();
}

// ---------------------------------------------------------------------------
// nativeEvent — frameless window chrome
// ---------------------------------------------------------------------------
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message,
                             qintptr* result) {
    Q_UNUSED(eventType);
    auto* msg = static_cast<MSG*>(message);

    switch (msg->message) {

    // WM_NCCALCSIZE: tell DWM to use our full client rect as the window rect.
    // When wParam is TRUE, adjust the first RECT (rcNewClient) so DWM removes
    // the standard NC frame.  When maximised we must inset by SM_CXSIZEFRAME +
    // SM_CXPADDEDBORDER on all sides to avoid the maximised 8px overhang
    // painting off-screen.
    case WM_NCCALCSIZE: {
        if (!msg->wParam)
            return false;  // wParam 0: just return 0 (default), handled by Qt

        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
        RECT& r = params->rgrc[0];

        if (IsZoomed(msg->hwnd)) {
            const int frame = GetSystemMetrics(SM_CXSIZEFRAME)
                            + GetSystemMetrics(SM_CXPADDEDBORDER);
            r.left   += frame;
            r.top    += frame;
            r.right  -= frame;
            r.bottom -= frame;
        }
        *result = 0;
        return true;
    }

    // WM_NCHITTEST: identify which part of the window was hit.
    case WM_NCHITTEST: {
        // Screen coords of the cursor.
        const POINT ptScreen{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};

        RECT wndRect;
        GetWindowRect(msg->hwnd, &wndRect);

        const int x = ptScreen.x - wndRect.left;
        const int y = ptScreen.y - wndRect.top;
        const int w = wndRect.right  - wndRect.left;
        const int h = wndRect.bottom - wndRect.top;

        // Physical resize-border thickness.  Use device pixel ratio for
        // per-monitor DPI awareness.
        const qreal dpr = devicePixelRatioF();
        const int rb = static_cast<int>(kResizeBorder * dpr);

        // Only show resize cursors when NOT maximised.
        const bool zoomed = IsZoomed(msg->hwnd);

        if (!zoomed) {
            if (x < rb && y < rb) { *result = HTTOPLEFT;     return true; }
            if (x > w - rb && y < rb) { *result = HTTOPRIGHT; return true; }
            if (x < rb && y > h - rb) { *result = HTBOTTOMLEFT; return true; }
            if (x > w - rb && y > h - rb) { *result = HTBOTTOMRIGHT; return true; }
            if (y < rb) { *result = HTTOP;    return true; }
            if (y > h - rb) { *result = HTBOTTOM; return true; }
            if (x < rb) { *result = HTLEFT;   return true; }
            if (x > w - rb) { *result = HTRIGHT;  return true; }
        }

        // Titlebar region in physical pixels.
        const int tbH = static_cast<int>(titleBarHeight() * dpr);

        if (y <= tbH) {
            // Check if cursor is over a caption button so Windows can show the
            // snap layout flyout over HTMAXBUTTON and route close/min correctly.
            if (titleBar_) {
                // Map cursor to titlebar widget coordinates (logical).
                const QPoint logPt = QPoint(physToLog(x, dpr), physToLog(y, dpr));
                const QPoint tbLocal = titleBar_->mapFrom(this, logPt);

                // Check maximize button for HTMAXBUTTON (enables snap flyout).
                if (titleBar_->btnMaximize()->geometry().contains(tbLocal)) {
                    *result = HTMAXBUTTON;
                    return true;
                }
                // Check minimize and close buttons — they are HTCLIENT so Qt
                // receives mouse events and handles them normally.
                if (titleBar_->btnMinimize()->geometry().contains(tbLocal) ||
                    titleBar_->btnClose()->geometry().contains(tbLocal)) {
                    *result = HTCLIENT;
                    return true;
                }
            }
            *result = HTCAPTION;
            return true;
        }

        *result = HTCLIENT;
        return true;
    }

    // WM_NCLBUTTONDOWN / WM_NCLBUTTONUP for HTMAXBUTTON:
    // Qt does not deliver QMouseEvent to widgets that are within an HT-claimed
    // NC region.  We need to forward the click ourselves so the maximize button
    // actually fires.  Track hover so the button still lights up correctly.
    case WM_NCLBUTTONDOWN: {
        if (msg->wParam == HTMAXBUTTON && titleBar_) {
            // Simulate a press on the maximize button.
            QMouseEvent press(QEvent::MouseButtonPress,
                              QPointF(0, 0), QPointF(0, 0),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(titleBar_->btnMaximize(), &press);
            *result = 0;
            return true;
        }
        break;
    }
    case WM_NCLBUTTONUP: {
        if (msg->wParam == HTMAXBUTTON && titleBar_) {
            QMouseEvent release(QEvent::MouseButtonRelease,
                                QPointF(0, 0), QPointF(0, 0),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(titleBar_->btnMaximize(), &release);
            onMaximizeClicked();
            *result = 0;
            return true;
        }
        break;
    }

    // WM_NCMOUSEMOVE over HTMAXBUTTON: keep hover highlight alive.
    case WM_NCMOUSEMOVE: {
        if (msg->wParam == HTMAXBUTTON && titleBar_) {
            QEvent enter(QEvent::Enter);
            QApplication::sendEvent(titleBar_->btnMaximize(), &enter);
        }
        break;
    }
    case WM_NCMOUSELEAVE: {
        if (titleBar_) {
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(titleBar_->btnMaximize(), &leave);
        }
        break;
    }

    default:
        break;
    }

    return QMainWindow::nativeEvent(eventType, message, result);
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

// ---------------------------------------------------------------------------
// Caption button slots
// ---------------------------------------------------------------------------

void MainWindow::onMinimizeClicked() {
    showMinimized();
}

void MainWindow::onMaximizeClicked() {
    if (isMaximized())
        showNormal();
    else
        showMaximized();
}

void MainWindow::onCloseClicked() {
    close();
}

// ---------------------------------------------------------------------------
// Business logic helpers
// ---------------------------------------------------------------------------

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
    QSet<QString> indexed;
    for (const IndexedRoot& ir : controller_->indexedRoots())
        indexed.insert(ir.letter);
    chipBar_->setIndexedDrives(indexed);
    chipBar_->setChipsEnabled(true);

    if (indexed.isEmpty()) {
        status_->setText(QStringLiteral(
            "색인된 드라이브 없음 — 드라이브 칩을 눌러 색인하세요"));
        return;
    }

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

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::onSearchTextChanged() {
    debounce_->start();
}

void MainWindow::onDebounceTimeout() {
    runSearch();
}

void MainWindow::runSearch() {
    if (!indexReady_)
        return;
    const quint64 seq = ++searchSeq_;
    std::vector<uint32_t> roots = buildAllowedRoots();
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
        pct = 99;
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

void MainWindow::onIndexingDone(quint64 files, quint64 dirs, quint64 elapsedMs) {
    indexReady_ = true;
    crawlingLetter_.clear();
    progress_->setValue(100);
    progress_->setVisible(false);
    chipBar_->setChipsEnabled(true);
    refreshIndexedState();

    if (files != 0 || dirs != 0) {
        const double secs = static_cast<double>(elapsedMs) / 1000.0;
        status_->setText(
            status_->text() +
            QStringLiteral("  ·  방금 색인: 파일 %1개, 폴더 %2개 (%3초)")
                .arg(files)
                .arg(dirs)
                .arg(QString::number(secs, 'f', 1)));
    }
    runSearch();
}

void MainWindow::onSearchReady(quint64 seq, QVector<quint32> indices,
                               quint64 total, bool capped, quint64 elapsedMs) {
    if (seq < lastShownSeq_)
        return;
    lastShownSeq_ = seq;

    model_->setRows(std::move(indices));

    if (!indexReady_)
        return;
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
    QAction* open   = menu.addAction(QStringLiteral("열기"));
    QAction* reveal = menu.addAction(QStringLiteral("폴더에서 열기"));
    QAction* copy   = menu.addAction(QStringLiteral("전체 경로 복사"));

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
    indexReady_ = false;
    chipBar_->setChipsEnabled(false);
    status_->setText(QStringLiteral("%1: 색인에서 제거 중…").arg(letter));
    controller_->removeDrive(letter);
}

void MainWindow::onReindexAllRequested() {
    if (controller_->busy())
        return;
    QVector<QString> indexed;
    for (const IndexedRoot& ir : controller_->indexedRoots())
        indexed.push_back(ir.letter);
    if (indexed.isEmpty())
        return;
    crawlingLetter_.clear();
    setBusyUi(QStringLiteral("전체 재색인 중…"));
    controller_->recrawlDrives(indexed);
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

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
