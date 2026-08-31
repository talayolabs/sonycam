#include "crsdk_backend.hpp"

#ifndef SONYCAM_WITH_CRSDK

namespace sonycam {

std::unique_ptr<CameraBackend> makeCrsdkBackend(std::string& error) {
    error = "sonycam was built without the Sony Camera Remote SDK. "
            "Rebuild with -DSONY_SDK_DIR=/path/to/CrSDK or use --fake.";
    return nullptr;
}

}  // namespace sonycam

#else  // SONYCAM_WITH_CRSDK

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "CRSDK/CameraRemote_SDK.h"
#include "CRSDK/IDeviceCallback.h"

namespace sonycam {

namespace {

using SCRSDK::CrDeviceProperty;
using SCRSDK::CrError;

#ifdef __APPLE__

// macOS eagerly launches ptpcamerad for imaging-class USB devices. It opens a
// PTP session before CrSDK can, then respawns too quickly for a one-shot kill.
// Suppress it only while connecting; the child watches this process so it
// cannot outlive an abnormal sonycamd exit.
class PtpCameraSuppressor {
public:
    PtpCameraSuppressor() {
        pid_ = ::fork();
        if (pid_ != 0) return;
        std::string parentPid = std::to_string(::getppid());
        ::execl("/bin/sh", "sonycam-ptp-suppressor", "-c",
                "while :; do "
                "kill -0 \"$1\" 2>/dev/null || exit 0; "
                "/usr/bin/killall -9 ptpcamerad PTPCamera 2>/dev/null; "
                "done",
                "sonycam-ptp-suppressor", parentPid.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ~PtpCameraSuppressor() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGTERM);
        while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    }

    PtpCameraSuppressor(const PtpCameraSuppressor&) = delete;
    PtpCameraSuppressor& operator=(const PtpCameraSuppressor&) = delete;

private:
    pid_t pid_ = -1;
};

int reenumerateSonyUsbClass(const char* className) {
    constexpr int kSonyVendor = 0x054c;
    CFMutableDictionaryRef matching = IOServiceMatching(className);
    if (!matching) return 0;

    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, matching, &iterator);
    if (kr != KERN_SUCCESS) return 0;

    int reset = 0;
    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        CFTypeRef value = IORegistryEntryCreateCFProperty(
            service, CFSTR("idVendor"), kCFAllocatorDefault, 0);
        int vendor = 0;
        if (value && CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue(static_cast<CFNumberRef>(value),
                             kCFNumberIntType, &vendor);
        if (value) CFRelease(value);
        if (vendor != kSonyVendor) {
            IOObjectRelease(service);
            continue;
        }

        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        kr = IOCreatePlugInInterfaceForService(
            service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            IOObjectRelease(service);
            continue;
        }

        IOUSBDeviceInterface942** device = nullptr;
        HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942),
            reinterpret_cast<LPVOID*>(&device));
        (*plugin)->Release(plugin);
        if (hr || !device) {
            IOObjectRelease(service);
            continue;
        }

        kr = (*device)->USBDeviceOpen(device);
        if (kr == KERN_SUCCESS || kr == kIOReturnExclusiveAccess) {
            if ((*device)->USBDeviceReEnumerate(device, 0) == KERN_SUCCESS)
                ++reset;
            (*device)->USBDeviceClose(device);
        }
        (*device)->Release(device);
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return reset;
}

int reenumerateSonyUsbDevices() {
    int reset = reenumerateSonyUsbClass("IOUSBHostDevice");
    reset += reenumerateSonyUsbClass("IOUSBDevice");
    return reset;
}

#endif  // __APPLE__

struct EnumEntry {
    const char* name;
    std::uint64_t value;
};

const EnumEntry kWhiteBalance[] = {
    {"auto", SCRSDK::CrWhiteBalance_AWB},
    {"daylight", SCRSDK::CrWhiteBalance_Daylight},
    {"shadow", SCRSDK::CrWhiteBalance_Shadow},
    {"cloudy", SCRSDK::CrWhiteBalance_Cloudy},
    {"tungsten", SCRSDK::CrWhiteBalance_Tungsten},
    {"fluorescent", SCRSDK::CrWhiteBalance_Fluorescent},
    {"flash", SCRSDK::CrWhiteBalance_Flush},
    {"color_temp", SCRSDK::CrWhiteBalance_ColorTemp},
    {"custom", SCRSDK::CrWhiteBalance_Custom},
};

const EnumEntry kFocusMode[] = {
    {"mf", SCRSDK::CrFocus_MF},
    {"af_s", SCRSDK::CrFocus_AF_S},
    {"af_c", SCRSDK::CrFocus_AF_C},
    {"af_a", SCRSDK::CrFocus_AF_A},
    {"dmf", SCRSDK::CrFocus_DMF},
};

const EnumEntry kFocusArea[] = {
    {"wide", SCRSDK::CrFocusArea_Wide},
    {"zone", SCRSDK::CrFocusArea_Zone},
    {"center", SCRSDK::CrFocusArea_Center},
    {"spot_s", SCRSDK::CrFocusArea_Flexible_Spot_S},
    {"spot_m", SCRSDK::CrFocusArea_Flexible_Spot_M},
    {"spot_l", SCRSDK::CrFocusArea_Flexible_Spot_L},
    {"expand_spot", SCRSDK::CrFocusArea_Expand_Flexible_Spot},
    {"tracking_wide", SCRSDK::CrFocusArea_Tracking_Wide},
    {"tracking_center", SCRSDK::CrFocusArea_Tracking_Center},
};

const EnumEntry kExposureProgram[] = {
    {"manual", SCRSDK::CrExposure_M_Manual},
    {"program_auto", SCRSDK::CrExposure_P_Auto},
    {"aperture_priority", SCRSDK::CrExposure_A_AperturePriority},
    {"shutter_priority", SCRSDK::CrExposure_S_ShutterSpeedPriority},
    {"auto", SCRSDK::CrExposure_Auto},
};

const EnumEntry kDriveMode[] = {
    {"single", SCRSDK::CrDrive_Single},
    {"continuous_hi", SCRSDK::CrDrive_Continuous_Hi},
    {"continuous_lo", SCRSDK::CrDrive_Continuous_Lo},
    {"timer_2s", SCRSDK::CrDrive_Timer_2s},
    {"timer_5s", SCRSDK::CrDrive_Timer_5s},
    {"timer_10s", SCRSDK::CrDrive_Timer_10s},
};

const EnumEntry kPriorityKey[] = {
    {"camera", SCRSDK::CrPriorityKey_CameraPosition},
    {"pc_remote", SCRSDK::CrPriorityKey_PCRemote},
};

std::string enumToString(const EnumEntry* table, size_t n, std::uint64_t v) {
    for (size_t i = 0; i < n; ++i)
        if (table[i].value == v) return table[i].name;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

bool enumFromString(const EnumEntry* table, size_t n, const std::string& s,
                    std::uint64_t& out) {
    for (size_t i = 0; i < n; ++i)
        if (s == table[i].name) { out = table[i].value; return true; }
    return false;
}

// --- value codecs for the numeric properties -------------------------------

std::string isoToString(std::uint64_t v) {
    std::uint32_t value = static_cast<std::uint32_t>(v) & 0xFFFFFF;
    if (value == SCRSDK::CrISO_AUTO) return "auto";
    return std::to_string(value);
}

bool isoFromString(const std::string& s, std::uint64_t& out) {
    if (s == "auto") { out = SCRSDK::CrISO_AUTO; return true; }
    try { out = std::stoul(s); } catch (...) { return false; }
    return true;
}

std::string apertureToString(std::uint64_t v) {
    std::uint16_t f = static_cast<std::uint16_t>(v);
    if (f == SCRSDK::CrFnumber_Nothing) return "-";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", f / 100.0);
    return buf;
}

bool apertureFromString(const std::string& in, std::uint64_t& out) {
    std::string s = (in.rfind("f/", 0) == 0) ? in.substr(2) : in;
    try { out = static_cast<std::uint64_t>(std::stod(s) * 100.0 + 0.5); }
    catch (...) { return false; }
    return true;
}

std::string shutterToString(std::uint64_t v) {
    std::uint32_t raw = static_cast<std::uint32_t>(v);
    if (raw == SCRSDK::CrShutterSpeed_Bulb) return "bulb";
    std::uint32_t num = raw >> 16, den = raw & 0xFFFF;
    if (den == 0) return "-";
    if (num % den == 0) return std::to_string(num / den) + "\"";
    if (num == 1) return "1/" + std::to_string(den);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f\"", static_cast<double>(num) / den);
    return buf;
}

bool shutterFromString(const std::string& s, std::uint64_t& out) {
    if (s == "bulb") { out = SCRSDK::CrShutterSpeed_Bulb; return true; }
    try {
        if (s.rfind("1/", 0) == 0) {
            std::uint32_t den = std::stoul(s.substr(2));
            out = (1u << 16) | den;
            return true;
        }
        std::string t = s;
        if (!t.empty() && t.back() == '"') t.pop_back();
        double secs = std::stod(t);
        std::uint32_t num = static_cast<std::uint32_t>(secs * 10 + 0.5);
        out = (static_cast<std::uint64_t>(num) << 16) | 10u;
        return true;
    } catch (...) {
        return false;
    }
}

std::string evToString(std::uint64_t v) {
    // stored as value * 1000, signed 16-bit
    std::int16_t milli = static_cast<std::int16_t>(v);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%+.1f", milli / 1000.0);
    return buf;
}

bool evFromString(const std::string& s, std::uint64_t& out) {
    try {
        std::int16_t milli = static_cast<std::int16_t>(std::stod(s) * 1000.0 +
                                                       (s[0] == '-' ? -0.5 : 0.5));
        out = static_cast<std::uint16_t>(milli);
        return true;
    } catch (...) {
        return false;
    }
}

// --- canonical property table ----------------------------------------------

struct PropDef {
    const char* name;
    std::uint32_t code;
    SCRSDK::CrDataType dataType;
    const EnumEntry* enums;
    size_t numEnums;
    std::string (*toStr)(std::uint64_t);
    bool (*fromStr)(const std::string&, std::uint64_t&);
};

const PropDef kProps[] = {
    {"iso", SCRSDK::CrDeviceProperty_IsoSensitivity, SCRSDK::CrDataType_UInt32,
     nullptr, 0, isoToString, isoFromString},
    {"aperture", SCRSDK::CrDeviceProperty_FNumber, SCRSDK::CrDataType_UInt16,
     nullptr, 0, apertureToString, apertureFromString},
    {"shutter_speed", SCRSDK::CrDeviceProperty_ShutterSpeed, SCRSDK::CrDataType_UInt32,
     nullptr, 0, shutterToString, shutterFromString},
    {"exposure_comp", SCRSDK::CrDeviceProperty_ExposureBiasCompensation,
     SCRSDK::CrDataType_UInt16, nullptr, 0, evToString, evFromString},
    {"exposure_program", SCRSDK::CrDeviceProperty_ExposureProgramMode,
     SCRSDK::CrDataType_UInt32, kExposureProgram, std::size(kExposureProgram),
     nullptr, nullptr},
    {"white_balance", SCRSDK::CrDeviceProperty_WhiteBalance,
     SCRSDK::CrDataType_UInt16, kWhiteBalance, std::size(kWhiteBalance),
     nullptr, nullptr},
    {"focus_mode", SCRSDK::CrDeviceProperty_FocusMode, SCRSDK::CrDataType_UInt16,
     kFocusMode, std::size(kFocusMode), nullptr, nullptr},
    {"focus_area", SCRSDK::CrDeviceProperty_FocusArea, SCRSDK::CrDataType_UInt16,
     kFocusArea, std::size(kFocusArea), nullptr, nullptr},
    {"drive_mode", SCRSDK::CrDeviceProperty_DriveMode, SCRSDK::CrDataType_UInt32,
     kDriveMode, std::size(kDriveMode), nullptr, nullptr},
    {"priority_key", SCRSDK::CrDeviceProperty_PriorityKeySettings,
     SCRSDK::CrDataType_UInt16, kPriorityKey, std::size(kPriorityKey),
     nullptr, nullptr},
};

const PropDef* findProp(const std::string& name) {
    for (const auto& p : kProps)
        if (name == p.name) return &p;
    return nullptr;
}

std::string valueToString(const PropDef& def, std::uint64_t v) {
    if (def.toStr) return def.toStr(v);
    return enumToString(def.enums, def.numEnums, v);
}

bool valueFromString(const PropDef& def, const std::string& s, std::uint64_t& out) {
    if (def.fromStr) return def.fromStr(s, out);
    if (enumFromString(def.enums, def.numEnums, s, out)) return true;
    // fall back to raw numeric (hex or decimal) for values not in our map
    try { out = std::stoull(s, nullptr, 0); return true; } catch (...) {}
    return false;
}

class Callback : public SCRSDK::IDeviceCallback {
public:
    void OnConnected(SCRSDK::DeviceConnectionVersioin) override {
        std::lock_guard<std::mutex> lk(m_);
        connected_ = true;
        cv_.notify_all();
    }
    void OnDisconnected(CrInt32u reason) override {
        std::lock_guard<std::mutex> lk(m_);
        connected_ = false;
        disconnectReason_ = reason;
        cv_.notify_all();
    }
    void OnError(CrInt32u error) override {
        std::lock_guard<std::mutex> lk(m_);
        lastError_ = error;
        cv_.notify_all();
    }

    bool waitConnected(int timeoutSec) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::seconds(timeoutSec),
                            [&] { return connected_ || lastError_ != 0; }) &&
               connected_;
    }
    void reset() {
        std::lock_guard<std::mutex> lk(m_);
        connected_ = false;
        lastError_ = 0;
        disconnectReason_ = 0;
    }
    CrInt32u lastError() {
        std::lock_guard<std::mutex> lk(m_);
        return lastError_;
    }
    CrInt32u disconnectReason() {
        std::lock_guard<std::mutex> lk(m_);
        return disconnectReason_;
    }
    bool isConnected() {
        std::lock_guard<std::mutex> lk(m_);
        return connected_;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool connected_ = false;
    CrInt32u lastError_ = 0;
    CrInt32u disconnectReason_ = 0;
};

std::string crErrorString(CrError e) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "CrError 0x%04x", static_cast<unsigned>(e));
    return buf;
}

class CrsdkBackend : public CameraBackend {
public:
    ~CrsdkBackend() override {
        disconnect();
        if (sdkInit_) SCRSDK::Release();
    }

    Result connect() override {
        if (handle_ != 0) return Result::success();
#ifdef __APPLE__
        PtpCameraSuppressor suppressor;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int resetCount = reenumerateSonyUsbDevices();
        if (resetCount > 0) {
            std::fprintf(stderr,
                         "sonycamd: re-enumerated %d Sony USB device(s)\n",
                         resetCount);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
#endif
        if (!sdkInit_) {
            if (!SCRSDK::Init()) return Result::fail("CrSDK Init() failed");
            sdkInit_ = true;
        }
        SCRSDK::ICrEnumCameraObjectInfo* enumInfo = nullptr;
        CrError err = SCRSDK::EnumCameraObjects(&enumInfo);
        if (err != SCRSDK::CrError_None || !enumInfo || enumInfo->GetCount() == 0) {
            if (enumInfo) enumInfo->Release();
            return Result::fail(
                "no camera found (is it on, in PC Remote mode, and connected via "
                "USB/Wi-Fi?)");
        }
        const SCRSDK::ICrCameraObjectInfo* found = enumInfo->GetCameraObjectInfo(0);
        camera_ = SCRSDK::CreateCameraObjectInfo(
            found->GetName(), found->GetModel(), found->GetUsbPid(),
            found->GetIdType(), found->GetIdSize(), found->GetId(),
            found->GetConnectionTypeName(), found->GetAdaptorName(),
            found->GetPairingNecessity(), found->GetSSHsupport());
        model_ = reinterpret_cast<const char*>(found->GetModel());
        enumInfo->Release();
        if (!camera_) return Result::fail("CreateCameraObjectInfo failed");

        callback_.reset();
        err = SCRSDK::Connect(camera_, &callback_, &handle_);
        if (err != SCRSDK::CrError_None) {
            handle_ = 0;
            return Result::fail("Connect failed: " + crErrorString(err));
        }
        if (!callback_.waitConnected(20)) {
            SCRSDK::Disconnect(handle_);
            SCRSDK::ReleaseDevice(handle_);
            handle_ = 0;
            if (callback_.lastError() != 0)
                return Result::fail(
                    "camera connection callback failed: " +
                    crErrorString(static_cast<CrError>(callback_.lastError())));
            if (callback_.disconnectReason() != 0)
                return Result::fail(
                    "camera disconnected while connecting: reason 0x" +
                    [&] {
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "%08x",
                                      callback_.disconnectReason());
                        return std::string(buf);
                    }());
            return Result::fail("timed out waiting for camera connection");
        }

        // On real bodies OnConnected fires before the property API is ready.
        // Wait for the priority property, then take the key and verify that
        // the camera actually applied it before reporting a usable session.
        Result ready = Result::fail("camera properties did not become ready");
        for (int i = 0; i < 50; ++i) {
            PropInfo priority;
            ready = getProp("priority_key", priority);
            if (ready.ok) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!ready.ok) {
            disconnect();
            return ready;
        }
        Result priority = setProp("priority_key", "pc_remote");
        if (!priority.ok) {
            disconnect();
            return Result::fail("cannot acquire PC Remote priority: " +
                                priority.error);
        }
        return Result::success();
    }

    Result disconnect() override {
        if (handle_ != 0) {
            SCRSDK::Disconnect(handle_);
            SCRSDK::ReleaseDevice(handle_);
            handle_ = 0;
        }
        if (camera_) {
            camera_->Release();
            camera_ = nullptr;
        }
        return Result::success();
    }

    CameraInfo info() override {
        CameraInfo ci;
        ci.connected = handle_ != 0 && callback_.isConnected();
        ci.model = model_;
        ci.transport = "usb/net";
        return ci;
    }

    Result listProps(std::vector<PropInfo>& out) override {
        if (handle_ == 0) return Result::fail("not connected");
        Result firstError = Result::success();
        for (const auto& def : kProps) {
            PropInfo pi;
            Result r = getProp(def.name, pi);
            if (r.ok) out.push_back(std::move(pi));
            else if (firstError.ok) firstError = r;
        }
        if (out.empty() && !firstError.ok) return firstError;
        return Result::success();
    }

    Result getProp(const std::string& name, PropInfo& out) override {
        if (handle_ == 0) return Result::fail("not connected");
        const PropDef* def = findProp(name);
        if (!def) return Result::fail("unknown property: " + name);

        CrDeviceProperty* props = nullptr;
        CrInt32 num = 0;
        CrInt32u code = def->code;
        CrError err = SCRSDK::GetSelectDeviceProperties(handle_, 1, &code, &props, &num);
        if (err != SCRSDK::CrError_None || num == 0 || !props) {
            if (props) SCRSDK::ReleaseDeviceProperties(handle_, props);
            return Result::fail("GetSelectDeviceProperties failed: " + crErrorString(err));
        }

        out.name = name;
        out.value = valueToString(*def, props[0].GetCurrentValue());
        out.writable = props[0].IsSetEnableCurrentValue();
        out.choices = readChoices(*def, props[0]);
        SCRSDK::ReleaseDeviceProperties(handle_, props);
        return Result::success();
    }

    Result setProp(const std::string& name, const std::string& value) override {
        if (handle_ == 0) return Result::fail("not connected");
        const PropDef* def = findProp(name);
        if (!def) return Result::fail("unknown property: " + name);
        std::uint64_t raw = 0;
        if (!valueFromString(*def, value, raw))
            return Result::fail("cannot parse value '" + value + "' for " + name);

        CrDeviceProperty* current = nullptr;
        CrInt32 num = 0;
        CrInt32u code = def->code;
        CrError err = SCRSDK::GetSelectDeviceProperties(
            handle_, 1, &code, &current, &num);
        if (err != SCRSDK::CrError_None || num == 0 || !current) {
            if (current) SCRSDK::ReleaseDeviceProperties(handle_, current);
            return Result::fail("cannot read " + name + " before writing: " +
                                crErrorString(err));
        }
        SCRSDK::CrDataType valueType = current[0].GetValueType();
        bool writable = current[0].IsSetEnableCurrentValue();
        SCRSDK::ReleaseDeviceProperties(handle_, current);
        if (!writable)
            return Result::fail(name +
                                " is not writable in the current camera mode");

        CrDeviceProperty prop;
        prop.SetCode(def->code);
        prop.SetValueType(valueType);
        prop.SetCurrentValue(raw);
        err = SCRSDK::SetDeviceProperty(handle_, &prop);
        if (err != SCRSDK::CrError_None)
            return Result::fail("SetDeviceProperty failed: " + crErrorString(err));

        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            current = nullptr;
            num = 0;
            err = SCRSDK::GetSelectDeviceProperties(
                handle_, 1, &code, &current, &num);
            if (err == SCRSDK::CrError_None && num > 0 && current) {
                bool applied = current[0].GetCurrentValue() == raw;
                SCRSDK::ReleaseDeviceProperties(handle_, current);
                if (applied) return Result::success();
            } else if (current) {
                SCRSDK::ReleaseDeviceProperties(handle_, current);
            }
        }
        return Result::fail("camera did not apply " + name + "=" + value);
    }

    Result capture(const std::string& saveDir, std::string& outFile) override {
        if (handle_ == 0) return Result::fail("not connected");
        if (!saveDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(saveDir, ec);
            if (ec) return Result::fail("cannot create " + saveDir + ": " + ec.message());
            SCRSDK::SetSaveInfo(handle_,
                                const_cast<CrChar*>(saveDir.c_str()),
                                const_cast<CrChar*>(""), -1);
        }
        CrError err = SCRSDK::SendCommand(handle_, SCRSDK::CrCommandId_Release,
                                          SCRSDK::CrCommandParam_Down);
        if (err != SCRSDK::CrError_None)
            return Result::fail("shutter down failed: " + crErrorString(err));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SCRSDK::SendCommand(handle_, SCRSDK::CrCommandId_Release,
                            SCRSDK::CrCommandParam_Up);
        outFile.clear();  // download arrives asynchronously via OnCompleteDownload
        return Result::success();
    }

    Result liveviewFrame(const std::string& path) override {
        if (handle_ == 0) return Result::fail("not connected");
        SCRSDK::CrLiveViewProperty* props = nullptr;
        CrInt32 num = 0;
        CrError err = SCRSDK::GetLiveViewProperties(handle_, &props, &num);
        if (err != SCRSDK::CrError_None)
            return Result::fail("GetLiveViewProperties failed: " +
                                crErrorString(err));
        if (props) SCRSDK::ReleaseLiveViewProperties(handle_, props);
        SCRSDK::CrImageInfo info;
        err = SCRSDK::GetLiveViewImageInfo(handle_, &info);
        if (err != SCRSDK::CrError_None)
            return Result::fail("GetLiveViewImageInfo failed: " + crErrorString(err));
        if (info.GetBufferSize() == 0)
            return Result::fail("live view not available (buffer size 0)");
        std::vector<CrInt8u> buf(info.GetBufferSize());
        SCRSDK::CrImageDataBlock block;
        block.SetSize(static_cast<CrInt32u>(buf.size()));
        block.SetData(buf.data());
        err = SCRSDK::GetLiveViewImage(handle_, &block);
        if (err != SCRSDK::CrError_None)
            return Result::fail("GetLiveViewImage failed: " + crErrorString(err));
        std::ofstream f(path, std::ios::binary);
        if (!f) return Result::fail("cannot write " + path);
        f.write(reinterpret_cast<const char*>(block.GetImageData()),
                block.GetImageSize());
        return Result::success();
    }

private:
    std::vector<std::string> readChoices(const PropDef& def,
                                         CrDeviceProperty& prop) {
        std::vector<std::string> out;
        CrInt8u* values = prop.GetValues();
        CrInt32u byteSize = prop.GetValueSize();
        if (!values || byteSize == 0) return out;
        size_t stride = 0;
        switch (prop.GetValueType() & 0x0FFF) {
            case SCRSDK::CrDataType_UInt8: stride = 1; break;
            case SCRSDK::CrDataType_UInt16: stride = 2; break;
            case SCRSDK::CrDataType_UInt32: stride = 4; break;
            case SCRSDK::CrDataType_UInt64: stride = 8; break;
            default: return out;
        }
        for (size_t off = 0; off + stride <= byteSize; off += stride) {
            std::uint64_t v = 0;
            std::memcpy(&v, values + off, stride);
            out.push_back(valueToString(def, v));
        }
        return out;
    }

    bool sdkInit_ = false;
    SCRSDK::ICrCameraObjectInfo* camera_ = nullptr;
    SCRSDK::CrDeviceHandle handle_ = 0;
    Callback callback_;
    std::string model_;
};

}  // namespace

std::unique_ptr<CameraBackend> makeCrsdkBackend(std::string& error) {
    (void)error;
    return std::make_unique<CrsdkBackend>();
}

}  // namespace sonycam

#endif  // SONYCAM_WITH_CRSDK
