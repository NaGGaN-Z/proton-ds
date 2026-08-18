#pragma once
// proton-ds gadget — leveled logger: stderr + optional file sink.
// LOG_LEVEL env: debug|info|warn|error (default: debug).

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <string>

namespace pds::log {

enum class Level : std::uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline const char* level_name(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "?";
}

inline Level threshold() {
    static const Level th = [] {
        const char* e = ::getenv("LOG_LEVEL");
        if (!e || !*e) return Level::Debug;
        if (std::string(e) == "info")  return Level::Info;
        if (std::string(e) == "warn")  return Level::Warn;
        if (std::string(e) == "error") return Level::Error;
        return Level::Debug;
    }();
    return th;
}

inline std::mutex& sink_mutex() { static std::mutex m; return m; }
inline std::FILE*& file_sink()  { static std::FILE* f = nullptr; return f; }

inline void open_file_sink(const std::string& path) {
    std::lock_guard<std::mutex> lk(sink_mutex());
    if (file_sink()) ::fclose(file_sink());
    file_sink() = ::fopen(path.c_str(), "a");
}

inline bool should_log(Level l) { return static_cast<int>(l) >= static_cast<int>(threshold()); }

inline void write(Level l, const char* tag, const char* fmt, ...) {
    if (!should_log(l)) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    ::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    std:: timespec ts{};
    ::timespec_get(&ts, TIME_UTC);
    std::tm tm{};
    ::gmtime_r(&ts.tv_sec, &tm);
    char head[64];
    ::snprintf(head, sizeof(head), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ %s",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
               level_name(l));

    std::lock_guard<std::mutex> lk(sink_mutex());
    std::fprintf(stderr, "%s [%s] %s\n", head, tag, msg);
    if (file_sink()) {
        std::fprintf(file_sink(), "%s [%s] %s\n", head, tag, msg);
        std::fflush(file_sink());
    }
}

} // namespace pds::log

#define LOGD(tag, ...) ::pds::log::write(::pds::log::Level::Debug, tag, __VA_ARGS__)
#define LOGI(tag, ...) ::pds::log::write(::pds::log::Level::Info,  tag, __VA_ARGS__)
#define LOGW(tag, ...) ::pds::log::write(::pds::log::Level::Warn,  tag, __VA_ARGS__)
#define LOGE(tag, ...) ::pds::log::write(::pds::log::Level::Error, tag, __VA_ARGS__)
