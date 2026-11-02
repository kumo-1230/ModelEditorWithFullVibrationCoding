#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdarg>

enum class LogLevel { Debug, Info, Warning, Error };

struct LogEntry {
    LogLevel level;
    double time;        // アプリ起動からの秒数
    std::string text;
};

// スレッドセーフなロガー。コンソールパネルとファイルに出力する。
class Log {
public:
    static Log& Get();

    void Write(LogLevel level, const char* fmt, ...);
    void WriteV(LogLevel level, const char* fmt, va_list args);

    void Clear();
    void SetFile(const std::string& path);
    void SetEchoStdout(bool echo) { m_echoStdout = echo; }

    // ロックして entries を参照する
    template<class F> void Visit(F&& f) {
        std::lock_guard<std::mutex> lock(m_mutex);
        f(m_entries);
    }
    int CountOf(LogLevel level);
    size_t Revision() const { return m_revision; }

private:
    std::mutex m_mutex;
    std::vector<LogEntry> m_entries;
    std::string m_filePath;
    FILE* m_file = nullptr;
    bool m_echoStdout = true;
    size_t m_revision = 0;
};

#define LOG_DEBUG(...) Log::Get().Write(LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  Log::Get().Write(LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...)  Log::Get().Write(LogLevel::Warning, __VA_ARGS__)
#define LOG_ERROR(...) Log::Get().Write(LogLevel::Error, __VA_ARGS__)
