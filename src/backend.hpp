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

    // Trigger the shutter. On success `outFile` may contain a downloaded
    // file path if the backend saves images locally (empty otherwise).
    virtual Result capture(const std::string& saveDir, std::string& outFile) = 0;

    // Fetch a single live-view JPEG frame and write it to `path`.
    virtual Result liveviewFrame(const std::string& path) = 0;
};

}  // namespace sonycam
