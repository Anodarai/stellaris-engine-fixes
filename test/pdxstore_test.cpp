// Tests src/pdxstore.h: the @sef: commands, numbers, the file format, save paths and files.
#include <windows.h>

#include <cstdio>
#include <string>

#include "../src/pdxstore.h"

using namespace pdxstore;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static void Numbers() {
    int64_t v = 0;
    CHECK(ParseFixed("12", &v) && v == 1200000);
    CHECK(ParseFixed("-0.5", &v) && v == -50000);
    CHECK(ParseFixed("+3.14159", &v) && v == 314159);
    CHECK(ParseFixed("1.234567", &v) && v == 123456);  // past five decimals: cut
    CHECK(ParseFixed("0.00001", &v) && v == 1);
    CHECK(ParseFixed("7.", &v) && v == 700000);
    CHECK(ParseFixed(".5", &v) && v == 50000);
    CHECK(!ParseFixed("", &v));
    CHECK(!ParseFixed("-", &v));
    CHECK(!ParseFixed(".", &v));
    CHECK(!ParseFixed("1e5", &v));
    CHECK(!ParseFixed("abc", &v));
    CHECK(!ParseFixed("99999999999999999999", &v));
    CHECK(FormatFixed(1200000) == "12");
    CHECK(FormatFixed(-50000) == "-0.5");
    CHECK(FormatFixed(1) == "0.00001");
    CHECK(FormatFixed(0) == "0");
    CHECK(FormatFixed(INT64_MIN) == "-92233720368547.75808");
    for (int64_t x : {int64_t(0), int64_t(1), int64_t(-1), int64_t(123456789), int64_t(-100000), INT64_MAX}) {
        CHECK(ParseFixed(FormatFixed(x), &v) && v == x);
    }
}

static void Commands() {
    Command c;
    std::string e;
    CHECK(ParseCommand("global.set my_key = my_var", c, e) && c.op == Op::Set && c.store == Which::Global &&
          c.key == "my_key" && c.variable == "my_var" && !c.number);
    CHECK(ParseCommand("  save.set  my_var  ", c, e) && c.op == Op::Set && c.store == Which::Save && c.key == "my_var" &&
          c.variable == "my_var");
    CHECK(ParseCommand("global.set k = -2.5", c, e) && c.number && c.value == -250000 && c.variable.empty());
    CHECK(ParseCommand("global.get v = k", c, e) && c.op == Op::Get && c.variable == "v" && c.key == "k");
    CHECK(ParseCommand("save.get v", c, e) && c.op == Op::Get && c.variable == "v" && c.key == "v");
    CHECK(ParseCommand("global.clear k", c, e) && c.op == Op::Clear && c.key == "k");
    CHECK(ParseCommand("global.has giga.preset", c, e) && c.op == Op::Has && c.key == "giga.preset");
    CHECK(ParseCommand("global.record giga_preset", c, e) && c.op == Op::Record && c.key == "giga_preset");
    CHECK(ParseCommand("global.run giga_preset", c, e) && c.op == Op::Run);
    CHECK(ParseCommand("record_end", c, e) && c.op == Op::RecordEnd);
    CHECK(!ParseCommand("record_end x", c, e));
    CHECK(!ParseCommand("other.set k", c, e));
    CHECK(!ParseCommand("global.frob k", c, e));
    CHECK(!ParseCommand("global.set", c, e));
    CHECK(!ParseCommand("global.set k =", c, e));
    CHECK(!ParseCommand("global.set k = two words", c, e));
    CHECK(!ParseCommand("global.clear k = v", c, e));
    CHECK(!ParseCommand("global.has a b", c, e));
    CHECK(!ParseCommand("global.set " + std::string(129, 'a'), c, e));
    CHECK(ParseCommand("global.set " + std::string(128, 'a'), c, e));
    CHECK(!ParseCommand("global.set k\"", c, e));
}

static void Recording() {
    std::string kept;
    CHECK(KeepRecordedLine("  set_global_flag = giga_x  ", &kept) && kept == "set_global_flag = giga_x");
    CHECK(KeepRecordedLine("event_target:global_event_country = {", &kept));
    CHECK(KeepRecordedLine("}", &kept) && kept == "}");
    CHECK(!KeepRecordedLine("Printing Current Preset Settings", &kept));
    CHECK(!KeepRecordedLine("----------------------------------------------------------", &kept));
    CHECK(!KeepRecordedLine("#No flag for O class stars found!", &kept));
    CHECK(!KeepRecordedLine("   ", &kept));
}

static void Format() {
    Store s;
    CHECK(s.Set("a", "1"));
    CHECK(s.Set("b.c", "line one\nline \"two\" \\ end\r"));
    CHECK(s.Set("empty", ""));
    CHECK(s.Dirty());
    std::string text = Serialize(s);
    Store t;
    std::string e;
    CHECK(Deserialize(text, t, e));
    CHECK(!t.Dirty());
    CHECK(t.Values() == s.Values());
    CHECK(Deserialize("# x\r\n\r\n  k = \"v\"  \r\n", t, e) && t.Count() == 1 && *t.Get("k") == "v");
    CHECK(!Deserialize("k = v\n", t, e) && t.Empty());
    CHECK(!Deserialize("k = \"v\\q\"\n", t, e));
    CHECK(!Deserialize("k = \"v\\\"\n", t, e));
    CHECK(!Deserialize("k = \"a\"b\"\n", t, e));
    CHECK(!Deserialize("bad key = \"v\"\n", t, e));
    CHECK(!Deserialize("novalue\n", t, e));

    Store limit;
    CHECK(!limit.Set("k", std::string(kMaxValue + 1, 'x')));
    CHECK(limit.Set("k", std::string(kMaxValue, 'x')));
    for (int i = 0; i < 14; ++i) CHECK(limit.Set("k" + std::to_string(i), std::string(kMaxValue, 'x')));
    CHECK(!limit.Set("over", std::string(kMaxValue, 'x')));  // 16 MB of values and the keys pass 16 MB
    CHECK(limit.Remove("k"));
    CHECK(!limit.Remove("k"));
    CHECK(limit.Set("over", std::string(kMaxValue, 'x')));
    limit.SetClean();
    CHECK(limit.Set("over", std::string(kMaxValue, 'x')) && !limit.Dirty());  // the same value: no change
}

static void Paths() {
    CHECK(SaveKey("save games/unitednations_1234/2230.01.01.sav", true) == "unitednations_1234/2230.01.01.sav");
    CHECK(SaveKey("C:\\Users\\x\\Documents\\Paradox Interactive\\Stellaris\\save games\\a\\b.sav", false) == "a/b.sav");
    CHECK(SaveKey("Save Games/a/b.sav", true) == "a/b.sav");
    CHECK(SaveKey("oos/dump.sav", true).empty());
    CHECK(SaveKey("oos/dump.sav", false) == "oos/dump.sav");
    CHECK(SaveKey("save games/../../x/./y.sav", true) == "x/y.sav");
    CHECK(SaveKey("C:/elsewhere/z.sav", false) == "elsewhere/z.sav");
    CHECK(SaveFile(L"D:\\g\\sefdata", "a/b.sav") == L"D:\\g\\sefdata\\saves\\a\\b.txt");
    CHECK(SaveFile(L"D", "f?o/ name. ") == L"D\\saves\\f_o\\name.txt");
    CHECK(SaveFile(L"D", "a/...") == L"D\\saves\\a\\_.txt");
    CHECK(SaveFile(L"D", "x/2230.01.01.SAV") == L"D\\saves\\x\\2230.01.01.txt");
}

static void Files() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"pdxstore_test_" + std::to_wstring(GetCurrentProcessId());
    std::wstring file = SaveFile(dir, "folder/name.sav");
    Store s, t;
    std::string e;
    CHECK(ReadStore(file, t, e) && t.Empty());  // missing: empty
    s.Set("giga_preset", "remove_all_menu_flags = yes\nset_global_flag = giga_x");
    s.Set("count", "3");
    CHECK(WriteStore(file, s) && !s.Dirty());
    CHECK(ReadStore(file, t, e) && t.Values() == s.Values());
    CHECK(GetFileAttributesW((file + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
    s.Clear();
    CHECK(WriteStore(file, s));  // empty: the file goes
    CHECK(GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(WriteStore(file, s));  // and again, with nothing to delete
    std::string bad = "k = broken\n";
    CHECK(WriteFileText(file, bad));
    t.Set("x", "y");
    CHECK(!ReadStore(file, t, e) && t.Empty());
    DeleteFileW(file.c_str());
    RemoveDirectoryW((dir + L"\\saves\\folder").c_str());
    RemoveDirectoryW((dir + L"\\saves").c_str());
    RemoveDirectoryW(dir.c_str());
}

int main() {
    Numbers();
    Commands();
    Recording();
    Format();
    Paths();
    Files();
    printf(g_failures ? "%d FAILURES\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
