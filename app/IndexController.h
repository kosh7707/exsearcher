#pragma once

#include "exsearcher/Index.h"
#include "exsearcher/CrawlIndexer.h"
#include "exsearcher/SearchEngine.h"

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

// Owns the core Index/SearchEngine and orchestrates the background crawl plus
// search execution. All core access happens off the UI thread; results are
// delivered back to the UI thread via queued signals.
//
// Threading contract:
//  - crawl runs on a dedicated std::thread, started by start().
//  - searches run via QtConcurrent on searchPool_ (maxThreadCount=1), NOT on
//    the global thread pool.  Using a private pool means shutdown() can drain
//    all in-flight searches (waitForDone) before engine_ is destroyed, which
//    eliminates the use-after-free that would occur if the window closes while
//    a search task is in flight.
//  - the Index is NOT read while the crawl is in flight (core appends are not
//    safe to read concurrently); search() refuses until indexingDone() fired.
class IndexController : public QObject {
    Q_OBJECT

public:
    explicit IndexController(QObject* parent = nullptr);
    ~IndexController() override;

    // Begin crawling the given roots on a background thread. If roots is empty,
    // auto-detect fixed + remote drives.
    void start(const QVector<QString>& roots);

    // Request the crawl thread to stop and join it. Safe to call repeatedly.
    void shutdown();

    bool indexReady() const { return ready_.load(std::memory_order_acquire); }

    // Read-only access for the model (UI thread, only valid after ready).
    const exsearcher::Index& index() const { return index_; }

    // Queue a search. seq is echoed back with the result so the UI can drop
    // superseded queries. No-op (emits empty) if the index isn't ready yet.
    void requestSearch(const QString& query, quint64 seq);

signals:
    // Throttled live progress during crawl: entries indexed so far.
    void progress(quint64 entries);
    // Crawl finished. Stats for the status bar.
    void indexingDone(quint64 files, quint64 dirs, quint64 elapsedMs);
    // Search completed. indices are entry ids; total is the true match count
    // (may exceed indices.size() when capped); capped indicates truncation.
    void searchReady(quint64 seq, QVector<quint32> indices, quint64 total,
                     bool capped, quint64 elapsedMs);

private:
    static QVector<QString> autoDetectRoots();
    void crawlThreadMain(QVector<QString> roots);

    exsearcher::Index index_;
    exsearcher::CrawlIndexer crawler_;
    std::unique_ptr<exsearcher::SearchEngine> engine_;

    // Private single-thread pool for search tasks. Keeps searches serialized
    // and lets shutdown() drain them safely before engine_ is destroyed.
    QThreadPool searchPool_;

    std::thread crawlThread_;
    std::atomic<bool> ready_{false};
};
