#include "Log.h"
#include <chrono>
#include <cstdio>

static double NowSeconds() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

Log& Log::Get() {
    static Log instance;
    return instance;
}

void Log::Write(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteV(level, fmt, args);
    va_end(args);
}

void Log::WriteV(LogLevel level, const char* fmt, va_list args) {
    char stackBuf[1024];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, copy);
    va_end(copy);
    std::string text;
    if (n < 0) {
        text = fmt;
    } else if (n < (int)sizeof(stackBuf)) {
        text.assign(stackBuf, n);
    } else {
        text.resize(n + 1);
        vsnprintf(text.data(), text.size(), fmt, args);
        text.resize(n);
    }

    static const char* kPrefix[] = { "[DEBUG]", "[INFO ]", "[WARN ]", "[ERROR]" };
    std::lock_guard<std::mutex> lock(m_mutex);
    LogEntry e{ level, NowSeconds(), std::move(text) };
    if (m_echoStdout) {
        FILE* out = level >= LogLevel::Warning ? stderr : stdout;
        fprintf(out, "%s %s\n", kPrefix[(int)level], e.text.c_str());
    }
    if (m_file) {
        fprintf(m_file, "%9.3f %s %s\n", e.time, kPrefix[(int)level], e.text.c_str());
        fflush(m_file);
    }
    m_entries.push_back(std::move(e));
    if (m_entries.size() > 20000)
        m_entries.erase(m_entries.begin(), m_entries.begin() + 5000);
    ++m_revision;
}

void Log::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    ++m_revision;
}

void Log::SetFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file) fclose(m_file);
    m_file = fopen(path.c_str(), "w");
    m_filePath = path;
}

int Log::CountOf(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    int c = 0;
    for (auto& e : m_entries) if (e.level == level) ++c;
    return c;
}
