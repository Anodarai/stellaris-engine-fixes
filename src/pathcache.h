// Answers "this path doesn't exist" from directory listings, without asking the file system.
//
// The game's virtual file system looks a file up in each mounted folder in turn, so a load makes
// over a million opens and stats that fail, nearly all because the folder doesn't exist in that
// mod (re/load_time.md). A query walks the path's folders from a known root and checks each name
// in its parent's listing, which is read once.
//
// Only "missing" is ever answered from here: anything else goes to the real API, so a name wrongly
// thought to exist costs nothing but the call. Paths in any unusual form aren't answered at all.
#pragma once
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pathcache {

enum class Answer {
    Unknown,      // ask the file system
    MissingFile,  // the folder exists, the name isn't in it: ERROR_FILE_NOT_FOUND
    MissingPath,  // a folder on the way doesn't exist: ERROR_PATH_NOT_FOUND
};

class Cache {
public:
    // Paths below a root are answered. The root itself is listed, and may be written to through
    // Invalidate; its contents must not change behind the cache's back.
    void AddRoot(const wchar_t* path) {
        wchar_t norm[kMaxPath];
        size_t n = Normalize(path, norm);
        if (n == 0 || n + 1 >= kMaxPath) return;
        if (norm[n - 1] != L'\\') norm[n++] = L'\\';
        roots_.emplace_back(norm, n);
    }
    // Also a root: the part of a path up to and including this folder sequence, wherever it occurs
    // ("\\workshop\\content\\281990\\"). In the normalized form: lower case, backslashes.
    void AddRootMarker(const wchar_t* marker) { markers_.emplace_back(marker); }

    Answer Query(const wchar_t* path) {
        wchar_t norm[kMaxPath];
        size_t n = Normalize(path, norm);
        if (n == 0 || norm[n - 1] == L'\\') return Answer::Unknown;
        size_t at = RootEnd(norm, n);  // just past the root's last backslash
        if (at == 0 || at >= n) return Answer::Unknown;
        for (;;) {
            size_t end = at;
            while (end < n && norm[end] != L'\\') ++end;
            uint64_t name = Hash(norm + at, end - at);
            int state = Contains(norm, at, name);
            if (state < 0) return Answer::Unknown;  // the parent can't be listed
            if (state == 0) return end == n ? Answer::MissingFile : Answer::MissingPath;
            if (end == n) return Answer::Unknown;
            at = end + 1;
        }
    }

    // Something was or may be created or removed at this path: forget its parent's listing.
    void Invalidate(const wchar_t* path) {
        wchar_t norm[kMaxPath];
        size_t n = Normalize(path, norm);
        while (n > 0 && norm[n - 1] == L'\\') --n;
        while (n > 0 && norm[n - 1] != L'\\') --n;
        if (n == 0) return;
        uint64_t key = Hash(norm, n);
        AcquireSRWLockExclusive(&lock_);
        auto it = dirs_.find(key);
        if (it != dirs_.end()) {
            it->second.dropped = true;
            it->second.names.clear();
            ++generation_;
        }
        ReleaseSRWLockExclusive(&lock_);
    }

    uint64_t Listings() const { return listings_; }

private:
    static constexpr size_t kMaxPath = 768;

    struct Dir {
        std::wstring path;  // normalized, with its last backslash
        bool listed = false;
        bool dropped = false;  // invalidated: list again
        std::unordered_set<uint64_t> names;
    };

    static uint64_t Hash(const wchar_t* s, size_t n) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < n; ++i) h = (h ^ s[i]) * 0x100000001b3ull;
        return h;
    }

    // Lower-cases and fixes separators. 0 for anything but a plain ASCII "x:\a\b" path, in which
    // Windows' own parsing could give a different file from the walk's: "." and "..", names ending
    // in a dot or a space, 8.3 names, streams, wildcards, and names NTFS might fold differently.
    static size_t Normalize(const wchar_t* path, wchar_t* out) {
        size_t n = 0;
        if (!((path[0] >= L'a' && path[0] <= L'z') || (path[0] >= L'A' && path[0] <= L'Z'))) return 0;
        if (path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) return 0;
        out[n++] = path[0] | 0x20;
        out[n++] = L':';
        out[n++] = L'\\';
        size_t start = n;
        for (const wchar_t* p = path + 3; *p; ++p) {
            wchar_t c = *p;
            if (c >= 0x80 || c < 0x20) return 0;
            if (c == L'/') c = L'\\';
            if (c == L'\\') {
                if (n == start) continue;  // a doubled separator
                if (out[n - 1] == L'.' || out[n - 1] == L' ') return 0;
                out[n++] = c;
                start = n;
                if (n + 1 >= kMaxPath) return 0;
                continue;
            }
            if (c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'~') return 0;
            if (c >= L'A' && c <= L'Z') c |= 0x20;
            out[n++] = c;
            if (n + 1 >= kMaxPath) return 0;
        }
        if (n > start && (out[n - 1] == L'.' || out[n - 1] == L' ')) return 0;
        out[n] = 0;
        return n;
    }

    size_t RootEnd(const wchar_t* norm, size_t n) const {
        for (const std::wstring& r : roots_) {
            if (r.size() <= n && wmemcmp(norm, r.data(), r.size()) == 0) return r.size();
        }
        for (const std::wstring& m : markers_) {
            const wchar_t* hit = wcsstr(norm, m.c_str());
            if (hit) return static_cast<size_t>(hit - norm) + m.size();
        }
        return 0;
    }

    // Whether the name (hash) is in the folder norm[0, dirLen), which ends in a backslash:
    // 1 yes, 0 no, -1 the folder can't be listed.
    int Contains(const wchar_t* norm, size_t dirLen, uint64_t name) {
        uint64_t key = Hash(norm, dirLen);
        AcquireSRWLockShared(&lock_);
        auto it = dirs_.find(key);
        if (it != dirs_.end() && !it->second.dropped && SamePath(it->second, norm, dirLen)) {
            int r = !it->second.listed ? -1 : it->second.names.count(name) != 0;
            ReleaseSRWLockShared(&lock_);
            return r;
        }
        bool collision = it != dirs_.end() && !SamePath(it->second, norm, dirLen);
        uint64_t generation = generation_;
        ReleaseSRWLockShared(&lock_);
        if (collision) return -1;

        Dir fresh;
        fresh.path.assign(norm, dirLen);
        fresh.listed = List(fresh);
        int r = !fresh.listed ? -1 : fresh.names.count(name) != 0;

        AcquireSRWLockExclusive(&lock_);
        ++listings_;
        // A listing that raced an invalidation may be out of date: use it for nothing but this
        // query's "exists" (r == 1), and don't keep it.
        if (generation != generation_) {
            if (r == 0) r = -1;
        } else {
            dirs_[key] = std::move(fresh);
        }
        ReleaseSRWLockExclusive(&lock_);
        return r;
    }

    static bool SamePath(const Dir& d, const wchar_t* norm, size_t len) {
        return d.path.size() == len && wmemcmp(d.path.data(), norm, len) == 0;
    }

    static bool List(Dir& dir) {
        std::wstring pattern = dir.path + L"*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return false;
        do {
            wchar_t name[MAX_PATH];
            size_t n = 0;
            for (; fd.cFileName[n] && n < MAX_PATH - 1; ++n) {
                wchar_t c = fd.cFileName[n];
                name[n] = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c | 0x20) : c;
            }
            dir.names.insert(Hash(name, n));
        } while (FindNextFileW(h, &fd));
        bool complete = GetLastError() == ERROR_NO_MORE_FILES;
        FindClose(h);
        return complete;
    }

    std::vector<std::wstring> roots_, markers_;  // fixed before the first query
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::unordered_map<uint64_t, Dir> dirs_;
    uint64_t generation_ = 0;
    uint64_t listings_ = 0;
};

}  // namespace pathcache
