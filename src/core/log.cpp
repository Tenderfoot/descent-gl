#include "core/log.h"

#include <cstdio>
#include <cstdlib>

namespace dvk::log {

void write(Level level, std::string_view msg) {
    const char* tag = "INFO ";
    if (level == Level::Warn) tag = "WARN ";
    if (level == Level::Error) tag = "ERROR";
    std::fprintf(stderr, "[%s] %.*s\n", tag, static_cast<int>(msg.size()), msg.data());
    std::fflush(stderr);
}

void fatalMsg(std::string_view msg) {
    write(Level::Error, msg);
    std::fprintf(stderr, "[FATAL] aborting\n");
    std::fflush(stderr);
    std::abort();
}

} // namespace dvk::log
