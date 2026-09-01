#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sonycam {

struct Result {
    bool ok = true;
    std::string error;

    static Result success() { return Result{}; }
    static Result fail(std::string msg) { return Result{false, std::move(msg)}; }
};

struct PropInfo {
    std::string name;        // canonical name, e.g. "iso"
    std::string value;       // human-readable current value, e.g. "800", "f/2.8"
    bool writable = false;
    std::vector<std::string> choices;  // allowed values if enumerable (may be empty)
};

struct CameraInfo {
    bool connected = false;
    std::string model;
    std::string id;
    std::string transport;  // "usb" | "wifi" | "fake"
};

// Abstract camera backend. Implemented by FakeBackend (always available)
// and CrsdkBackend (requires the Sony Camera Remote SDK at build time).
class CameraBackend {
public:
    virtual ~CameraBackend() = default;

    virtual Result connect() = 0;
    virtual Result disconnect() = 0;
    virtual CameraInfo info() = 0;

    // Hardware identification: body/lens model, serial and firmware info.
    // Entries are name/value pairs; unsupported fields are omitted.
    virtual Result gearInfo(std::vector<PropInfo>& out) = 0;

    virtual Result listProps(std::vector<PropInfo>& out) = 0;
    virtual Result getProp(const std::string& name, PropInfo& out) = 0;
    virtual Result setProp(const std::string& name, const std::string& value) = 0;

    // Movie recording. op: "start", "stop", or "status". `outState` reports
    // the resulting recording state (recording / not_recording / ...).
    virtual Result record(const std::string& op, std::string& outState) = 0;

    // Power zoom. op: "in", "out" (drive for `ms` milliseconds, then stop)
    // or "stop". Fails when the lens has no remote zoom.
    virtual Result zoom(const std::string& op, int ms) = 0;

    // Full camera configuration as a file. op: "save" (camera -> `path`)
    // or "load" (`path` -> camera).
    virtual Result preset(const std::string& op, const std::string& path) = 0;

    // Focus control. op: "af" (half-press, wait for lock, release),
    // "near"/"far" (manual-focus nudges, `steps` times), "status" (read the
    // focus indication). `outStatus` reports the resulting focus state.
    virtual Result focus(const std::string& op, int steps,
                         std::string& outStatus) = 0;

    // Trigger the shutter. On success `outFile` may contain a downloaded
    // file path if the backend saves images locally (empty otherwise).
    virtual Result capture(const std::string& saveDir, std::string& outFile) = 0;

    // Fetch a single live-view JPEG frame and write it to `path`.
    virtual Result liveviewFrame(const std::string& path) = 0;
};

}  // namespace sonycam
