#pragma once

#include <QAbstractTableModel>
#include <QFileIconProvider>
#include <QHash>
#include <QIcon>
#include <QVector>

#include <cstdint>

namespace exsearcher {
class Index;
}

// Table model over a vector of entry indices into the core Index. data() pulls
// from the Index lazily (full path reconstruction only for visible rows).
// Columns: Name | Path | Size | Modified.
class ResultsModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { ColName = 0, ColPath, ColSize, ColModified, ColumnCount };

    explicit ResultsModel(QObject* parent = nullptr);

    // The Index is owned elsewhere; it must outlive the model and be fully
    // populated (crawl finished) before rows are set.
    void setIndex(const exsearcher::Index* index) { index_ = index; }

    void setRows(QVector<quint32> rows);
    void clearRows();

    // Entry index backing a model row (for activation/context-menu actions).
    quint32 entryIndex(int row) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

private:
    QIcon iconForEntry(quint32 id) const;

    const exsearcher::Index* index_ = nullptr;
    QVector<quint32> rows_;

    // Icon cache: keyed by extension (lowercase) for files, "." for directories.
    // QFileIconProvider shell calls are expensive — cache per extension.
    mutable QFileIconProvider iconProvider_;
    mutable QHash<QString, QIcon> iconCache_;
};
