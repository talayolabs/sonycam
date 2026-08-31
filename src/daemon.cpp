// sonycamd: holds the camera connection and serves JSON-lines requests
// over a unix domain socket. One request per line, one response per line.
//
// Request:  {"cmd":"get","prop":"iso"}
// Response: {"ok":true,"result":{...}} | {"ok":false,"error":"..."}

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "backend.hpp"
#include "crsdk_backend.hpp"
#include "fake_backend.hpp"
#include "protocol.hpp"

using json = nlohmann::json;
using namespace sonycam;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

json propToJson(const PropInfo& p) {
    json j{{"name", p.name}, {"value", p.value}, {"writable", p.writable}};
    if (!p.choices.empty()) j["choices"] = p.choices;
    return j;
}

json handle(CameraBackend& cam, const json& req, bool& shutdown) {
    const std::string cmd = req.value("cmd", "");
    json resp{{"ok", true}};
    Result r;

    if (cmd == "ping") {
        resp["result"] = "pong";
    } else if (cmd == "shutdown") {
        shutdown = true;
        resp["result"] = "bye";
    } else if (cmd == "connect") {
        r = cam.connect();
    } else if (cmd == "disconnect") {
        r = cam.disconnect();
    } else if (cmd == "status") {
        CameraInfo ci = cam.info();
        resp["result"] = {{"connected", ci.connected},
                          {"model", ci.model},
                          {"transport", ci.transport}};
    } else if (cmd == "info") {
        std::vector<PropInfo> fields;
        r = cam.gearInfo(fields);
        if (r.ok) {
            json arr = json::array();
            for (const auto& p : fields) arr.push_back(propToJson(p));
            resp["result"] = arr;
        }
    } else if (cmd == "props") {
        std::vector<PropInfo> props;
        r = cam.listProps(props);
        if (r.ok) {
            json arr = json::array();
            for (const auto& p : props) arr.push_back(propToJson(p));
            resp["result"] = arr;
        }
    } else if (cmd == "get") {
        PropInfo p;
        r = cam.getProp(req.value("prop", ""), p);
        if (r.ok) resp["result"] = propToJson(p);
    } else if (cmd == "set") {
        r = cam.setProp(req.value("prop", ""), req.value("value", ""));
        if (r.ok) {
            PropInfo p;
            if (cam.getProp(req.value("prop", ""), p).ok)
                resp["result"] = propToJson(p);
        }
    } else if (cmd == "capture") {
        std::string outFile;
        r = cam.capture(req.value("dir", ""), outFile);
        if (r.ok) resp["result"] = {{"file", outFile}};
    } else if (cmd == "liveview") {
        const std::string path = req.value("path", "");
        if (path.empty()) return {{"ok", false}, {"error", "missing 'path'"}};
        r = cam.liveviewFrame(path);
        if (r.ok) resp["result"] = {{"file", path}};
    } else {
        return {{"ok", false}, {"error", "unknown command: " + cmd}};
    }

    if (!r.ok) return {{"ok", false}, {"error", r.error}};
    return resp;
}

void serveClient(int fd, CameraBackend& cam, bool& shutdown) {
    std::string buf;
    char chunk[4096];
    for (;;) {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            json resp;
            try {
                resp = handle(cam, json::parse(line), shutdown);
            } catch (const std::exception& e) {
                resp = {{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
            }
            std::string out = resp.dump() + "\n";
            if (::write(fd, out.data(), out.size()) < 0) return;
            if (shutdown) return;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool fake = false;
    std::string socketPath = defaultSocketPath();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fake") fake = true;
        else if (a == "--socket" && i + 1 < argc) socketPath = argv[++i];
        else {
            std::fprintf(stderr, "usage: sonycamd [--fake] [--socket PATH]\n");
            return 2;
        }
    }
    if (std::getenv("SONYCAM_FAKE")) fake = true;

    std::unique_ptr<CameraBackend> cam;
    if (fake) {
        cam = std::make_unique<FakeBackend>();
    } else {
        std::string err;
        cam = makeCrsdkBackend(err);
        if (!cam) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
    }

    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);
    ::signal(SIGPIPE, SIG_IGN);

    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { std::perror("socket"); return 1; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(socketPath.c_str());
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }
    ::chmod(socketPath.c_str(), 0600);
    if (::listen(srv, 4) < 0) { std::perror("listen"); return 1; }
    std::fprintf(stderr, "sonycamd: listening on %s (%s backend)\n",
                 socketPath.c_str(), fake ? "fake" : "crsdk");

    bool shutdown = false;
    while (!g_stop && !shutdown) {
        int fd = ::accept(srv, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        serveClient(fd, *cam, shutdown);
        ::close(fd);
    }

    cam->disconnect();
    ::close(srv);
    ::unlink(socketPath.c_str());
    return 0;
}
