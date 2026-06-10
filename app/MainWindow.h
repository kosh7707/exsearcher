#pragma once

#include <QMainWindow>
#include <QVector>

#include <cstdint>
#include <vector>

class QLineEdit;
class QTableView;
class QTimer;
class QLabel;
class QProgressBar;
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

private:
    void openEntry(quint32 entryId);
    void openContainingFolder(quint32 entryId);
    void copyFullPath(quint32 entryId);
    void runSearch();
    void applyDarkTitlebar();

    // Refresh chip indexed-state + the idle status line from the controller.
    void refreshIndexedState();
    void setBusyUi(const QString& statusText);

    // Build the allowedRoots vector from currently enabled chips.
    // Returns nullptr (no filter) when all indexed drives are enabled.
    std::vector<uint32_t> buildAllowedRoots() const;

    QLineEdit* search_ = nullptr;
    QTableView* table_ = nullptr;
    QTimer* debounce_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    DriveChipBar* chipBar_ = nullptr;

    IndexController* controller_ = nullptr;
    ResultsModel* model_ = nullptr;
    QSettings* settings_ = nullptr;

    bool indexReady_ = false;
    quint64 searchSeq_ = 0;     // increments per dispatched query
    quint64 lastShownSeq_ = 0;  // newest seq whose result is displayed

    QString crawlingLetter_;  // drive currently being crawled (for status text)
};
