#pragma once

#include "exsearcher/Index.h"
#include "exsearcher/CrawlIndexer.h"
#include "exsearcher/SearchEngine.h"
#include "exsearcher/Snapshot.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// Drive metadata exposed to the UI for chip rendering.
struct DriveInfo {
    QString letter;    // e.g. "C:"
    bool isRemote;     // DRIVE_REMOTE = true, DRIVE_FIXED = false
};

// One indexed root, surfaced to the UI so chips can show which drives are
// actually indexed (vs merely enabled in settings).
struct IndexedRoot {
    QString letter;            // drive letter, e.g. "C:"
    quint32 rootEntryIdx;      // entry index of this root in the Index
    quint64 crawledAtFiletime; // FILETIME of the last crawl
};

// Owns the core Index/SearchEngine. Loads a persisted snapshot at startup (no
// auto-crawl), then crawls drives on demand, appending to the existing index
// and persisting the result. All core access happens off the UI thread; results
// and progress are delivered back to the UI thread via queued signals.
//
// Threading contract:
//  - one crawl/mutation runs at a time on a dedicated std::thread (crawlThread_).
//  - searches run via QtConcurrent on searchPool_ (maxThreadCount=1), NOT on the
//    global pool. shutdown()/drainSearches() can wait those out before engine_
//    is destroyed or the index is mutated, avoiding use-after-free / data races.
//  - the Index is NOT read while a crawl/mutation is in flight: ready_ is set
//    false for the duration; search() refuses until it is true again.
class IndexController : public QObject {
    Q_OBJECT

public:
    explicit IndexController(QObject* parent = nullptr);
    ~IndexController() override;

    // Load the snapshot at snapshotPath() if present. Returns true on success.
    // Must be called once, before any crawl, while no threads touch the index.
    // On success the index is immediately searchable (engine built, ready_=true).
    bool loadSnapshot();

    // Crawl a single drive (e.g. "Z:") on a background thread, APPENDING to the
    // existing index. After the crawl completes it rebuilds the SearchEngine and
    // saves the snapshot (on the crawl thread), then emits indexingDone.
    void crawlDrive(const QString& letter);

    // Recrawl: removeRoot for each listed drive, then crawl them all fresh.
    // Used by "재색인" (single drive) and the global reindex button.
    void recrawlDrives(const QVector<QString>& letters);

    // Remove a drive's root from the index and persist. No recrawl.
    void removeDrive(const QString& letter);

    // Request the crawl thread to stop and join it. Safe to call repeatedly.
    void shutdown();

    bool indexReady() const { return ready_.load(std::memory_order_acquire); }
    // True from the moment a crawl/mutation is requested until indexingDone is
    // about to fire. Used by the UI to block overlapping operations.
    bool busy() const { return busy_.load(std::memory_order_acquire); }

    // Read-only access for the model (UI thread, only valid when ready).
    const exsearcher::Index& index() const { return index_; }

    // All detected drives (fixed + remote), for chip rendering.
    const QVector<DriveInfo>& detectedDrives() const { return detectedDrives_; }

    // The drives currently present in the index (from snapshot roots / crawls).
    const QVector<IndexedRoot>& indexedRoots() const { return indexedRoots_; }
    bool isIndexed(const QString& letter) const;

    // Queue a search with optional root filter (nullptr = all roots).
    // seq is echoed back with the result so the UI can drop superseded queries.
    void requestSearch(const QString& query, quint64 seq,
                       const std::vector<uint32_t>* allowedRoots = nullptr);

    // Returns the root entry index for a given drive letter (e.g. "C:").
    // Returns UINT32_MAX if the drive isn't indexed.
    quint32 rootIndexForDrive(const QString& letter) const;

signals:
    // Throttled live progress during crawl.
    void progress(quint64 entries, quint64 dirsDone, quint64 dirsPending,
                  QString currentDir);
    // Crawl/mutation finished. Stats are for the most recent operation.
    void indexingDone(quint64 files, quint64 dirs, quint64 elapsedMs);
    // Search completed. indices are entry ids; total is the true match count
    // (may exceed indices.size() when capped); capped indicates truncation.
    void searchReady(quint64 seq, QVector<quint32> indices, quint64 total,
                     bool capped, quint64 elapsedMs);

private:
    static QVector<DriveInfo> detectDrives();
    QString snapshotPath() const;

    // Drain in-flight searches and join any running crawl thread. Leaves the
    // index quiescent for direct mutation on the calling thread.
    void quiesce();

    // Rebuild engine_, recompute indexedRoots_ from the index, save snapshot.
    // Runs on the crawl thread (or directly under quiesce()). saveMs receives
    // the snapshot save time in ms.
    void rebuildAndSave(quint64& saveMs);

    // Recompute indexedRoots_ by scanning kNoParent entries.
    void rebuildIndexedRoots();

    // Body of a crawl-append operation, run on crawlThread_. removeFirst lists
    // drives to removeRoot before crawling; crawlList lists drives to crawl.
    void crawlThreadMain(QVector<QString> removeFirst, QVector<QString> crawlList);

    exsearcher::Index index_;
    std::unique_ptr<exsearcher::CrawlIndexer> crawler_;
    std::unique_ptr<exsearcher::SearchEngine> engine_;

    // Private single-thread pool for search tasks.
    QThreadPool searchPool_;

    std::thread crawlThread_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> busy_{false};

    QVector<DriveInfo> detectedDrives_;
    QVector<IndexedRoot> indexedRoots_;
    QHash<QString, quint32> driveRootIndex_;  // letter -> root entry index
};
