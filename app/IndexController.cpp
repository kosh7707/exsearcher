#include "IndexController.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QtConcurrent>

#include <chrono>

using namespace exsearcher;

namespace {
constexpr uint32_t kMaxResults = 100000;
}

IndexController::IndexController(QObject* parent)
    : QObject(parent), crawler_(index_) {
    qRegisterMetaType<QVector<quint32>>("QVector<quint32>");
    // One thread is enough — the 30ms debounce + sequence check handle
    // staleness; serializing searches avoids queueing redundant work.
    searchPool_.setMaxThreadCount(1);
}

IndexController::~IndexController() {
    shutdown();
}

QVector<QString> IndexController::autoDetectRoots() {
    QVector<QString> roots;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        UINT type = GetDriveTypeW(root);
        if (type == DRIVE_FIXED || type == DRIVE_REMOTE) {
            // Store as "C:" form (crawler strips trailing separators anyway).
            roots.push_back(QString::fromWCharArray(root, 2));
        }
    }
    return roots;
}

void IndexController::start(const QVector<QString>& roots) {
    QVector<QString> resolved = roots.isEmpty() ? autoDetectRoots() : roots;
    crawlThread_ = std::thread(
        [this, resolved] { crawlThreadMain(resolved); });
}

void IndexController::shutdown() {
    // Drain in-flight search tasks FIRST so no lambda can call engine_->search()
    // after engine_ is destroyed below. clear() drops queued-but-not-started
    // tasks; waitForDone() blocks until the running task (if any) finishes.
    searchPool_.clear();
    searchPool_.waitForDone();

    // Now safe to cancel + join the crawl thread (engine_ still alive during
    // crawl since ready_ was false; but belt-and-suspenders: drain pool first).
    crawler_.requestCancel();
    if (crawlThread_.joinable())
        crawlThread_.join();

    // engine_ destroyed here (unique_ptr DTOR) — all users already gone.
}

void IndexController::crawlThreadMain(QVector<QString> roots) {
    std::vector<std::wstring> wroots;
    wroots.reserve(roots.size());
    for (const QString& r : roots)
        wroots.push_back(r.toStdWString());

    crawler_.setProgressCallback([this](uint64_t n) {
        // Throttled by core (~100k). Hand to UI thread via queued signal.
        emit progress(static_cast<quint64>(n));
    });

    CrawlStats stats = crawler_.crawl(wroots);

    // Build the search engine only after the crawl fully completes, then mark
    // the index readable. Order matters: engine_ must exist before ready_.
    engine_ = std::make_unique<SearchEngine>(index_);
    ready_.store(true, std::memory_order_release);

    emit indexingDone(static_cast<quint64>(stats.totalFiles),
                      static_cast<quint64>(stats.totalDirs),
                      static_cast<quint64>(stats.elapsedMs));
}

void IndexController::requestSearch(const QString& query, quint64 seq) {
    if (!ready_.load(std::memory_order_acquire)) {
        emit searchReady(seq, QVector<quint32>(), 0, false, 0);
        return;
    }

    std::wstring wq = query.toStdWString();
    SearchEngine* eng = engine_.get();
    // Run on the private pool. shutdown() drains this pool before destroying
    // engine_, so eng is guaranteed valid for the lifetime of the task.
    Q_UNUSED(QtConcurrent::run(&searchPool_, [this, eng, wq, seq]() {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        SearchResult res = eng->search(wq, kMaxResults);
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
