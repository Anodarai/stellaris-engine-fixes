#include "../src/logging.h"

#include <string>

static void Record(FILE* file, LogLevel level, const char* fmt, ...) {
    const SYSTEMTIME timestamp{2026, 11, 0, 11, 18, 30, 0, 123};
    va_list args;
    va_start(args, fmt);
    WriteLogRecord(file, timestamp, level, "test", fmt, args);
    va_end(args);
}

int main() {
    FILE* file = nullptr;
    if (tmpfile_s(&file) != 0 || !file) {
        fprintf(stderr, "Could not open a temporary log\n");
        return 1;
    }

    const std::string longMessage(2048, 'x');
    Record(file, LogLevel::Error, "mod\\name\r\nbad\t%s%c%c", " | field", 0x11, 0);
    Record(file, LogLevel::Warn, "%s", longMessage.c_str());
    Record(file, LogLevel::Info, "ready");
    Record(file, LogLevel::Debug, "name=caf\xc3\xa9");
    Record(file, LogLevel::Stats, "calls=%d avg_us=%.1f", 12, 5.5);
    rewind(file);
    std::string output;
    char buffer[4096];
    while (size_t size = fread(buffer, 1, sizeof(buffer), file)) output.append(buffer, size);
    fclose(file);

    const std::string expected =
        "2026-11-11T18:30:00.123Z ERROR test | mod\\\\name\\r\\nbad\\t | field\\x11\\x00\n"
        "2026-11-11T18:30:00.123Z WARN  test | " + longMessage + "\n"
        "2026-11-11T18:30:00.123Z INFO  test | ready\n"
        "2026-11-11T18:30:00.123Z DEBUG test | name=caf\xc3\xa9\n"
        "2026-11-11T18:30:00.123Z STATS test | calls=12 avg_us=5.5\n";
    if (output != expected) {
        fprintf(stderr, "Unexpected log output:\n%s", output.c_str());
        return 1;
    }
    puts("Log record format passed");
}
