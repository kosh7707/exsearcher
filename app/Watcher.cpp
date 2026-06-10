#include "Watcher.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
// 64 KB notification buffer. Large enough that bursts rarely overflow; on the
// rare overflow ReadDirectoryChangesW returns a zero-length record and we flag
// the root for a full rescan instead of trying to recover partial events.
constexpr DWORD kBufBytes = 64 * 1024;
}  // namespace

// Per-root watch state: the directory handle, a manual-reset cancel event used
// to break out of the blocking GetOverlappedResult wait, the overlapped struct,
// the thread, and a sticky overflow flag.
struct Watcher::RootWatch {
    QString letter;            // "C:"
    std::wstring rootPath;     // "C:\\"
    HANDLE dirHandle = INVALID_HANDLE_VALUE;
    HANDLE cancelEvent = nullptr;
    OVERLAPPED ov{};
    std::thread thread;
    std::atomic<bool> overflow{false};
};

Watcher::Watcher(QObject* parent) : QObject(parent) {}

Watcher::~Watcher() {
    stop();
}

void Watcher::addRoot(const QString& driveLetter) {
    // Ignore duplicates.
    for (const auto& rw : roots_)
        if (rw->letter == driveLetter)
            return;

    auto rw = std::make_unique<RootWatch>();
    rw->letter = driveLetter;
    rw->rootPath = (driveLetter + QStringLiteral("\\")).toStdWString();

    // FILE_FLAG_OVERLAPPED so we can wait on either the read completion or the
    // cancel event and shut down cleanly. FILE_FLAG_BACKUP_SEMANTICS is required
    // to open a directory handle.
    rw->dirHandle = CreateFileW(
        rw->rootPath.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (rw->dirHandle == INVALID_HANDLE_VALUE)
        return;  // can't watch this drive; silently skip (rare, permissions)

    rw->cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    rw->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    RootWatch* raw = rw.get();
    roots_.push_back(std::move(rw));
    raw->thread = std::thread([this, raw] { watchLoop(raw); });
}

void Watcher::stop() {
    stopping_.store(true, std::memory_order_release);
    for (auto& rw : roots_) {
        if (rw->cancelEvent)
            SetEvent(rw->cancelEvent);
    }
    for (auto& rw : roots_) {
        if (rw->thread.joinable())
            rw->thread.join();
        if (rw->ov.hEvent)
            CloseHandle(rw->ov.hEvent);
        if (rw->cancelEvent)
            CloseHandle(rw->cancelEvent);
        if (rw->dirHandle != INVALID_HANDLE_VALUE)
            CloseHandle(rw->dirHandle);
        rw->ov.hEvent = nullptr;
        rw->cancelEvent = nullptr;
        rw->dirHandle = INVALID_HANDLE_VALUE;
    }
    roots_.clear();
}

bool Watcher::takeOverflow(QString& overflowedRoot) {
    for (auto& rw : roots_) {
        if (rw->overflow.exchange(false, std::memory_order_acq_rel)) {
            overflowedRoot = rw->letter;
            return true;
        }
    }
    return false;
}

std::vector<Watcher::Event> Watcher::drain() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Event> out;
    out.swap(queue_);
    return out;
}

int Watcher::watchedCount() const {
    return static_cast<int>(roots_.size());
}

void Watcher::watchLoop(RootWatch* rw) {
    std::vector<char> buffer(kBufBytes);

    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME |
                         FILE_NOTIFY_CHANGE_DIR_NAME |
                         FILE_NOTIFY_CHANGE_LAST_WRITE |
                         FILE_NOTIFY_CHANGE_SIZE;

    HANDLE waitHandles[2] = {rw->ov.hEvent, rw->cancelEvent};

    while (!stopping_.load(std::memory_order_acquire)) {
        ResetEvent(rw->ov.hEvent);
        DWORD bytesReturned = 0;

        BOOL ok = ReadDirectoryChangesW(
            rw->dirHandle, buffer.data(), kBufBytes,
            TRUE /* watch subtree */, filter, &bytesReturned, &rw->ov, nullptr);
        if (!ok) {
            // Couldn't queue the read; treat as overflow / give up this drive.
            rw->overflow.store(true, std::memory_order_release);
            emit eventsPending();
            return;
        }

        const DWORD wr = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0 + 1) {
            // Cancel event signalled: cancel the pending I/O and exit.
            CancelIo(rw->dirHandle);
            // Reap the cancelled overlapped result so the buffer isn't touched
            // after we return.
            DWORD dummy = 0;
            GetOverlappedResult(rw->dirHandle, &rw->ov, &dummy, TRUE);
            return;
        }
        if (wr != WAIT_OBJECT_0)
            return;  // unexpected; bail

        DWORD transferred = 0;
        if (!GetOverlappedResult(rw->dirHandle, &rw->ov, &transferred, FALSE)) {
            rw->overflow.store(true, std::memory_order_release);
            emit eventsPending();
            continue;
        }

        // Zero-length notification = buffer overflowed; the OS dropped events.
        // Flag a full rescan and keep watching.
        if (transferred == 0) {
            rw->overflow.store(true, std::memory_order_release);
            emit eventsPending();
            continue;
        }

        // Parse the FILE_NOTIFY_INFORMATION record chain into Events.
        std::vector<Event> parsed;
        size_t offset = 0;
        for (;;) {
            auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                buffer.data() + offset);

            const size_t nameChars = fni->FileNameLength / sizeof(wchar_t);
            std::wstring rel(fni->FileName, nameChars);

            Event ev;
            ev.root = rw->letter;
            ev.relPath = std::move(rel);
            bool keep = true;
            switch (fni->Action) {
                case FILE_ACTION_ADDED: ev.action = Action::Added; break;
                case FILE_ACTION_REMOVED: ev.action = Action::Removed; break;
                case FILE_ACTION_MODIFIED: ev.action = Action::Modified; break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    ev.action = Action::RenamedOld; break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    ev.action = Action::RenamedNew; break;
                default: keep = false; break;
            }
            if (keep)
                parsed.push_back(std::move(ev));

            if (fni->NextEntryOffset == 0)
                break;
            offset += fni->NextEntryOffset;
            if (offset >= buffer.size())
                break;  // defensive
        }

        if (!parsed.empty()) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (auto& e : parsed)
                    queue_.push_back(std::move(e));
            }
            emit eventsPending();
        }
    }
}
