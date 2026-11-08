// Data that Paradox script keeps outside the game, without C#: log = "@sef:..." commands
// (re/script_storage.md). Two stores of text values by key: "global", shared by every game, and
// "save", which follows the save game. This header knows nothing about the game, so
// test/pdxstore_test.cpp runs all of it; dllmain.cpp, "Script storage", does the game's side.
#pragma once
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace pdxstore {

constexpr size_t kMaxName = 128;               // a key or a variable name
constexpr size_t kMaxValue = 1024 * 1024;      // one value
constexpr size_t kMaxStore = 16 * 1024 * 1024; // every key and value of one store together

// ---- Numbers ----
// Variables are CFixedPoint: the value times 100000 in an int64.
constexpr int64_t kFixedOne = 100000;

// "12", "-0.5", "3.14159": no exponent, at most five decimals (more are cut). False if not a number.
inline bool ParseFixed(const std::string& s, int64_t* out) {
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) negative = s[i++] == '-';
    uint64_t whole = 0, frac = 0;
    int digits = 0, fracDigits = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++digits) {
        whole = whole * 10 + static_cast<uint64_t>(s[i] - '0');
        if (whole > static_cast<uint64_t>(INT64_MAX / kFixedOne)) return false;
    }
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++digits) {
            if (fracDigits < 5) {
                frac = frac * 10 + static_cast<uint64_t>(s[i] - '0');
                ++fracDigits;
            }
        }
    }
    if (digits == 0 || i != s.size()) return false;
    for (; fracDigits < 5; ++fracDigits) frac *= 10;
    if (whole == static_cast<uint64_t>(INT64_MAX / kFixedOne) && frac > static_cast<uint64_t>(INT64_MAX % kFixedOne)) return false;
    int64_t v = static_cast<int64_t>(whole) * kFixedOne + static_cast<int64_t>(frac);
    *out = negative ? -v : v;
    return true;
}

// The shortest text ParseFixed reads back as the same value: "12", "-0.5", "0.00001".
inline std::string FormatFixed(int64_t v) {
    uint64_t a = v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%s%llu", v < 0 ? "-" : "", static_cast<unsigned long long>(a / kFixedOne));
    uint64_t frac = a % kFixedOne;
    if (frac) {
        char f[8];
        snprintf(f, sizeof(f), "%05llu", static_cast<unsigned long long>(frac));
        size_t len = 5;
        while (f[len - 1] == '0') --len;
        f[len] = '\0';
        snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), ".%s", f);
    }
    return buf;
}

// ---- Commands ----

// Keys and variable names: letters, digits and _ . : @ -, as Paradox script names are.
inline bool ValidName(const std::string& s) {
    if (s.empty() || s.size() > kMaxName) return false;
    for (unsigned char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                  c == ':' || c == '@' || c == '-';
        if (!ok) return false;
    }
    return true;
}

enum class Op { Set, Get, Clear, Has, Record, RecordEnd, Run };
enum class Which { Global, Save };

struct Command {
    Op op = Op::Set;
    Which store = Which::Global;
    std::string key;       // the stored key
    std::string variable;  // Set: the variable read (empty with a number); Get: the variable written
    bool number = false;   // Set: `value` holds a number written in the command
    int64_t value = 0;
};

inline std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

// The text after "@sef:". False and `error` when it isn't a command.
//   global.set KEY = VARIABLE | NUMBER     global.set VARIABLE  (KEY = VARIABLE)
//   global.get VARIABLE = KEY              global.get VARIABLE  (VARIABLE = KEY)
//   global.clear KEY    global.has KEY (a trigger)    global.record KEY    global.run KEY    record_end
// and the same with save. instead of global.
inline bool ParseCommand(const std::string& text, Command& out, std::string& error) {
    out = Command();
    std::string t = Trim(text);
    size_t space = t.find_first_of(" \t");
    std::string verb = t.substr(0, space);
    std::string rest = space == std::string::npos ? std::string() : Trim(t.substr(space));
    if (verb == "record_end") {
        if (!rest.empty()) {
            error = "record_end takes nothing after it";
            return false;
        }
        out.op = Op::RecordEnd;
        return true;
    }
    size_t dot = verb.find('.');
    std::string store = verb.substr(0, dot), op = dot == std::string::npos ? std::string() : verb.substr(dot + 1);
    if (store == "global") {
        out.store = Which::Global;
    } else if (store == "save") {
        out.store = Which::Save;
    } else {
        error = "unknown command \"" + verb + "\": it starts with global. or save.";
        return false;
    }
    std::string left = rest, right;
    size_t eq = rest.find('=');
    bool hasRight = eq != std::string::npos;
    if (hasRight) {
        left = Trim(rest.substr(0, eq));
        right = Trim(rest.substr(eq + 1));
    }
    auto name = [&](const std::string& s, const char* what) {
        if (ValidName(s)) return true;
        error = std::string(what) + " \"" + s + "\" is not a name (letters, digits, _ . : @ -, at most 128)";
        return false;
    };
    auto single = [&](Op o) {
        out.op = o;
        if (hasRight) {
            error = verb + " takes one key, without =";
            return false;
        }
        out.key = left;
        return name(left, "key");
    };
    if (op == "set") {
        out.op = Op::Set;
        out.key = left;
        if (!name(left, "key")) return false;
        if (!hasRight) {
            out.variable = left;
            return true;
        }
        if (ParseFixed(right, &out.value)) {
            out.number = true;
            return true;
        }
        out.variable = right;
        return name(right, "variable");
    }
    if (op == "get") {
        out.op = Op::Get;
        out.variable = left;
        out.key = hasRight ? right : left;
        return name(left, "variable") && name(out.key, "key");
    }
    if (op == "clear") return single(Op::Clear);
    if (op == "has") return single(Op::Has);
    if (op == "record") return single(Op::Record);
    if (op == "run") return single(Op::Run);
    error = "unknown command \"" + verb + "\"; known: set, get, clear, has, record, run, and record_end";
    return false;
}

// Whether a line written while recording is kept: script has "=", "{" or "}"; headings, rules
// ("-----") and comments don't. The line is kept trimmed.
inline bool KeepRecordedLine(const std::string& line, std::string* kept) {
    std::string t = Trim(line);
    if (t.empty() || t[0] == '#') return false;
    if (t.find_first_of("={}") == std::string::npos) return false;
    *kept = t;
    return true;
}

// ---- The stores ----

class Store {
public:
    // False when the store would grow past kMaxStore, or the value past kMaxValue.
    bool Set(const std::string& key, const std::string& value) {
        if (value.size() > kMaxValue) return false;
        auto it = values_.find(key);
        size_t old = it == values_.end() ? 0 : key.size() + it->second.size();
        size_t chars = chars_ - old + key.size() + value.size();
        if (chars > kMaxStore) return false;
        if (it != values_.end() && it->second == value) return true;
        values_[key] = value;
        chars_ = chars;
        dirty_ = true;
        return true;
    }
    const std::string* Get(const std::string& key) const {
        auto it = values_.find(key);
        return it == values_.end() ? nullptr : &it->second;
    }
    bool Remove(const std::string& key) {
        auto it = values_.find(key);
        if (it == values_.end()) return false;
        chars_ -= key.size() + it->second.size();
        values_.erase(it);
        dirty_ = true;
        return true;
    }
    void Clear() {
        if (!values_.empty()) dirty_ = true;
        values_.clear();
        chars_ = 0;
    }
    bool Empty() const { return values_.empty(); }
    size_t Count() const { return values_.size(); }
    bool Dirty() const { return dirty_; }
    void SetClean() { dirty_ = false; }
    const std::map<std::string, std::string>& Values() const { return values_; }

private:
    std::map<std::string, std::string> values_;
    size_t chars_ = 0;
    bool dirty_ = false;
};

// The file: a heading, then one `key = "value"` per line, sorted; \\, \" and \n escaped.
inline std::string Serialize(const Store& store) {
    std::string out = "# stellaris-engine-fixes: data Paradox script keeps with log = \"@sef:...\"\n";
    for (const auto& kv : store.Values()) {
        out += kv.first;
        out += " = \"";
        for (char c : kv.second) {
            if (c == '\\') out += "\\\\";
            else if (c == '"') out += "\\\"";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else out += c;
        }
        out += "\"\n";
    }
    return out;
}

// Reads Serialize's text into `store` (replacing it). False and `error` naming the line otherwise;
// `store` is then empty.
inline bool Deserialize(const std::string& text, Store& store, std::string& error) {
    store.Clear();
    size_t pos = 0;
    int lineNo = 0;
    auto fail = [&](const char* why) {
        error = "line " + std::to_string(lineNo) + ": " + why;
        store.Clear();
        return false;
    };
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) return fail("no =");
        std::string key = Trim(t.substr(0, eq)), v = Trim(t.substr(eq + 1));
        if (!ValidName(key)) return fail("not a key");
        if (v.size() < 2 || v.front() != '"' || v.back() != '"') return fail("the value is not in quotes");
        std::string value;
        for (size_t i = 1; i + 1 < v.size(); ++i) {
            char c = v[i];
            if (c == '"') return fail("a quote inside the value");
            if (c != '\\') {
                value += c;
                continue;
            }
            if (i + 2 >= v.size()) return fail("a \\ at the end");
            char e = v[++i];
            if (e == '\\') value += '\\';
            else if (e == '"') value += '"';
            else if (e == 'n') value += '\n';
            else if (e == 'r') value += '\r';
            else return fail("an unknown \\ escape");
        }
        if (!store.Set(key, value)) return fail("too much data");
    }
    store.SetClean();
    return true;
}

// ---- Files ----

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// The save's name under "save games" (folder/name.sav), as Storage.cs takes it. A path outside
// "save games" gives "" when saving (an out-of-sync dump, not a save) and its own parts when loading.
inline std::string SaveKey(const std::string& path, bool saving) {
    std::string p = path;
    for (char& c : p) {
        if (c == '\\') c = '/';
    }
    static const char kSaveGames[] = "save games/";
    std::string lower = p;
    for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    size_t at = lower.rfind(kSaveGames);
    if (at != std::string::npos) p = p.substr(at + sizeof(kSaveGames) - 1);
    else if (saving) return std::string();
    std::string out;
    size_t pos = 0;
    while (pos <= p.size()) {
        size_t end = p.find('/', pos);
        if (end == std::string::npos) end = p.size();
        std::string part = p.substr(pos, end - pos);
        pos = end + 1;
        if (part.empty() || part == "." || part == ".." || part.find(':') != std::string::npos) continue;
        if (!out.empty()) out += '/';
        out += part;
    }
    return out;
}

// <dir>\saves\<folder>\<name>.txt, each part made a safe file name.
inline std::wstring SaveFile(const std::wstring& dir, const std::string& key) {
    std::wstring file = dir + L"\\saves";
    size_t pos = 0;
    while (pos <= key.size()) {
        size_t end = key.find('/', pos);
        if (end == std::string::npos) end = key.size();
        std::string part = key.substr(pos, end - pos);
        pos = end + 1;
        bool last = pos > key.size();
        if (last && part.size() > 4 && _stricmp(part.c_str() + part.size() - 4, ".sav") == 0) part.resize(part.size() - 4);
        for (char& c : part) {
            if (static_cast<unsigned char>(c) < 32 || strchr("<>:\"/\\|?*", c)) c = '_';
        }
        while (!part.empty() && (part.back() == '.' || part.back() == ' ')) part.pop_back();
        while (!part.empty() && part.front() == ' ') part.erase(0, 1);
        if (part.empty()) part = "_";
        file += L"\\" + Widen(part);
    }
    return file + L".txt";
}

inline void MakeDirectories(const std::wstring& file) {
    for (size_t i = 3; i < file.size(); ++i) {
        if (file[i] == L'\\') CreateDirectoryW(file.substr(0, i).c_str(), nullptr);
    }
}

// False if the file can't be read; a missing file reads as "" with `missing` set.
inline bool ReadFileText(const std::wstring& file, std::string& out, bool* missing) {
    out.clear();
    *missing = false;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        *missing = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
        return *missing;
    }
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart <= static_cast<LONGLONG>(2 * kMaxStore + 4096);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (::ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

// Through a .tmp file and a rename, so a crash mid-write leaves the old file.
inline bool WriteFileText(const std::wstring& file, const std::string& text) {
    MakeDirectories(file);
    std::wstring temp = file + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = text.empty() || (::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size());
    CloseHandle(h);
    if (!ok || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// Writes the store, or deletes the file when it is empty. Clears the dirty flag on success.
inline bool WriteStore(const std::wstring& file, Store& store) {
    bool ok = store.Empty() ? (DeleteFileW(file.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND ||
                               GetLastError() == ERROR_PATH_NOT_FOUND)
                            : WriteFileText(file, Serialize(store));
    if (ok) store.SetClean();
    return ok;
}

// Reads the store from its file; a missing file is an empty store. False and `error` otherwise.
inline bool ReadStore(const std::wstring& file, Store& store, std::string& error) {
    std::string text;
    bool missing = false;
    if (!ReadFileText(file, text, &missing)) {
        store.Clear();
        store.SetClean();
        error = "cannot read the file";
        return false;
    }
    if (missing) {
        store.Clear();
        store.SetClean();
        return true;
    }
    return Deserialize(text, store, error);
}

}  // namespace pdxstore
