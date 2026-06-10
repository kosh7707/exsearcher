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
#include <QSettings>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>
#include <chrono>

using namespace exsearcher;

namespace {
constexpr uint32_t kMaxResults = 100000;

// Watcher batch apply debounce: drain + apply at most every 2 s.
constexpr int kApplyDebounceMs = 2000;
// Snapshot save debounce during live updates: at most one save per 60 s.
constexpr int kSaveDebounceMs = 60'000;
// Trigger a background rescan when tombstones exceed this fraction of entries.
constexpr double kTombstoneRescanFrac = 0.05;

uint64_t nowFiletime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Convert a FILETIME (100ns since 1601) split into a uint64.
uint64_t toFt(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
}  // namespace

IndexController::IndexController(QObject* parent) : QObject(parent) {
    qRegisterMetaType<QVector<quint32>>("QVector<quint32>");
    searchPool_.setMaxThreadCount(1);
    detectedDrives_ = detectDrives();

    watcher_ = new Watcher(this);
    connect(watcher_, &Watcher::eventsPending, this,
            &IndexController::onWatcherEventsPending, Qt::QueuedConnection);

    // Debounce timer for applying watcher batches (≤ every 2 s).
    applyTimer_ = new QTimer(this);
    applyTimer_->setSingleShot(true);
    applyTimer_->setInterval(kApplyDebounceMs);
    connect(applyTimer_, &QTimer::timeout, this,
            &IndexController::applyWatcherBatch);

    // Periodic network rescan tick (1 min).
    rescanTimer_ = new QTimer(this);
    rescanTimer_->setInterval(60'000);
    connect(rescanTimer_, &QTimer::timeout, this,
            &IndexController::onRescanTick);

    sinceLastSave_.start();
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
    rebuildDirPathMap();
    ready_.store(true, std::memory_order_release);

    // Live updates: start watching indexed local drives + arm the network
    // rescan tick (both gated by settings; no-ops if disabled).
    if (settings_) {
        // Materialize the defaults so settings.ini documents the toggles.
        if (!settings_->contains(QStringLiteral("index/watchLocal")))
            settings_->setValue(QStringLiteral("index/watchLocal"), true);
        if (!settings_->contains(QStringLiteral("index/rescanMinutes")))
            settings_->setValue(QStringLiteral("index/rescanMinutes"), 30);
    }
    startWatcherForIndexedDrives();
    if (settings_) {
        const int mins = settings_->value(QStringLiteral("index/rescanMinutes"),
                                          30).toInt();
        if (mins > 0)
            rescanTimer_->start();
    }
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
    // Stop accepting/applying live events first so nothing mutates mid-teardown.
    if (applyTimer_)
        applyTimer_->stop();
    if (rescanTimer_)
        rescanTimer_->stop();
    if (watcher_)
        watcher_->stop();  // joins all RDCW threads cleanly

    quiesce();

    // Persist any pending watcher mutations on clean shutdown (always save when
    // dirty, regardless of the 60 s debounce).
    if (dirty_ && ready_.load(std::memory_order_acquire)) {
        quint64 saveMs = 0;
        rebuildAndSave(saveMs);
        dirty_ = false;
    }
    // engine_ destroyed at controller death; all users already drained/joined.
}

void IndexController::crawlDrive(const QString& letter) {
    quiesce();
    QVector<QString> crawlList{letter};

    // Adding a drive while others are already indexed: non-blocking append swap
    // so existing search stays live. Only a from-empty first index blocks.
    if (index_.size() == 0) {
        busy_.store(true, std::memory_order_release);
        ready_.store(false, std::memory_order_release);
        crawlThread_ = std::thread([this, crawlList] {
            crawlThreadMain(QVector<QString>(), crawlList);
        });
        return;
    }

    busy_.store(true, std::memory_order_release);
    rescanning_.store(true, std::memory_order_release);
    crawlThread_ = std::thread(
        [this, crawlList] { rescanThreadMain(crawlList, /*manual=*/true); });
}

void IndexController::recrawlDrives(const QVector<QString>& letters) {
    quiesce();
    QVector<QString> list = letters;

    // First-index-of-everything is the only blocking case: there is nothing to
    // search anyway, so a temp-index swap buys nothing. Everything else runs
    // non-blocking — search stays live on the main index during the crawl.
    if (index_.size() == 0) {
        busy_.store(true, std::memory_order_release);
        ready_.store(false, std::memory_order_release);
        crawlThread_ = std::thread(
            [this, list] { crawlThreadMain(list, list); });
        return;
    }

    busy_.store(true, std::memory_order_release);
    rescanning_.store(true, std::memory_order_release);
    // ready_ stays TRUE: search keeps serving the current main index.
    crawlThread_ = std::thread(
        [this, list] { rescanThreadMain(list, /*manual=*/true); });
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
    rebuildDirPathMap();

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
    // Newly-indexed local drives become watchable; idempotent for existing ones.
    startWatcherForIndexedDrives();
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

// ---------------------------------------------------------------------------
// Live-update support (M3)
// ---------------------------------------------------------------------------

QString IndexController::dirKey(const QString& fullPath) {
    // Case-insensitive map key. Trailing separators stripped so "C:\\a\\" and
    // "C:\\a" collide as intended. Windows paths are case-insensitive, so a
    // lowercased key matches RDCW-reported paths regardless of their case.
    QString p = fullPath;
    while (p.endsWith(QLatin1Char('\\')) || p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p.toLower();
}

void IndexController::rebuildDirPathMap() {
    dirPathMap_.clear();
    const auto& entries = index_.entries();
    dirPathMap_.reserve(static_cast<int>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        const exsearcher::FileEntry& fe = entries[i];
        // Only directories are navigation targets for the watcher. Skip
        // tombstoned entries — they no longer represent a live path.
        if ((fe.attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            continue;
        if (fe.attr & exsearcher::kAttrTombstone)
            continue;
        const QString full =
            QString::fromStdString(index_.fullPath(static_cast<uint32_t>(i)));
        dirPathMap_.insert(dirKey(full), static_cast<quint32>(i));
    }
}

void IndexController::startWatcherForIndexedDrives() {
    if (!settings_)
        return;
    const bool watchLocal =
        settings_->value(QStringLiteral("index/watchLocal"), true).toBool();
    if (!watchLocal)
        return;

    // Watch only indexed LOCAL drives. A drive is local when detectedDrives_
    // marks it non-remote.
    for (const IndexedRoot& ir : indexedRoots_) {
        bool isRemote = false;
        bool known = false;
        for (const DriveInfo& di : detectedDrives_) {
            if (di.letter == ir.letter) {
                isRemote = di.isRemote;
                known = true;
                break;
            }
        }
        if (known && !isRemote)
            watcher_->addRoot(ir.letter);
    }
}

void IndexController::onWatcherEventsPending() {
    // (Re)arm the debounce: events keep coming; we apply the accumulated batch
    // when 2 s pass without restart, but QTimer single-shot start() restarts the
    // 2 s window each call, bounding apply rate to "at most every 2 s of quiet".
    if (!applyTimer_->isActive())
        applyTimer_->start();
}

void IndexController::applyWatcherBatch() {
    // If a crawl/rescan is in flight, defer: re-arm the debounce and leave the
    // watcher queue intact so no events are lost while busy.
    if (busy_.load(std::memory_order_acquire) ||
        !ready_.load(std::memory_order_acquire)) {
        applyTimer_->start();
        return;
    }

    // Pull any overflow flags first: an overflow means RDCW dropped events, so
    // the only safe recovery is a full rescan of that drive.
    QString overflowed;
    QVector<QString> rescanDrives;
    while (watcher_ && watcher_->takeOverflow(overflowed)) {
        if (!rescanDrives.contains(overflowed))
            rescanDrives.push_back(overflowed);
    }

    std::vector<Watcher::Event> events =
        watcher_ ? watcher_->drain() : std::vector<Watcher::Event>{};

    if (!events.empty()) {
        // Mutation barrier: drain in-flight searches so nothing reads index_/
        // engine_ while we mutate. The crawl thread is NOT involved here.
        searchPool_.clear();
        searchPool_.waitForDone();

        bool structural = false;  // appended/removed entries => rebuild engine

        for (const Watcher::Event& ev : events) {
            const QString full = ev.root + QStringLiteral("\\") +
                                 QString::fromStdWString(ev.relPath);
            const QString rel = QString::fromStdWString(ev.relPath);
            const int slash = rel.lastIndexOf(QLatin1Char('\\'));
            const QString parentRel = slash >= 0 ? rel.left(slash) : QString();
            const QString parentFull =
                parentRel.isEmpty() ? ev.root
                                    : (ev.root + QStringLiteral("\\") + parentRel);
            const QString leaf = slash >= 0 ? rel.mid(slash + 1) : rel;

            switch (ev.action) {
                case Watcher::Action::Added:
                case Watcher::Action::RenamedNew: {
                    auto it = dirPathMap_.find(dirKey(parentFull));
                    if (it == dirPathMap_.end())
                        break;  // parent unseen: ignore (rare; rescan covers it)
                    const quint32 parentIdx = it.value();

                    // Stat the new path for size/mtime/attr.
                    const std::wstring wfull = full.toStdWString();
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (!GetFileAttributesExW(wfull.c_str(), GetFileExInfoStandard,
                                              &fad))
                        break;  // gone already; skip

                    PendingEntry pe;
                    // Build name + lowercase mirror. WinText (the crawler's fold
                    // helper) is core-internal, so live appends fold via Qt's
                    // toLower(), which matches the search fold for the ASCII and
                    // common-script cases. The Index requires the mirror to share
                    // the primary's byte length; on any non-ASCII length change
                    // we fall back to the unfolded bytes (same rule WinText uses).
                    {
                        const std::string u8 = leaf.toUtf8().toStdString();
                        const std::string u8low =
                            leaf.toLower().toUtf8().toStdString();
                        pe.name = u8;
                        pe.nameLower = (u8low.size() == u8.size()) ? u8low : u8;
                    }
                    pe.parentIdx = parentIdx;
                    pe.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) |
                              fad.nFileSizeLow;
                    pe.mtime = toFt(fad.ftLastWriteTime);
                    pe.attr = fad.dwFileAttributes;

                    const uint32_t newIdx = index_.appendOne(pe);
                    structural = true;

                    // If the new entry is itself a directory, register it in the
                    // path map so children created under it resolve.
                    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        dirPathMap_.insert(dirKey(full), newIdx);
                    break;
                }
                case Watcher::Action::Removed:
                case Watcher::Action::RenamedOld: {
                    // Resolve the entry. If it's a known directory, tombstone it
                    // and all descendants (rare; linear scan by parent chain).
                    auto dit = dirPathMap_.find(dirKey(full));
                    if (dit != dirPathMap_.end()) {
                        const quint32 dirIdx = dit.value();
                        // Tombstone the dir + every entry whose ancestor chain
                        // passes through it. Single forward pass: mark a set of
                        // removed indices.
                        const auto& entries = index_.entries();
                        std::vector<uint8_t> drop(entries.size(), 0);
                        if (dirIdx < entries.size())
                            drop[dirIdx] = 1;
                        for (size_t i = dirIdx; i < entries.size(); ++i) {
                            const uint32_t p = entries[i].parentIdx;
                            if (p != Index::kNoParent && p < drop.size() &&
                                drop[p])
                                drop[i] = 1;
                        }
                        for (size_t i = 0; i < drop.size(); ++i)
                            if (drop[i])
                                index_.markDeleted(static_cast<uint32_t>(i));
                        dirPathMap_.remove(dirKey(full));
                        structural = true;  // search results change
                        break;
                    }
                    // Otherwise it's a file (or unknown): find it under its
                    // parent dir and tombstone the single entry.
                    auto pit = dirPathMap_.find(dirKey(parentFull));
                    if (pit == dirPathMap_.end())
                        break;
                    const quint32 parentIdx = pit.value();
                    const std::string leafLowU8 =
                        leaf.toLower().toUtf8().toStdString();
                    const auto& entries = index_.entries();
                    for (size_t i = parentIdx + 1; i < entries.size(); ++i) {
                        if (entries[i].parentIdx != parentIdx)
                            continue;
                        if (index_.isDeleted(static_cast<uint32_t>(i)))
                            continue;
                        std::string nm = index_.name(static_cast<uint32_t>(i));
                        // Compare case-insensitively via QString.
                        if (QString::fromStdString(nm).toLower().toUtf8()
                                .toStdString() == leafLowU8) {
                            index_.markDeleted(static_cast<uint32_t>(i));
                            structural = true;
                            break;
                        }
                    }
                    break;
                }
                case Watcher::Action::Modified: {
                    // Update size/mtime in place. Resolve via parent + name.
                    auto pit = dirPathMap_.find(dirKey(parentFull));
                    if (pit == dirPathMap_.end())
                        break;
                    const quint32 parentIdx = pit.value();
                    const std::wstring wfull = full.toStdWString();
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (!GetFileAttributesExW(wfull.c_str(),
                                              GetFileExInfoStandard, &fad))
                        break;
                    const std::string leafLowU8 =
                        leaf.toLower().toUtf8().toStdString();
                    const auto& entries = index_.entries();
                    for (size_t i = parentIdx + 1; i < entries.size(); ++i) {
                        if (entries[i].parentIdx != parentIdx)
                            continue;
                        std::string nm = index_.name(static_cast<uint32_t>(i));
                        if (QString::fromStdString(nm).toLower().toUtf8()
                                .toStdString() == leafLowU8) {
                            const uint64_t sz =
                                (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) |
                                fad.nFileSizeLow;
                            index_.updateMeta(static_cast<uint32_t>(i), sz,
                                              toFt(fad.ftLastWriteTime));
                            break;
                        }
                    }
                    break;
                }
            }
        }

        if (structural) {
            engine_ = std::make_unique<SearchEngine>(index_);
            dirty_ = true;
        } else if (!events.empty()) {
            dirty_ = true;  // metadata changed; worth persisting eventually
        }

        // Natural compaction guard: if tombstones exceeded the threshold, queue
        // a full background rescan of the affected drives.
        if (index_.size() > 0) {
            const double frac = static_cast<double>(index_.tombstoneCount()) /
                                static_cast<double>(index_.size());
            if (frac > kTombstoneRescanFrac) {
                for (const IndexedRoot& ir : indexedRoots_)
                    if (!rescanDrives.contains(ir.letter))
                        rescanDrives.push_back(ir.letter);
            }
        }

        // Debounced snapshot save: at most once per 60 s of live batches.
        if (dirty_ && sinceLastSave_.elapsed() >= kSaveDebounceMs) {
            quint64 saveMs = 0;
            rebuildAndSave(saveMs);  // also rebuilds map/roots, compacts on save
            dirty_ = false;
            sinceLastSave_.restart();
        }
    }

    // Kick a rescan for overflowed / tombstone-heavy drives (one at a time;
    // recrawlDrives picks the non-blocking path). Skip if already busy.
    if (!rescanDrives.isEmpty() && !busy_.load(std::memory_order_acquire)) {
        // Keep only currently-indexed drives.
        QVector<QString> valid;
        for (const QString& d : rescanDrives)
            if (isIndexed(d))
                valid.push_back(d);
        if (!valid.isEmpty())
            recrawlDrives(valid);
    }
}

void IndexController::rescanThreadMain(QVector<QString> letters, bool manual) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    Q_UNUSED(manual);

    // Crawl into a TEMP index while search keeps serving the main index.
    Index temp;
    crawler_ = std::make_unique<CrawlIndexer>(temp);
    crawler_->setProgressCallback([this](const CrawlProgress& p) {
        emit progress(static_cast<quint64>(p.entries),
                      static_cast<quint64>(p.dirsDone),
                      static_cast<quint64>(p.dirsPending),
                      QString::fromStdWString(p.currentDir));
    });

    std::vector<std::wstring> wroots;
    wroots.reserve(letters.size());
    for (const QString& r : letters)
        wroots.push_back(r.toStdWString());
    CrawlStats stats = crawler_->crawl(wroots);

    quint64 saveMs = 0;
    if (!crawler_->cancelRequested()) {
        // Mutation barrier. Flip ready_ false for the brief swap window so the
        // UI stops dispatching searches that would read index_/engine_ mid-swap
        // (requestSearch early-returns while !ready_). Then drain any search
        // already in flight before mutating. Hold time = a few ms.
        ready_.store(false, std::memory_order_release);
        searchPool_.clear();
        searchPool_.waitForDone();

        // Remove old roots for the drives we just recrawled (descending order so
        // earlier removals don't shift later indices).
        QVector<quint32> idxs;
        for (const QString& letter : letters) {
            const quint32 idx = rootIndexForDrive(letter);
            if (idx != UINT32_MAX)
                idxs.push_back(idx);
        }
        std::sort(idxs.begin(), idxs.end(), std::greater<quint32>());
        for (quint32 idx : idxs)
            index_.removeRoot(static_cast<uint32_t>(idx));

        // Append the freshly-crawled temp index, then rebuild + save.
        index_.appendIndex(temp);
        rebuildAndSave(saveMs);  // rebuilds engine, roots, path map; saves
        startWatcherForIndexedDrives();  // new local drive becomes watchable
        dirty_ = false;
        sinceLastSave_.restart();
        ready_.store(true, std::memory_order_release);  // search live again
    }

    rescanning_.store(false, std::memory_order_release);
    busy_.store(false, std::memory_order_release);

    const quint64 elapsed = static_cast<quint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() -
                                                              start)
            .count());
    Q_UNUSED(stats.skippedDirs);
    emit indexingDone(static_cast<quint64>(stats.totalFiles),
                      static_cast<quint64>(stats.totalDirs), elapsed);
}

void IndexController::onRescanTick() {
    if (!settings_)
        return;
    if (busy_.load(std::memory_order_acquire))
        return;  // one drive at a time
    if (!ready_.load(std::memory_order_acquire))
        return;

    const int mins =
        settings_->value(QStringLiteral("index/rescanMinutes"), 30).toInt();
    if (mins <= 0)
        return;

    const uint64_t now = nowFiletime();
    // 100ns ticks per minute.
    const uint64_t intervalTicks =
        static_cast<uint64_t>(mins) * 60ull * 10'000'000ull;

    // Find one due, indexed REMOTE drive and rescan it (non-blocking).
    for (const IndexedRoot& ir : indexedRoots_) {
        bool isRemote = false;
        for (const DriveInfo& di : detectedDrives_)
            if (di.letter == ir.letter) {
                isRemote = di.isRemote;
                break;
            }
        if (!isRemote)
            continue;
        if (ir.crawledAtFiletime != 0 &&
            now - ir.crawledAtFiletime < intervalTicks)
            continue;  // not due yet
        recrawlDrives({ir.letter});  // updates crawledAt via rebuildAndSave
        return;                      // only one at a time
    }
}
