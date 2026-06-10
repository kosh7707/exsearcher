#pragma once

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QVector>

#include <cstdint>
#include <vector>

class QElapsedTimer;
class QLineEdit;
class QTableView;
class QTimer;
class QLabel;
class QProgressBar;
class QSettings;
class QPushButton;
class QHBoxLayout;
class IndexController;
class ResultsModel;
class DriveChipBar;

// ---------------------------------------------------------------------------
// TitleBar — custom caption bar with Segoe MDL2 Assets caption glyphs.
// Lives as the top widget of the central layout; nativeEvent in MainWindow
// handles WM_NCHITTEST / WM_NCCALCSIZE / WM_NCLBUTTONDOWN|UP.
// ---------------------------------------------------------------------------
class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent = nullptr);

    // Update maximize glyph after window state changes.
    void updateMaxGlyph(bool isMaximized);

    QPushButton* btnMinimize() const { return btnMin_; }
    QPushButton* btnMaximize() const { return btnMax_; }
    QPushButton* btnClose()    const { return btnClose_; }

private:
    QPushButton* btnMin_   = nullptr;
    QPushButton* btnMax_   = nullptr;
    QPushButton* btnClose_ = nullptr;
};

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QVector<QString>& roots, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message,
                     qintptr* result) override;

private slots:
    void onSearchTextChanged();
    void onDebounceTimeout();
    void onProgress(quint64 entries, quint64 dirsDone, quint64 dirsPending,
                    QString currentDir);
    void onIndexingDone(quint64 files, quint64 dirs, quint64 elapsedMs);
    void onSearchReady(quint64 seq, QVector<quint32> indices, quint64 total,
                       bool capped, quint64 elapsedMs);
    void onActivated(const QModelIndex& index);
    void onContextMenu(const QPoint& pos);
    void onDriveFilterChanged();
    void onCrawlRequested(const QString& letter);
    void onRecrawlDriveRequested(const QString& letter);
    void onRemoveDriveRequested(const QString& letter);
    void onReindexAllRequested();

    // Caption button slots.
    void onMinimizeClicked();
    void onMaximizeClicked();
    void onCloseClicked();

    // Tray.
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void showFromTray();
    void quitFromTray();

private:
    void openEntry(quint32 entryId);
    void openContainingFolder(quint32 entryId);
    void copyFullPath(quint32 entryId);
    void runSearch();

    // Refresh chip indexed-state + the idle status line from the controller.
    void refreshIndexedState();
    void setBusyUi(const QString& statusText);

    // Build the allowedRoots vector from currently enabled chips.
    // Returns empty (no filter) when all indexed drives are enabled.
    std::vector<uint32_t> buildAllowedRoots() const;

    // Return the titlebar height in logical pixels (used in NCHITTEST).
    int titleBarHeight() const;

    TitleBar*    titleBar_  = nullptr;
    QLineEdit*   search_    = nullptr;
    QTableView*  table_     = nullptr;
    QTimer*      debounce_  = nullptr;
    QLabel*      status_    = nullptr;
    QProgressBar* progress_ = nullptr;
    DriveChipBar* chipBar_  = nullptr;

    IndexController* controller_ = nullptr;
    ResultsModel*    model_      = nullptr;
    QSettings*       settings_   = nullptr;
    QSystemTrayIcon* tray_       = nullptr;

    // True only when exiting via the tray menu; closeEvent then really quits
    // instead of hiding to the tray.
    bool quitting_ = false;

    bool     indexReady_    = false;
    quint64  searchSeq_     = 0;    // increments per dispatched query
    quint64  lastShownSeq_  = 0;    // newest seq whose result is displayed

    QString crawlingLetter_;  // drive currently being crawled (for status text)

    // ETA estimation during crawl. We keep an EMA of dirs-per-second sampled per
    // progress signal and divide pending dirs by it. Reset at each crawl start.
    void resetCrawlEta();
    QString crawlEtaText(quint64 dirsDone, quint64 dirsPending) const;

    QElapsedTimer* crawlTimer_ = nullptr;  // wall time since crawl start
    double   dirsPerSecEma_ = 0.0;         // smoothed crawl rate
    quint64  etaLastDirsDone_ = 0;         // dirsDone at last sample
    qint64   etaLastMs_ = 0;               // elapsed ms at last sample
    bool     rescanLive_ = false;          // non-blocking rescan in progress
};
