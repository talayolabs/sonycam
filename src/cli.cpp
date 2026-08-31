// sonycam: thin client for sonycamd. Auto-starts the daemon when needed.
//
// Exit codes: 0 = ok, 1 = camera/daemon error, 2 = usage, 3 = daemon unreachable.

#include <fcntl.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "protocol.hpp"

using json = nlohmann::json;
using namespace sonycam;

namespace {

const char kUsage[] =
    "sonycam - control a Sony camera (Camera Remote SDK)\n"
    "\n"
    "usage: sonycam [--json] [--fake] [--socket PATH] <command> [args]\n"
    "\n"
    "commands:\n"
    "  status                     connection state and camera model\n"
    "  info                       identify gear: body/lens model, serials, firmware\n"
    "  props                      list all supported properties with values\n"
    "  get <prop>                 read one property (e.g. iso, aperture)\n"
    "  set <prop> <value>         write one property (e.g. set iso 800)\n"
    "  focus af                   autofocus (half-press), waits for lock\n"
    "  focus near|far [N]         manual-focus nudge N steps (needs focus_mode mf)\n"
    "  focus status               current focus indication\n"
    "  capture [--dir DIR]        trigger the shutter\n"
    "  liveview <out.jpg>         save one live-view frame\n"
    "  connect | disconnect       manage the camera connection\n"
    "  daemon stop                stop the background daemon\n"
    "\n"
    "properties: iso aperture shutter_speed exposure_comp exposure_program\n"
    "            white_balance focus_mode focus_area drive_mode priority_key\n"
    "\n"
    "examples:\n"
    "  sonycam set aperture 2.8      sonycam set shutter_speed 1/250\n"
    "  sonycam set iso auto          sonycam --json props\n"
    "\n"
    "flags:\n"
    "  --json     machine-readable output\n"
    "  --fake     use the built-in simulated camera (no hardware needed)\n"
    "\n"
    "The first command auto-starts the sonycamd daemon, which keeps the\n"
    "camera connection open between invocations.\n";

int connectSocket(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string daemonBinary(const char* argv0) {
    if (!std::strchr(argv0, '/')) return "sonycamd";  // found via PATH
    std::vector<char> buf(argv0, argv0 + std::strlen(argv0) + 1);
    std::string dir = ::dirname(buf.data());
    return dir + "/sonycamd";
}

std::string daemonLogPath(const std::string& socketPath) {
    return socketPath + ".log";
}

int spawnDaemon(const std::string& bin, const std::string& socketPath, bool fake) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::setsid();
        // Detach from the client's stdio: inherited pipes would never see
        // EOF while the daemon lives. Keep stderr in a log for debugging.
        int devnull = ::open("/dev/null", O_RDWR);
        int log = ::open(daemonLogPath(socketPath).c_str(),
                         O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
        }
        if (log >= 0 || devnull >= 0)
            ::dup2(log >= 0 ? log : devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) ::close(devnull);
        if (log > STDERR_FILENO) ::close(log);
        std::vector<const char*> args{bin.c_str(), "--socket", socketPath.c_str()};
        if (fake) args.push_back("--fake");
        args.push_back(nullptr);
        ::execvp(bin.c_str(), const_cast<char* const*>(args.data()));
        std::perror("exec sonycamd");
        _exit(127);
    }
    return 0;
}

bool sendRequest(int fd, const json& req, json& resp) {
    std::string out = req.dump() + "\n";
    if (::write(fd, out.data(), out.size()) < 0) return false;
    std::string buf;
    char chunk[4096];
    for (;;) {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) return false;
        buf.append(chunk, static_cast<size_t>(n));
        size_t pos = buf.find('\n');
        if (pos != std::string::npos) {
            try {
                resp = json::parse(buf.substr(0, pos));
            } catch (...) {
                return false;
            }
            return true;
        }
    }
}

void printProp(const json& p) {
    std::printf("%-18s %s%s\n", p.value("name", "?").c_str(),
                p.value("value", "").c_str(),
                p.value("writable", false) ? "" : "  (read-only)");
}

int printResult(const std::string& cmd, const json& resp, bool jsonOut) {
    if (jsonOut) {
        std::printf("%s\n", resp.dump().c_str());
        return resp.value("ok", false) ? 0 : 1;
    }
    if (!resp.value("ok", false)) {
        std::fprintf(stderr, "error: %s\n", resp.value("error", "unknown").c_str());
        return 1;
    }
    const json result = resp.contains("result") ? resp["result"] : json();
    if (cmd == "props") {
        for (const auto& p : result) printProp(p);
    } else if (cmd == "info") {
        for (const auto& p : result)
            std::printf("%-18s %s\n", p.value("name", "?").c_str(),
                        p.value("value", "").c_str());
    } else if (cmd == "get" || cmd == "set") {
        printProp(result);
        if (result.contains("choices")) {
            std::string s;
            for (const auto& c : result["choices"])
                s += (s.empty() ? "" : " ") + c.get<std::string>();
            std::printf("%-18s %s\n", "  choices:", s.c_str());
        }
    } else if (cmd == "status") {
        std::printf("connected: %s\nmodel:     %s\ntransport: %s\n",
                    result.value("connected", false) ? "yes" : "no",
                    result.value("model", "-").c_str(),
                    result.value("transport", "-").c_str());
    } else if (cmd == "focus") {
        std::printf("focus: %s\n", result.value("status", "").c_str());
    } else if (cmd == "capture") {
        std::string f = result.value("file", "");
        std::printf("captured%s%s\n", f.empty() ? "" : ": ", f.c_str());
    } else if (cmd == "liveview") {
        std::printf("saved %s\n", result.value("file", "").c_str());
    } else {
        std::printf("ok\n");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool jsonOut = false, fake = false;
    std::string socketPath = defaultSocketPath();
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") jsonOut = true;
        else if (a == "--fake") fake = true;
        else if (a == "--socket" && i + 1 < argc) socketPath = argv[++i];
        else if (a == "-h" || a == "--help") { std::fputs(kUsage, stdout); return 0; }
        else args.push_back(a);
    }
    if (std::getenv("SONYCAM_FAKE")) fake = true;
    if (args.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    const std::string cmd = args[0];
    json req;
    if (cmd == "status" || cmd == "info" || cmd == "props" ||
        cmd == "connect" || cmd == "disconnect") {
        req = {{"cmd", cmd}};
    } else if (cmd == "get") {
        if (args.size() != 2) { std::fprintf(stderr, "usage: sonycam get <prop>\n"); return 2; }
        req = {{"cmd", "get"}, {"prop", args[1]}};
    } else if (cmd == "set") {
        if (args.size() != 3) { std::fprintf(stderr, "usage: sonycam set <prop> <value>\n"); return 2; }
        req = {{"cmd", "set"}, {"prop", args[1]}, {"value", args[2]}};
    } else if (cmd == "focus") {
        if (args.size() < 2 || args.size() > 3) {
            std::fprintf(stderr, "usage: sonycam focus af|near|far|status [steps]\n");
            return 2;
        }
        int steps = 1;
        if (args.size() == 3) {
            try { steps = std::stoi(args[2]); } catch (...) { steps = 0; }
            if (steps < 1 || steps > 100) {
                std::fprintf(stderr, "steps must be 1-100\n");
                return 2;
            }
        }
        req = {{"cmd", "focus"}, {"op", args[1]}, {"steps", steps}};
    } else if (cmd == "capture") {
        req = {{"cmd", "capture"}};
        for (size_t i = 1; i + 1 < args.size(); ++i)
            if (args[i] == "--dir") req["dir"] = args[i + 1];
    } else if (cmd == "liveview") {
        if (args.size() != 2) { std::fprintf(stderr, "usage: sonycam liveview <out.jpg>\n"); return 2; }
        req = {{"cmd", "liveview"}, {"path", args[1]}};
    } else if (cmd == "daemon") {
        if (args.size() == 2 && args[1] == "stop") {
            req = {{"cmd", "shutdown"}};
        } else {
            std::fprintf(stderr, "usage: sonycam daemon stop\n");
            return 2;
        }
    } else {
        std::fprintf(stderr, "unknown command: %s\n\n%s", cmd.c_str(), kUsage);
        return 2;
    }

    int fd = connectSocket(socketPath);
    if (fd < 0 && cmd == "daemon") {
        // nothing to stop
        if (jsonOut) std::printf("{\"ok\":true,\"result\":\"not running\"}\n");
        else std::printf("daemon not running\n");
        return 0;
    }
    if (fd < 0) {
        if (spawnDaemon(daemonBinary(argv[0]), socketPath, fake) < 0) {
            std::fprintf(stderr, "error: cannot start sonycamd\n");
            return 3;
        }
        for (int i = 0; i < 50 && fd < 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            fd = connectSocket(socketPath);
        }
        if (fd < 0) {
            std::fprintf(stderr,
                         "error: daemon did not start (socket %s); see %s\n",
                         socketPath.c_str(), daemonLogPath(socketPath).c_str());
            return 3;
        }
        // Freshly started daemon: establish the camera connection first.
        json connResp;
        if (!sendRequest(fd, json{{"cmd", "connect"}}, connResp) ||
            !connResp.value("ok", false)) {
            std::string err = connResp.is_object()
                                  ? connResp.value("error", "connect failed")
                                  : "connect failed";
            if (cmd != "status" && cmd != "connect") {
                if (jsonOut)
                    std::printf("%s\n", json{{"ok", false}, {"error", err}}.dump().c_str());
                else
                    std::fprintf(stderr, "error: %s\n", err.c_str());
                ::close(fd);
                return 1;
            }
        }
    }

    json resp;
    if (!sendRequest(fd, req, resp)) {
        std::fprintf(stderr, "error: lost connection to daemon\n");
        ::close(fd);
        return 3;
    }
    ::close(fd);
    return printResult(cmd, resp, jsonOut);
}
