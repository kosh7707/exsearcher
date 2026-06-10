#pragma once

#include <QMainWindow>
#include <QVector>

#include <cstdint>

class QLineEdit;
class QTableView;
class QTimer;
class QLabel;
class IndexController;
class ResultsModel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QVector<QString>& roots, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onSearchTextChanged();
    void onDebounceTimeout();
    void onProgress(quint64 entries);
    void onIndexingDone(quint64 files, quint64 dirs, quint64 elapsedMs);
    void onSearchReady(quint64 seq, QVector<quint32> indices, quint64 total,
                       bool capped, quint64 elapsedMs);
    void onActivated(const QModelIndex& index);
    void onContextMenu(const QPoint& pos);

private:
    void openEntry(quint32 entryId);
    void openContainingFolder(quint32 entryId);
    void copyFullPath(quint32 entryId);
    void runSearch();

    QLineEdit* search_ = nullptr;
    QTableView* table_ = nullptr;
    QTimer* debounce_ = nullptr;
    QLabel* status_ = nullptr;

    IndexController* controller_ = nullptr;
    ResultsModel* model_ = nullptr;

    bool indexReady_ = false;
    quint64 searchSeq_ = 0;     // increments per dispatched query
    quint64 lastShownSeq_ = 0;  // newest seq whose result is displayed
};
