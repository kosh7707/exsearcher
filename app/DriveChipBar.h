#pragma once

#include "IndexController.h"

#include <QWidget>
#include <QVector>
#include <QHash>

class QPushButton;
class QHBoxLayout;
class QSettings;

// A horizontal row of toggleable "chip" buttons, one per detected drive.
// Checked = drive is enabled for search filtering.
// Persists state to settings.ini under the "drives" group.
class DriveChipBar : public QWidget {
    Q_OBJECT

public:
    explicit DriveChipBar(const QVector<DriveInfo>& drives,
                          QSettings* settings,
                          QWidget* parent = nullptr);

    // Returns the set of drive letters that are currently checked.
    QVector<QString> enabledDrives() const;

    // Set enabled/disabled state of all chips (during crawl).
    void setChipsEnabled(bool enabled);

    // True when every detected drive is checked.
    bool allEnabled() const;

signals:
    // Emitted when a chip toggles OFF for a drive that was indexed.
    void filterChanged();
    // Emitted when a chip toggles ON for a drive not yet indexed this session
    // (or when reindex is clicked): caller should re-crawl all enabled drives.
    void reindexRequested();

private slots:
    void onChipToggled(bool checked);
    void onReindexClicked();

private:
    void persistState();

    QVector<DriveInfo> drives_;
    QVector<QPushButton*> chips_;
    QPushButton* reindexBtn_ = nullptr;
    QSettings* settings_ = nullptr;
};
