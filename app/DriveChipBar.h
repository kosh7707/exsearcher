#pragma once

#include "IndexController.h"

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QSet>

class QPushButton;
class QHBoxLayout;
class QSettings;

// A horizontal row of toggleable "chip" buttons, one per detected drive.
// Three chip states:
//   - indexed + checked   : drive is indexed and included in search
//   - indexed + unchecked : drive is indexed but filtered out of search
//   - not indexed         : drive has no index yet (dashed outline / tooltip)
// The CHECKED set (search filter) persists in settings.ini; which drives are
// INDEXED is supplied by the controller (snapshot roots / crawl results).
class DriveChipBar : public QWidget {
    Q_OBJECT

public:
    explicit DriveChipBar(const QVector<DriveInfo>& drives,
                          QSettings* settings,
                          QWidget* parent = nullptr);

    // Drive letters whose chip is checked AND indexed (the active search filter).
    QVector<QString> enabledDrives() const;

    // Set enabled/disabled state of all chips (during crawl).
    void setChipsEnabled(bool enabled);

    // True when every INDEXED drive is checked (no filtering among indexed set).
    bool allIndexedEnabled() const;

    // Tell the bar which drives are currently indexed; restyles the chips.
    void setIndexedDrives(const QSet<QString>& indexed);

    // True when at least one drive is indexed.
    bool anyIndexed() const { return !indexed_.isEmpty(); }

signals:
    // A not-indexed drive chip was clicked: crawl this drive.
    void crawlRequested(const QString& letter);
    // An indexed drive's checked state changed: re-run search with new filter.
    void filterChanged();
    // Context menu "재색인" on an indexed drive: removeRoot + recrawl it.
    void recrawlDriveRequested(const QString& letter);
    // Context menu "색인에서 제거" on an indexed drive: removeRoot + save.
    void removeDriveRequested(const QString& letter);
    // Global "재색인" button: recrawl every indexed drive.
    void reindexAllRequested();

private slots:
    void onChipClicked();
    void onChipContextMenu(const QPoint& pos);
    void onReindexClicked();

private:
    void persistState();
    void restyleChip(QPushButton* btn);

    QVector<DriveInfo> drives_;
    QVector<QPushButton*> chips_;
    QPushButton* reindexBtn_ = nullptr;
    QSettings* settings_ = nullptr;

    QSet<QString> indexed_;          // letters currently indexed
    QHash<QString, bool> checked_;   // persisted checked (filter) state
};
