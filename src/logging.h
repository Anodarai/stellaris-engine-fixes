#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <new>

enum class LogLevel { Error, Warn, Info, Debug, Stats };
using LogFn = void (*)(LogLevel level, const char* component, const char* fmt, ...);

inline const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Info: return "INFO";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Stats: return "STATS";
    }
    return "ERROR";
}

// The caller serializes records and flushes them for live readers.
inline void WriteLogRecord(FILE* file, const SYSTEMTIME& t, LogLevel level, const char* component,
                           const char* fmt, va_list args) {
    char local[1024];
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(local, sizeof(local), fmt, copy);
    va_end(copy);

    const char* message = local;
    std::unique_ptr<char[]> extended;
    if (length >= static_cast<int>(sizeof(local))) {
        size_t capacity = static_cast<size_t>(length) + 1;
        extended.reset(new (std::nothrow) char[capacity]);
        if (extended) {
            message = extended.get();
            length = vsnprintf(extended.get(), capacity, fmt, args);
            if (length >= 0 && static_cast<size_t>(length) >= capacity) length = -1;
        } else {
            length = -1;
        }
    }
    if (length < 0) {
        static constexpr char fallback[] = "[could not format log message]";
        message = fallback;
        length = sizeof(fallback) - 1;
    }

    fprintf(file, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %-5s %s | ",
            unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay), unsigned(t.wHour),
            unsigned(t.wMinute), unsigned(t.wSecond), unsigned(t.wMilliseconds),
            LogLevelName(level), component);
    const char* run = message;
    const char* end = message + length;
    for (const char* p = run; p != end; ++p) {
        unsigned char byte = static_cast<unsigned char>(*p);
        if (byte >= ' ' && byte != 0x7f && byte != '\\') continue;
        fwrite(run, 1, p - run, file);
        switch (byte) {
            case '\\': fputs("\\\\", file); break;
            case '\r': fputs("\\r", file); break;
            case '\n': fputs("\\n", file); break;
            case '\t': fputs("\\t", file); break;
            default: fprintf(file, "\\x%02X", unsigned(byte)); break;
        }
        run = p + 1;
    }
    fwrite(run, 1, end - run, file);
    fputc('\n', file);
}
