#pragma once

#include <cstdlib>
#include <string>

namespace sonycam {

// Default unix socket path for the daemon <-> CLI protocol (JSON lines).
inline std::string defaultSocketPath() {
    const char* env = std::getenv("SONYCAM_SOCKET");
    if (env && *env) return env;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.sonycam.sock";
}

}  // namespace sonycam
