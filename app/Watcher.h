#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Watcher — per-local-drive filesystem change monitor (ReadDirectoryChangesW).
//
// One Watcher owns one background thread per watched LOCAL (DRIVE_FIXED) root.
// Each thread runs ReadDirectoryChangesW on the root handle, watching the whole
// subtree for name/dir/size/last-write changes, and pushes parsed events into a
// shared, mutex-guarded queue. The controller drains that queue on a debounce
// timer (≤ every 2 s) and applies the batch under its mutation barrier.
//
// The watcher never touches the Index itself: it only reports raw path-level
// events. Translating those into index ops (path-map lookup, tombstone,
// updateMeta, appendOne) is the controller's job, which alone holds the barrier.
//
// Network drives are NOT watched here (RDCW is unreliable over SMB/WebDAV); the
// controller handles those via periodic full rescans.
// ---------------------------------------------------------------------------

class Watcher : public QObject {
    Q_OBJECT

public:
    enum class Action : uint8_t {
        Added,       // FILE_ACTION_ADDED
        Removed,     // FILE_ACTION_REMOVED
        Modified,    // FILE_ACTION_MODIFIED
        RenamedOld,  // FILE_ACTION_RENAMED_OLD_NAME
        RenamedNew,  // FILE_ACTION_RENAMED_NEW_NAME
    };

    // One filesystem change. `root` is the watched drive root ("C:"); `relPath`
    // is the path relative to that root, using backslash separators and the
    // original case as reported by the OS (e.g. "사진\\foo.txt").
    struct Event {
        QString root;
        Action action;
        std::wstring relPath;
    };

    explicit Watcher(QObject* parent = nullptr);
    ~Watcher() override;

    // Start watching one local drive root ("C:"). Spawns a dedicated thread.
    // Safe to call once per root; duplicate roots are ignored.
    void addRoot(const QString& driveLetter);

    // Stop all threads and join them. Idempotent; called from shutdown().
    void stop();

    // True if any watch thread reported a buffer overflow for the given root
    // since the last call; clears the flag. The controller polls this to decide
    // whether a full rescan of that drive is needed (RDCW dropped events).
    bool takeOverflow(QString& overflowedRoot);

    // Atomically swap out the accumulated event batch. Returns events queued
    // since the last drain (may be empty). Called by the controller's debounce.
    std::vector<Event> drain();

    // Number of currently watched roots.
    int watchedCount() const;

signals:
    // Emitted (queued) whenever new events arrive, so the controller can arm
    // its debounce timer. Carries no data; the controller calls drain().
    void eventsPending();

private:
    struct RootWatch;

    void watchLoop(RootWatch* rw);

    std::mutex mutex_;                 // guards queue_ + overflow flags
    std::vector<Event> queue_;
    std::vector<std::unique_ptr<RootWatch>> roots_;
    std::atomic<bool> stopping_{false};
};
