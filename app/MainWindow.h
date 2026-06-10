#pragma once

#include <QMainWindow>
#include <QVector>

#include <cstdint>
#include <vector>

class QLineEdit;
class QTableView;
class QTimer;
class QLabel;
class QSettings;
class IndexController;
class ResultsModel;
class DriveChipBar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QVector<QString>& roots, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message,
                     qintptr* result) override;

private slots:
    void onSearchTextChanged();
    void onDebounceTimeout();
    void onProgress(quint64 entries);
    void onIndexingDone(quint64 files, quint64 dirs, quint64 elapsedMs);
    void onSearchReady(quint64 seq, QVector<quint32> indices, quint64 total,
                       bool capped, quint64 elapsedMs);
    void onActivated(const QModelIndex& index);
    void onContextMenu(const QPoint& pos);
    void onDriveFilterChanged();
    void onReindexRequested();

private:
    void openEntry(quint32 entryId);
    void openContainingFolder(quint32 entryId);
    void copyFullPath(quint32 entryId);
    void runSearch();
    void applyDarkTitlebar();

    // Build the allowedRoots vector from currently enabled chips.
    // Returns nullptr (no filter) when all drives are enabled.
    std::vector<uint32_t> buildAllowedRoots() const;

    QLineEdit* search_ = nullptr;
    QTableView* table_ = nullptr;
    QTimer* debounce_ = nullptr;
    QLabel* status_ = nullptr;
    DriveChipBar* chipBar_ = nullptr;

    IndexController* controller_ = nullptr;
    ResultsModel* model_ = nullptr;
    QSettings* settings_ = nullptr;

    bool indexReady_ = false;
    quint64 searchSeq_ = 0;     // increments per dispatched query
    quint64 lastShownSeq_ = 0;  // newest seq whose result is displayed

    // Track whether all drives were indexed this session. If a chip is turned ON
    // after crawl for a drive that wasn't crawled, a reindex is needed.
    QVector<QString> crawledDrives_;
};
