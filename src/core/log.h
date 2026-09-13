#pragma once

#include <format>
#include <string_view>
#include <utility>

// Minimal logging. Everything goes to stderr with a level prefix.
// fatal() logs and aborts: used for unrecoverable setup errors and, in
// Debug builds, for any error-severity KHR_debug message from the driver.
namespace dvk::log {

enum class Level { Info, Warn, Error };

void write(Level level, std::string_view msg);
[[noreturn]] void fatalMsg(std::string_view msg);

template <class... Args>
void info(std::format_string<Args...> f, Args&&... a) {
    write(Level::Info, std::format(f, std::forward<Args>(a)...));
}
template <class... Args>
void warn(std::format_string<Args...> f, Args&&... a) {
    write(Level::Warn, std::format(f, std::forward<Args>(a)...));
}
template <class... Args>
void error(std::format_string<Args...> f, Args&&... a) {
    write(Level::Error, std::format(f, std::forward<Args>(a)...));
}
template <class... Args>
[[noreturn]] void fatal(std::format_string<Args...> f, Args&&... a) {
    fatalMsg(std::format(f, std::forward<Args>(a)...));
}

} // namespace dvk::log
