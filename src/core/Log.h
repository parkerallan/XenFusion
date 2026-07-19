#pragma once

#include <string>
#include <vector>

enum class LogLevel
{
    Info,
    Warning,
    Error,
    Build,
    Script, // output from a script's log() call — shown as [LOG]
};

struct LogEntry
{
    LogLevel    level;
    std::string text;
};

// Global typed log. Any code can emit entries (applog::Error, applog::Build,
// ...) without threading state around; the Log panel reads Entries().
namespace applog
{
    void Add(LogLevel level, const std::string& text);

    inline void Info  (const std::string& s) { Add(LogLevel::Info,    s); }
    inline void Warn  (const std::string& s) { Add(LogLevel::Warning, s); }
    inline void Error (const std::string& s) { Add(LogLevel::Error,   s); }
    inline void Build (const std::string& s) { Add(LogLevel::Build,   s); }
    inline void Script(const std::string& s) { Add(LogLevel::Script,  s); }

    const std::vector<LogEntry>& Entries();
    void Clear();
}
