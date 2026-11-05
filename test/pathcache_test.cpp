// pathcache tests. No arguments: the unit tests, in a temporary folder.
// With a file of recorded lookups (one per line: 'o' open or 's' stat, then the path; see
// re/load_time.md) and roots: replays them with and without the cache and compares every answer.
//   pathcache_test <lookups.txt> <exe dir> [marker...]
#include <cstdio>
#include <string>
#include <vector>

#include "../src/pathcache.h"

using pathcache::Answer;

namespace {

int g_failures = 0;
void Expect(const wchar_t* what, Answer got, Answer want) {
    if (got == want) return;
    wprintf(L"FAIL %s: got %d, want %d\n", what, static_cast<int>(got), static_cast<int>(want));
    ++g_failures;
}

void Touch(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

int UnitTests() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring root = std::wstring(temp) + L"pathcache_test\\";
    if (root.find(L'~') != std::wstring::npos) {
        wchar_t longName[MAX_PATH];
        GetLongPathNameW(root.substr(0, root.size() - 15).c_str(), longName, MAX_PATH);
        root = std::wstring(longName) + L"pathcache_test\\";
    }
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"Sub").c_str(), nullptr);
    Touch(root + L"Sub\\File.TXT");

    pathcache::Cache c;
    c.AddRoot(root.c_str());
    auto q = [&](const std::wstring& rel) { return c.Query((root + rel).c_str()); };
    Expect(L"existing file", q(L"Sub\\File.TXT"), Answer::Unknown);
    Expect(L"other case", q(L"sub\\file.txt"), Answer::Unknown);
    Expect(L"forward slashes", q(L"sub/file.txt"), Answer::Unknown);
    Expect(L"missing file", q(L"Sub\\Nope.txt"), Answer::MissingFile);
    Expect(L"missing folder", q(L"Nope\\file.txt"), Answer::MissingPath);
    Expect(L"missing deep", q(L"Nope\\a\\b\\c.txt"), Answer::MissingPath);
    Expect(L"file as a folder", q(L"Sub\\File.TXT\\x"), Answer::Unknown);
    Expect(L"dot", q(L"Sub\\.\\Nope.txt"), Answer::Unknown);
    Expect(L"dot dot", q(L"Sub\\..\\Nope.txt"), Answer::Unknown);
    Expect(L"trailing dot", q(L"Sub\\Nope."), Answer::Unknown);
    Expect(L"trailing space", q(L"Sub\\Nope "), Answer::Unknown);
    Expect(L"short name", q(L"Sub\\FILE~1.TXT"), Answer::Unknown);
    Expect(L"stream", q(L"Sub\\File.TXT:x"), Answer::Unknown);
    Expect(L"wildcard", q(L"Sub\\*"), Answer::Unknown);
    Expect(L"non-ASCII", q(L"Sub\\caf\u00e9.txt"), Answer::Unknown);
    Expect(L"folder itself", q(L"Sub\\"), Answer::Unknown);
    Expect(L"outside the root", c.Query(L"C:\\Windows\\no_such_file_here"), Answer::Unknown);
    Expect(L"relative", c.Query(L"Sub\\Nope.txt"), Answer::Unknown);
    Expect(L"UNC", c.Query(L"\\\\?\\C:\\Windows\\x"), Answer::Unknown);

    // A file created through the hook's path: the listing is dropped.
    Expect(L"before create", q(L"Sub\\New.txt"), Answer::MissingFile);
    c.Invalidate((root + L"Sub\\New.txt").c_str());
    Touch(root + L"Sub\\New.txt");
    Expect(L"after create", q(L"Sub\\New.txt"), Answer::Unknown);
    DeleteFileW((root + L"Sub\\New.txt").c_str());
    c.Invalidate((root + L"Sub\\New.txt").c_str());
    Expect(L"after delete", q(L"Sub\\New.txt"), Answer::MissingFile);

    DeleteFileW((root + L"Sub\\File.TXT").c_str());
    RemoveDirectoryW((root + L"Sub").c_str());
    RemoveDirectoryW(root.c_str());
    printf(g_failures ? "%d FAILED\n" : "all pathcache tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}

struct Lookup {
    bool open;
    std::wstring path;
};

// What the file system says: 0 exists, else the error.
DWORD Real(const Lookup& l) {
    if (l.open) {
        HANDLE h = CreateFileW(l.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return GetLastError();
        CloseHandle(h);
        return 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA d;
    return GetFileAttributesExW(l.path.c_str(), GetFileExInfoStandard, &d) ? 0 : GetLastError();
}

double Now() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) / f.QuadPart;
}

int Replay(int argc, wchar_t** argv) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, argv[1], L"r") != 0 || !f) return 2;
    std::vector<Lookup> all;
    char line[2048];
    wchar_t wide[2048];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n < 2) continue;
        if (!MultiByteToWideChar(CP_UTF8, 0, line + 1, -1, wide, 2048)) continue;
        all.push_back({line[0] == 'o', wide});
    }
    fclose(f);
    printf("%zu lookups\n", all.size());

    pathcache::Cache c;
    c.AddRoot(argv[2]);
    for (int i = 3; i < argc; ++i) c.AddRootMarker(argv[i]);

    double t0 = Now();
    size_t missing = 0;
    for (const Lookup& l : all) missing += Real(l) != 0;
    double real1 = Now() - t0;
    t0 = Now();
    for (const Lookup& l : all) Real(l);
    double real2 = Now() - t0;

    t0 = Now();
    size_t file = 0, path = 0, passed = 0;
    for (const Lookup& l : all) {
        switch (c.Query(l.path.c_str())) {
            case Answer::MissingFile: ++file; break;
            case Answer::MissingPath: ++path; break;
            default: ++passed; Real(l); break;
        }
    }
    double cached1 = Now() - t0;
    size_t listings = c.Listings();
    t0 = Now();
    for (const Lookup& l : all) {
        if (c.Query(l.path.c_str()) == Answer::Unknown) Real(l);
    }
    double cached2 = Now() - t0;

    size_t wrong = 0, wrongCode = 0, shown = 0;
    for (const Lookup& l : all) {
        Answer a = c.Query(l.path.c_str());
        if (a == Answer::Unknown) continue;
        DWORD real = Real(l);
        bool ok = real != 0;
        bool code = a == Answer::MissingFile ? real == ERROR_FILE_NOT_FOUND : real == ERROR_PATH_NOT_FOUND;
        if (!ok) ++wrong;
        else if (!code) ++wrongCode;
        if ((!ok || !code) && shown < 20) {
            ++shown;
            wprintf(L"  %s: cache %d, real %lu: %s\n", ok ? L"code" : L"WRONG", static_cast<int>(a), real, l.path.c_str());
        }
    }
    printf("really missing: %zu of %zu\n", missing, all.size());
    printf("answered by the cache: %zu missing file, %zu missing path; passed on %zu; %zu listings\n", file, path, passed, listings);
    printf("real API: %.2f s, then %.2f s; with the cache: %.2f s (with listing), then %.2f s\n", real1, real2, cached1, cached2);
    printf("answers wrong: %zu; right but another error code: %zu\n", wrong, wrongCode);
    return wrong ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3) return Replay(argc, argv);
    return UnitTests();
}
