#include "IndexController.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QCoreApplication>
#include <QDir>
#include <QtConcurrent>

#include <chrono>

using namespace exsearcher;

namespace {
constexpr uint32_t kMaxResults = 100000;

uint64_t nowFiletime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
}  // namespace

IndexController::IndexController(QObject* parent) : QObject(parent) {
    qRegisterMetaType<QVector<quint32>>("QVector<quint32>");
    searchPool_.setMaxThreadCount(1);
    detectedDrives_ = detectDrives();
}

IndexController::~IndexController() {
    shutdown();
}

QString IndexController::snapshotPath() const {
    return QCoreApplication::applicationDirPath() +
           QStringLiteral("/data/index.exsdb");
}

QVector<DriveInfo> IndexController::detectDrives() {
    QVector<DriveInfo> drives;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        UINT type = GetDriveTypeW(root);
        if (type == DRIVE_FIXED || type == DRIVE_REMOTE) {
            DriveInfo di;
            di.letter = QString::fromWCharArray(root, 2);
            di.isRemote = (type == DRIVE_REMOTE);
            drives.push_back(di);
        }
    }
    return drives;
}

void IndexController::rebuildIndexedRoots() {
    indexedRoots_.clear();
    driveRootIndex_.clear();
    const auto& entries = index_.entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].parentIdx != Index::kNoParent)
            continue;
        std::string rootName = index_.name(static_cast<uint32_t>(i));
        QString name = QString::fromStdString(rootName);
        // Root name is a drive letter ("C:") or a path; the drive chip key is
        // the first two chars when it looks like a "X:" drive.
        QString letter = name.left(2);
        IndexedRoot ir;
        ir.letter = letter;
        ir.rootEntryIdx = static_cast<quint32>(i);
        ir.crawledAtFiletime = 0;  // filled from snapshot meta below if present
        indexedRoots_.push_back(ir);
        driveRootIndex_[letter] = static_cast<quint32>(i);
    }
}

bool IndexController::loadSnapshot() {
    std::vector<RootMeta> roots;
    const std::wstring path = snapshotPath().toStdWString();
    if (!exsearcher::loadSnapshot(index_, roots, path))
        return false;

    rebuildIndexedRoots();
    // Overlay crawl timestamps from the snapshot metadata.
    for (const RootMeta& rm : roots) {
        const QString letter = QString::fromStdString(rm.rootPathU8).left(2);
        for (IndexedRoot& ir : indexedRoots_)
            if (ir.letter == letter)
                ir.crawledAtFiletime = rm.crawledAtFiletime;
    }

    engine_ = std::make_unique<SearchEngine>(index_);
    ready_.store(true, std::memory_order_release);
    return true;
}

bool IndexController::isIndexed(const QString& letter) const {
    return driveRootIndex_.contains(letter);
}

void IndexController::quiesce() {
    // Drain in-flight searches so no lambda reads engine_/index_ during mutation.
    searchPool_.clear();
    searchPool_.waitForDone();
    // Join any running crawl thread.
    if (crawler_)
        crawler_->requestCancel();
    if (crawlThread_.joinable())
        crawlThread_.join();
}

void IndexController::shutdown() {
    quiesce();
    // engine_ destroyed at controller death; all users already drained/joined.
}

void IndexController::crawlDrive(const QString& letter) {
    quiesce();
    busy_.store(true, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    QVector<QString> crawlList{letter};
    crawlThread_ = std::thread([this, crawlList] {
        crawlThreadMain(QVector<QString>(), crawlList);
    });
}

void IndexController::recrawlDrives(const QVector<QString>& letters) {
    quiesce();
    busy_.store(true, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    QVector<QString> list = letters;
    crawlThread_ = std::thread(
        [this, list] { crawlThreadMain(list, list); });
}

void IndexController::removeDrive(const QString& letter) {
    quiesce();
    busy_.store(true, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    crawlThread_ = std::thread([this, letter] {
        // Mutation only: removeRoot then rebuild+save. No crawl.
        const quint32 idx = rootIndexForDrive(letter);
        if (idx != UINT32_MAX)
            index_.removeRoot(static_cast<uint32_t>(idx));
        quint64 saveMs = 0;
        rebuildAndSave(saveMs);
        ready_.store(true, std::memory_order_release);
        busy_.store(false, std::memory_order_release);
        emit indexingDone(0, 0, saveMs);
    });
}

void IndexController::rebuildAndSave(quint64& saveMs) {
    using clock = std::chrono::steady_clock;
    engine_ = std::make_unique<SearchEngine>(index_);
    rebuildIndexedRoots();

    // Persist. Build RootMeta from current roots, stamping crawl time = now for
    // roots whose timestamp is unset (newly crawled), preserving others.
    std::vector<RootMeta> metas;
    const uint64_t now = nowFiletime();
    const auto& entries = index_.entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].parentIdx != Index::kNoParent)
            continue;
        RootMeta rm;
        rm.rootEntryIdx = static_cast<uint32_t>(i);
        rm.rootPathU8 = index_.name(static_cast<uint32_t>(i));
        const QString letter = QString::fromStdString(rm.rootPathU8).left(2);
        rm.crawledAtFiletime = now;
        for (const IndexedRoot& ir : indexedRoots_)
            if (ir.letter == letter && ir.crawledAtFiletime != 0)
                rm.crawledAtFiletime = ir.crawledAtFiletime;
        metas.push_back(std::move(rm));
    }

    const QString path = snapshotPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    const auto t0 = clock::now();
    saveSnapshot(index_, metas, path.toStdWString());
    const auto t1 = clock::now();
    saveMs = static_cast<quint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    // Re-stamp the freshly-saved timestamps into indexedRoots_ for the UI.
    for (const RootMeta& rm : metas) {
        const QString letter = QString::fromStdString(rm.rootPathU8).left(2);
        for (IndexedRoot& ir : indexedRoots_)
            if (ir.letter == letter)
                ir.crawledAtFiletime = rm.crawledAtFiletime;
    }
}

void IndexController::crawlThreadMain(QVector<QString> removeFirst,
                                      QVector<QString> crawlList) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    // Remove requested roots first (recrawl path). Walk in descending root-idx
    // order so earlier removals don't shift the indices of later ones.
    if (!removeFirst.isEmpty()) {
        QVector<quint32> idxs;
        for (const QString& letter : removeFirst) {
            const quint32 idx = rootIndexForDrive(letter);
            if (idx != UINT32_MAX)
                idxs.push_back(idx);
        }
        std::sort(idxs.begin(), idxs.end(), std::greater<quint32>());
        for (quint32 idx : idxs)
            index_.removeRoot(static_cast<uint32_t>(idx));
        // Indices changed; refresh the map before any rootIndexForDrive use.
        rebuildIndexedRoots();
    }

    std::vector<std::wstring> wroots;
    wroots.reserve(crawlList.size());
    for (const QString& r : crawlList)
        wroots.push_back(r.toStdWString());  // "Z:" -> crawler seeds "Z:" root

    crawler_ = std::make_unique<CrawlIndexer>(index_);
    crawler_->setProgressCallback([this](const CrawlProgress& p) {
        emit progress(static_cast<quint64>(p.entries),
                      static_cast<quint64>(p.dirsDone),
                      static_cast<quint64>(p.dirsPending),
                      QString::fromStdWString(p.currentDir));
    });

    CrawlStats stats = crawler_->crawl(wroots);

    quint64 saveMs = 0;
    rebuildAndSave(saveMs);
    ready_.store(true, std::memory_order_release);
    busy_.store(false, std::memory_order_release);

    const quint64 elapsed = static_cast<quint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() -
                                                              start)
            .count());
    Q_UNUSED(stats.skippedDirs);
    emit indexingDone(static_cast<quint64>(stats.totalFiles),
                      static_cast<quint64>(stats.totalDirs), elapsed);
}

quint32 IndexController::rootIndexForDrive(const QString& letter) const {
    auto it = driveRootIndex_.find(letter);
    return (it != driveRootIndex_.end()) ? it.value() : UINT32_MAX;
}

void IndexController::requestSearch(const QString& query, quint64 seq,
                                    const std::vector<uint32_t>* allowedRoots) {
    if (!ready_.load(std::memory_order_acquire)) {
        emit searchReady(seq, QVector<quint32>(), 0, false, 0);
        return;
    }

    std::wstring wq = query.toStdWString();
    SearchEngine* eng = engine_.get();
    if (!eng) {
        emit searchReady(seq, QVector<quint32>(), 0, false, 0);
        return;
    }

    std::shared_ptr<std::vector<uint32_t>> rootsCopy;
    if (allowedRoots && !allowedRoots->empty())
        rootsCopy = std::make_shared<std::vector<uint32_t>>(*allowedRoots);

    Q_UNUSED(QtConcurrent::run(&searchPool_, [this, eng, wq, seq, rootsCopy]() {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        const std::vector<uint32_t>* rootsPtr =
            rootsCopy ? rootsCopy.get() : nullptr;
        SearchResult res = eng->search(wq, kMaxResults, rootsPtr);
        const auto t1 = clock::now();
        const quint64 ms = static_cast<quint64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count());

        QVector<quint32> indices;
        indices.reserve(static_cast<int>(res.indices.size()));
        for (uint32_t idx : res.indices)
            indices.push_back(idx);

        const bool capped = res.totalMatches > res.indices.size();
        emit searchReady(seq, std::move(indices),
                         static_cast<quint64>(res.totalMatches), capped, ms);
    }));
}
