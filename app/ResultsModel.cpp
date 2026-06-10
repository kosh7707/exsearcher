#include "ResultsModel.h"

#include "exsearcher/Index.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QColor>
#include <QDateTime>
#include <QFileInfo>

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

// Muted gray for secondary columns (path, size, date) — light theme #6B6B68.
const QColor kSecondaryColor{0x6B, 0x6B, 0x68};

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

QIcon ResultsModel::iconForEntry(quint32 id) const {
    if (!index_)
        return QIcon();

    const exsearcher::FileEntry& fe = index_->entry(id);
    const bool isDir = (fe.attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (isDir) {
        auto it = iconCache_.find(QStringLiteral("."));
        if (it != iconCache_.end())
            return it.value();
        QIcon icon = iconProvider_.icon(QFileIconProvider::Folder);
        iconCache_[QStringLiteral(".")] = icon;
        return icon;
    }

    // Extract extension from entry name (lowercase key for cache).
    const QString nameStr = QString::fromStdString(index_->name(id));
    const int dotPos = nameStr.lastIndexOf('.');
    const QString ext = (dotPos >= 0)
        ? nameStr.mid(dotPos).toLower()
        : QStringLiteral("*");

    auto it = iconCache_.find(ext);
    if (it != iconCache_.end())
        return it.value();

    // Use a dummy QFileInfo with the extension to query the shell icon.
    // This avoids hitting the actual file path (which may not exist or be slow).
    QFileInfo fi(QStringLiteral("dummy") + ext);
    QIcon icon = iconProvider_.icon(fi);
    iconCache_[ext] = icon;
    return icon;
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

    if (role == Qt::ForegroundRole) {
        // Dim secondary columns.
        if (index.column() == ColPath || index.column() == ColSize ||
            index.column() == ColModified)
            return kSecondaryColor;
        return QVariant();
    }

    if (role == Qt::DecorationRole) {
        if (index.column() == ColName)
            return iconForEntry(id);
        return QVariant();
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
