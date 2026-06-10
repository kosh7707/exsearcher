#include "ResultsModel.h"

#include "exsearcher/Index.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QDateTime>

namespace {

QString humanSize(uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    if (u == 0)
        return QString::number(bytes) + " B";
    return QString::number(v, 'f', 1) + ' ' + units[u];
}

// FILETIME (uint64, 100ns since 1601) -> "yyyy-MM-dd HH:mm" local time.
QString formatMtime(uint64_t mtime) {
    if (mtime == 0)
        return QString();
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(mtime & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(mtime >> 32);
    FILETIME local;
    SYSTEMTIME stime;
    if (!FileTimeToLocalFileTime(&ft, &local) ||
        !FileTimeToSystemTime(&local, &stime))
        return QString();
    QDateTime dt(QDate(stime.wYear, stime.wMonth, stime.wDay),
                 QTime(stime.wHour, stime.wMinute, stime.wSecond));
    return dt.toString("yyyy-MM-dd HH:mm");
}

// Parent directory of a full path (strip the trailing component).
QString parentDir(const QString& fullPath) {
    int pos = fullPath.lastIndexOf('\\');
    if (pos <= 0)
        return fullPath;
    return fullPath.left(pos);
}

} // namespace

ResultsModel::ResultsModel(QObject* parent) : QAbstractTableModel(parent) {}

void ResultsModel::setRows(QVector<quint32> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

void ResultsModel::clearRows() {
    beginResetModel();
    rows_.clear();
    endResetModel();
}

quint32 ResultsModel::entryIndex(int row) const {
    if (row < 0 || row >= rows_.size())
        return UINT32_MAX;
    return rows_[row];
}

int ResultsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return rows_.size();
}

int ResultsModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant ResultsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || !index_)
        return QVariant();
    const int row = index.row();
    if (row < 0 || row >= rows_.size())
        return QVariant();

    const quint32 id = rows_[row];
    const exsearcher::FileEntry& fe = index_->entry(id);
    const bool isDir = (fe.attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (role == Qt::TextAlignmentRole) {
        if (index.column() == ColSize)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }

    if (role != Qt::DisplayRole)
        return QVariant();

    switch (index.column()) {
    case ColName:
        return QString::fromStdString(index_->name(id));
    case ColPath:
        return parentDir(QString::fromStdString(index_->fullPath(id)));
    case ColSize:
        return isDir ? QString() : humanSize(fe.size);
    case ColModified:
        return formatMtime(fe.mtime);
    default:
        return QVariant();
    }
}

QVariant ResultsModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return QVariant();
    switch (section) {
    case ColName:
        return QStringLiteral("이름");
    case ColPath:
        return QStringLiteral("경로");
    case ColSize:
        return QStringLiteral("크기");
    case ColModified:
        return QStringLiteral("수정한 날짜");
    default:
        return QVariant();
    }
}
