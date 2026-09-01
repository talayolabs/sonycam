#include "fake_backend.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace sonycam {

namespace {

// Uncompressed 24-bit BMP test pattern that changes every frame, so the
// simulated live view looks like a moving feed. Browsers and image viewers
// render BMP natively; the real backend produces JPEG instead.
void writeBmpFrame(std::ofstream& f, int frame) {
    const int w = 320, h = 180;
    const std::uint32_t rowBytes = w * 3;  // multiple of 4, no padding
    const std::uint32_t dataSize = rowBytes * h;
    const std::uint32_t fileSize = 54 + dataSize;
    unsigned char hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = fileSize & 0xFF; hdr[3] = (fileSize >> 8) & 0xFF;
    hdr[4] = (fileSize >> 16) & 0xFF; hdr[5] = (fileSize >> 24) & 0xFF;
    hdr[10] = 54;              // pixel data offset
    hdr[14] = 40;              // BITMAPINFOHEADER size
    hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
    hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
    hdr[26] = 1;               // planes
    hdr[28] = 24;              // bits per pixel
    hdr[34] = dataSize & 0xFF; hdr[35] = (dataSize >> 8) & 0xFF;
    hdr[36] = (dataSize >> 16) & 0xFF; hdr[37] = (dataSize >> 24) & 0xFF;
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    std::vector<unsigned char> row(rowBytes);
    const int bar = (frame * 6) % w;
    for (int y = h - 1; y >= 0; --y) {  // BMP rows are bottom-up
        for (int x = 0; x < w; ++x) {
            unsigned char r = static_cast<unsigned char>(((x + frame * 3) % w) * 255 / (w - 1));
            unsigned char g = static_cast<unsigned char>(y * 255 / (h - 1));
            unsigned char b = 96;
            int d = x - bar; if (d < 0) d = -d;
            if (d < 6) { r = g = b = 240; }
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        f.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
}

// Minimal valid 1x1 grayscale JPEG (used as the canned capture file).
const unsigned char kTinyJpeg[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
    0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08, 0x07, 0x07, 0x07, 0x09,
    0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
    0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20,
    0x24, 0x2E, 0x27, 0x20, 0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29,
    0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27, 0x39, 0x3D, 0x38, 0x32,
    0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03,
    0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D,
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
    0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
    0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
    0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4,
    0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01,
    0x00, 0x00, 0x3F, 0x00, 0xFB, 0xFF, 0xD9};

bool isNumber(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    bool dot = false;
    for (; i < s.size(); ++i) {
        if (s[i] == '.' && !dot) { dot = true; continue; }
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

}  // namespace

FakeBackend::FakeBackend() {
    props_["iso"] = {"auto", {"auto", "100", "200", "400", "800", "1600", "3200",
                              "6400", "12800", "25600", "51200", "102400"}, true};
    props_["aperture"] = {"2.8", {"1.8", "2.0", "2.8", "4.0", "5.6", "8.0",
                                  "11", "16", "22"}, true};
    props_["shutter_speed"] = {"1/125", {"bulb", "30\"", "15\"", "8\"", "4\"", "2\"",
                                         "1\"", "1/2", "1/4", "1/8", "1/15", "1/30",
                                         "1/60", "1/125", "1/250", "1/500", "1/1000",
                                         "1/2000", "1/4000", "1/8000"}, true};
    props_["exposure_program"] = {"manual", {"manual", "program_auto",
                                             "aperture_priority",
                                             "shutter_priority", "auto"}, true};
    props_["exposure_comp"] = {"0", {}, true};
    props_["white_balance"] = {"auto", {"auto", "daylight", "shadow", "cloudy",
                                        "tungsten", "fluorescent", "flash",
                                        "color_temp", "custom"}, true};
    props_["focus_mode"] = {"af_s", {"mf", "af_s", "af_c", "af_a", "dmf"}, true};
    props_["focus_area"] = {"wide", {"wide", "zone", "center", "spot_s", "spot_m",
                                     "spot_l", "expand_spot", "tracking_wide",
                                     "tracking_center"}, true};
    props_["drive_mode"] = {"single", {"single", "continuous_hi", "continuous_lo",
                                       "timer_2s", "timer_5s", "timer_10s"}, true};
    props_["battery_level"] = {"87", {}, false};
    props_["priority_key"] = {"pc_remote", {"camera", "pc_remote"}, true};
}

Result FakeBackend::connect() {
    connected_ = true;
    return Result::success();
}

Result FakeBackend::disconnect() {
    connected_ = false;
    return Result::success();
}

CameraInfo FakeBackend::info() {
    CameraInfo ci;
    ci.connected = connected_;
    ci.model = "FAKE ILCE-7CM2";
    ci.id = "fake-0001";
    ci.transport = "fake";
    return ci;
}

Result FakeBackend::gearInfo(std::vector<PropInfo>& out) {
    if (!connected_) return Result::fail("not connected");
    out.push_back(PropInfo{"model", "FAKE ILCE-7CM2", false, {}});
    out.push_back(PropInfo{"body_serial", "0000001", false, {}});
    out.push_back(PropInfo{"body_firmware", "9.99", false, {}});
    out.push_back(PropInfo{"lens", "FAKE FE 28-70mm F3.5-5.6 OSS", false, {}});
    out.push_back(PropInfo{"lens_firmware", "01", false, {}});
    out.push_back(PropInfo{"remote_zoom", "no", false, {}});
    return Result::success();
}

Result FakeBackend::listProps(std::vector<PropInfo>& out) {
    if (!connected_) return Result::fail("not connected");
    for (const auto& [name, p] : props_) {
        out.push_back(PropInfo{name, p.value, p.writable, p.choices});
    }
    return Result::success();
}

Result FakeBackend::getProp(const std::string& name, PropInfo& out) {
    if (!connected_) return Result::fail("not connected");
    auto it = props_.find(name);
    if (it == props_.end()) return Result::fail("unknown property: " + name);
    out = PropInfo{name, it->second.value, it->second.writable, it->second.choices};
    return Result::success();
}

Result FakeBackend::setProp(const std::string& name, const std::string& value) {
    if (!connected_) return Result::fail("not connected");
    auto it = props_.find(name);
    if (it == props_.end()) return Result::fail("unknown property: " + name);
    if (!it->second.writable) return Result::fail("property is read-only: " + name);
    if (!it->second.choices.empty()) {
        const auto& c = it->second.choices;
        if (std::find(c.begin(), c.end(), value) == c.end())
            return Result::fail("invalid value '" + value + "' for " + name);
    } else if (!isNumber(value)) {
        return Result::fail("invalid numeric value '" + value + "' for " + name);
    }
    it->second.value = value;
    return Result::success();
}

Result FakeBackend::record(const std::string& op, std::string& outState) {
    if (!connected_) return Result::fail("not connected");
    if (op == "start") recording_ = true;
    else if (op == "stop") recording_ = false;
    else if (op != "status") return Result::fail("unknown record op: " + op);
    outState = recording_ ? "recording" : "not_recording";
    return Result::success();
}

Result FakeBackend::focus(const std::string& op, int steps, std::string& outStatus) {
    if (!connected_) return Result::fail("not connected");
    const std::string mode = props_["focus_mode"].value;
    if (op == "status") {
        outStatus = "unlocked";
        return Result::success();
    }
    if (op == "af") {
        if (mode == "mf")
            return Result::fail("autofocus requires an AF focus mode (current: mf)");
        outStatus = "focused";
        return Result::success();
    }
    if (op == "near" || op == "far") {
        if (mode != "mf")
            return Result::fail("focus nudge requires focus_mode mf (current: " +
                                mode + ")");
        outStatus = op + " x" + std::to_string(steps);
        return Result::success();
    }
    return Result::fail("unknown focus op: " + op);
}

Result FakeBackend::capture(const std::string& saveDir, std::string& outFile) {
    if (!connected_) return Result::fail("not connected");
    if (props_["priority_key"].value != "pc_remote")
        return Result::fail("priority key is not pc_remote; camera refuses remote release");
    std::string dir = saveDir.empty() ? std::string(".") : saveDir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return Result::fail("cannot create " + dir + ": " + ec.message());
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/DSC%05d.JPG", ++captureCount_);
    outFile = dir + buf;
    std::ofstream f(outFile, std::ios::binary);
    if (!f) return Result::fail("cannot write " + outFile);
    f.write(reinterpret_cast<const char*>(kTinyJpeg), sizeof(kTinyJpeg));
    return Result::success();
}

Result FakeBackend::liveviewFrame(const std::string& path) {
    if (!connected_) return Result::fail("not connected");
    std::ofstream f(path, std::ios::binary);
    if (!f) return Result::fail("cannot write " + path);
    writeBmpFrame(f, liveviewCount_++);
    return Result::success();
}

}  // namespace sonycam
