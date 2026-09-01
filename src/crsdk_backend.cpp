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

#include <cctype>
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
    {"underwater_auto", SCRSDK::CrWhiteBalance_Underwater_Auto},
    {"daylight", SCRSDK::CrWhiteBalance_Daylight},
    {"shadow", SCRSDK::CrWhiteBalance_Shadow},
    {"cloudy", SCRSDK::CrWhiteBalance_Cloudy},
    {"tungsten", SCRSDK::CrWhiteBalance_Tungsten},
    {"fluorescent", SCRSDK::CrWhiteBalance_Fluorescent},
    {"fluorescent_warm_white", SCRSDK::CrWhiteBalance_Fluorescent_WarmWhite},
    {"fluorescent_cool_white", SCRSDK::CrWhiteBalance_Fluorescent_CoolWhite},
    {"fluorescent_day_white", SCRSDK::CrWhiteBalance_Fluorescent_DayWhite},
    {"fluorescent_daylight", SCRSDK::CrWhiteBalance_Fluorescent_Daylight},
    {"flash", SCRSDK::CrWhiteBalance_Flush},
    {"color_temp", SCRSDK::CrWhiteBalance_ColorTemp},
    {"custom_1", SCRSDK::CrWhiteBalance_Custom_1},
    {"custom_2", SCRSDK::CrWhiteBalance_Custom_2},
    {"custom_3", SCRSDK::CrWhiteBalance_Custom_3},
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
    {"portrait", SCRSDK::CrExposure_Portrait},
    {"auto", SCRSDK::CrExposure_Auto},
    {"auto_plus", SCRSDK::CrExposure_Auto_Plus},
    {"sports_action", SCRSDK::CrExposure_Sports_Action},
    {"sunset", SCRSDK::CrExposure_Sunset},
    {"night", SCRSDK::CrExposure_Night},
    {"landscape", SCRSDK::CrExposure_Landscape},
    {"macro", SCRSDK::CrExposure_Macro},
    {"handheld_twilight", SCRSDK::CrExposure_HandheldTwilight},
    {"night_portrait", SCRSDK::CrExposure_NightPortrait},
    {"anti_motion_blur", SCRSDK::CrExposure_AntiMotionBlur},
    {"movie_p", SCRSDK::CrExposure_Movie_P},
    {"movie_a", SCRSDK::CrExposure_Movie_A},
    {"movie_s", SCRSDK::CrExposure_Movie_S},
    {"movie_m", SCRSDK::CrExposure_Movie_M},
    {"movie_auto", SCRSDK::CrExposure_Movie_Auto},
    {"movie_f", SCRSDK::CrExposure_Movie_F},
    {"sq_p", SCRSDK::CrExposure_Movie_SQMotion_P},
    {"sq_a", SCRSDK::CrExposure_Movie_SQMotion_A},
    {"sq_s", SCRSDK::CrExposure_Movie_SQMotion_S},
    {"sq_m", SCRSDK::CrExposure_Movie_SQMotion_M},
    {"sq_auto", SCRSDK::CrExposure_Movie_SQMotion_AUTO},
    {"sq_f", SCRSDK::CrExposure_Movie_SQMotion_F},
    {"interval_f", SCRSDK::CrExposure_Movie_IntervalRec_F},
    {"interval_p", SCRSDK::CrExposure_Movie_IntervalRec_P},
    {"interval_a", SCRSDK::CrExposure_Movie_IntervalRec_A},
    {"interval_s", SCRSDK::CrExposure_Movie_IntervalRec_S},
    {"interval_m", SCRSDK::CrExposure_Movie_IntervalRec_M},
    {"interval_auto", SCRSDK::CrExposure_Movie_IntervalRec_AUTO},
};

const EnumEntry kDriveMode[] = {
    {"single", SCRSDK::CrDrive_Single},
    {"continuous_hi_plus", SCRSDK::CrDrive_Continuous_Hi_Plus},
    {"continuous_hi", SCRSDK::CrDrive_Continuous_Hi},
    {"continuous_mid", SCRSDK::CrDrive_Continuous_Mid},
    {"continuous_lo", SCRSDK::CrDrive_Continuous_Lo},
    {"continuous", SCRSDK::CrDrive_Continuous},
    {"focus_bracket", SCRSDK::CrDrive_FocusBracket},
    {"timelapse", SCRSDK::CrDrive_Timelapse},
    {"timer_2s", SCRSDK::CrDrive_Timer_2s},
    {"timer_5s", SCRSDK::CrDrive_Timer_5s},
    {"timer_10s", SCRSDK::CrDrive_Timer_10s},
};

const EnumEntry kPriorityKey[] = {
    {"camera", SCRSDK::CrPriorityKey_CameraPosition},
    {"pc_remote", SCRSDK::CrPriorityKey_PCRemote},
};

const EnumEntry kFileFormat[] = {
    {"jpeg", SCRSDK::CrFileType_Jpeg},
    {"raw", SCRSDK::CrFileType_Raw},
    {"raw+jpeg", SCRSDK::CrFileType_RawJpeg},
    {"raw+heif", SCRSDK::CrFileType_RawHeif},
    {"heif", SCRSDK::CrFileType_Heif},
};

const EnumEntry kImageQuality[] = {
    {"light", SCRSDK::CrImageQuality_Light},
    {"standard", SCRSDK::CrImageQuality_Standard},
    {"fine", SCRSDK::CrImageQuality_Fine},
    {"extra_fine", SCRSDK::CrImageQuality_ExFine},
};

const EnumEntry kMovieFormat[] = {
    {"avchd", SCRSDK::CrFileFormatMovie_AVCHD},
    {"mp4", SCRSDK::CrFileFormatMovie_MP4},
    {"xavc_s_4k", SCRSDK::CrFileFormatMovie_XAVC_S_4K},
    {"xavc_s_hd", SCRSDK::CrFileFormatMovie_XAVC_S_HD},
    {"xavc_hs_8k", SCRSDK::CrFileFormatMovie_XAVC_HS_8K},
    {"xavc_hs_4k", SCRSDK::CrFileFormatMovie_XAVC_HS_4K},
    {"xavc_s_l_4k", SCRSDK::CrFileFormatMovie_XAVC_S_L_4K},
    {"xavc_s_l_hd", SCRSDK::CrFileFormatMovie_XAVC_S_L_HD},
    {"xavc_s_i_4k", SCRSDK::CrFileFormatMovie_XAVC_S_I_4K},
    {"xavc_s_i_hd", SCRSDK::CrFileFormatMovie_XAVC_S_I_HD},
    {"xavc_i", SCRSDK::CrFileFormatMovie_XAVC_I},
    {"xavc_l", SCRSDK::CrFileFormatMovie_XAVC_L},
    {"xavc_hs_hd", SCRSDK::CrFileFormatMovie_XAVC_HS_HD},
    {"xavc_s_i_dci_4k", SCRSDK::CrFileFormatMovie_XAVC_S_I_DCI_4K},
    {"xavc_h_i_hq", SCRSDK::CrFileFormatMovie_XAVC_H_I_HQ},
    {"xavc_h_i_sq", SCRSDK::CrFileFormatMovie_XAVC_H_I_SQ},
    {"xavc_h_l", SCRSDK::CrFileFormatMovie_XAVC_H_L},
    {"x_ocn_xt", SCRSDK::CrFileFormatMovie_X_OCN_XT},
    {"x_ocn_st", SCRSDK::CrFileFormatMovie_X_OCN_ST},
    {"x_ocn_lt", SCRSDK::CrFileFormatMovie_X_OCN_LT},
    {"xavc_proxy", SCRSDK::CrFileFormatMovie_XAVC_Proxy},
    {"xavc_hs_l_422", SCRSDK::CrFileFormatMovie_XAVC_HS_L_422},
    {"xavc_hs_l_420", SCRSDK::CrFileFormatMovie_XAVC_HS_L_420},
    {"xavc_s_l_422", SCRSDK::CrFileFormatMovie_XAVC_S_L_422},
    {"xavc_s_l_420", SCRSDK::CrFileFormatMovie_XAVC_S_L_420},
    {"xavc_s_i_422", SCRSDK::CrFileFormatMovie_XAVC_S_I_422},
    {"mpeg_hd_422", SCRSDK::CrFileFormatMovie_MPEG_HD_422},
};

const EnumEntry kMovieFps[] = {
    {"120p", SCRSDK::CrRecordingFrameRateSettingMovie_120p},
    {"100p", SCRSDK::CrRecordingFrameRateSettingMovie_100p},
    {"60p", SCRSDK::CrRecordingFrameRateSettingMovie_60p},
    {"50p", SCRSDK::CrRecordingFrameRateSettingMovie_50p},
    {"30p", SCRSDK::CrRecordingFrameRateSettingMovie_30p},
    {"25p", SCRSDK::CrRecordingFrameRateSettingMovie_25p},
    {"24p", SCRSDK::CrRecordingFrameRateSettingMovie_24p},
    {"23.98p", SCRSDK::CrRecordingFrameRateSettingMovie_23_98p},
    {"29.97p", SCRSDK::CrRecordingFrameRateSettingMovie_29_97p},
    {"59.94p", SCRSDK::CrRecordingFrameRateSettingMovie_59_94p},
    {"24.00p", SCRSDK::CrRecordingFrameRateSettingMovie_24_00p},
    {"119.88p", SCRSDK::CrRecordingFrameRateSettingMovie_119_88p},
};

const EnumEntry kMovieQuality[] = {
    {"60p_50m", SCRSDK::CrRecordingSettingMovie_60p_50M},
    {"30p_50m", SCRSDK::CrRecordingSettingMovie_30p_50M},
    {"24p_50m", SCRSDK::CrRecordingSettingMovie_24p_50M},
    {"50p_50m", SCRSDK::CrRecordingSettingMovie_50p_50M},
    {"25p_50m", SCRSDK::CrRecordingSettingMovie_25p_50M},
    {"60i_24m", SCRSDK::CrRecordingSettingMovie_60i_24M},
    {"50i_24m_fx", SCRSDK::CrRecordingSettingMovie_50i_24M_FX},
    {"60i_17m_fh", SCRSDK::CrRecordingSettingMovie_60i_17M_FH},
    {"50i_17m_fh", SCRSDK::CrRecordingSettingMovie_50i_17M_FH},
    {"60p_28m_ps", SCRSDK::CrRecordingSettingMovie_60p_28M_PS},
    {"50p_28m_ps", SCRSDK::CrRecordingSettingMovie_50p_28M_PS},
    {"24p_24m_fx", SCRSDK::CrRecordingSettingMovie_24p_24M_FX},
    {"25p_24m_fx", SCRSDK::CrRecordingSettingMovie_25p_24M_FX},
    {"24p_17m_fh", SCRSDK::CrRecordingSettingMovie_24p_17M_FH},
    {"25p_17m_fh", SCRSDK::CrRecordingSettingMovie_25p_17M_FH},
    {"120p_50m_1280x720", SCRSDK::CrRecordingSettingMovie_120p_50M_1280x720},
    {"100p_50m_1280x720", SCRSDK::CrRecordingSettingMovie_100p_50M_1280x720},
    {"1920x1080_30p_16m", SCRSDK::CrRecordingSettingMovie_1920x1080_30p_16M},
    {"1920x1080_25p_16m", SCRSDK::CrRecordingSettingMovie_1920x1080_25p_16M},
    {"1280x720_30p_6m", SCRSDK::CrRecordingSettingMovie_1280x720_30p_6M},
    {"1280x720_25p_6m", SCRSDK::CrRecordingSettingMovie_1280x720_25p_6M},
    {"1920x1080_60p_28m", SCRSDK::CrRecordingSettingMovie_1920x1080_60p_28M},
    {"1920x1080_50p_28m", SCRSDK::CrRecordingSettingMovie_1920x1080_50p_28M},
    {"60p_25m_xavc_s_hd", SCRSDK::CrRecordingSettingMovie_60p_25M_XAVC_S_HD},
    {"50p_25m_xavc_s_hd", SCRSDK::CrRecordingSettingMovie_50p_25M_XAVC_S_HD},
    {"30p_16m_xavc_s_hd", SCRSDK::CrRecordingSettingMovie_30p_16M_XAVC_S_HD},
    {"25p_16m_xavc_s_hd", SCRSDK::CrRecordingSettingMovie_25p_16M_XAVC_S_HD},
    {"120p_100m_1920x1080_xavc_s_hd", SCRSDK::CrRecordingSettingMovie_120p_100M_1920x1080_XAVC_S_HD},
    {"100p_100m_1920x1080_xavc_s_hd", SCRSDK::CrRecordingSettingMovie_100p_100M_1920x1080_XAVC_S_HD},
    {"120p_60m_1920x1080_xavc_s_hd", SCRSDK::CrRecordingSettingMovie_120p_60M_1920x1080_XAVC_S_HD},
    {"100p_60m_1920x1080_xavc_s_hd", SCRSDK::CrRecordingSettingMovie_100p_60M_1920x1080_XAVC_S_HD},
    {"30p_100m_xavc_s_4k", SCRSDK::CrRecordingSettingMovie_30p_100M_XAVC_S_4K},
    {"25p_100m_xavc_s_4k", SCRSDK::CrRecordingSettingMovie_25p_100M_XAVC_S_4K},
    {"24p_100m_xavc_s_4k", SCRSDK::CrRecordingSettingMovie_24p_100M_XAVC_S_4K},
    {"30p_60m_xavc_s_4k", SCRSDK::CrRecordingSettingMovie_30p_60M_XAVC_S_4K},
    {"25p_60m_xavc_s_4k", SCRSDK::CrRecordingSettingMovie_25p_60M_XAVC_S_4K},
    {"24p_60m_xavc_s_4k", SCRSDK::CrRecordingSettingMovie_24p_60M_XAVC_S_4K},
    {"600m_422_10bit", SCRSDK::CrRecordingSettingMovie_600M_422_10bit},
    {"500m_422_10bit", SCRSDK::CrRecordingSettingMovie_500M_422_10bit},
    {"400m_420_10bit", SCRSDK::CrRecordingSettingMovie_400M_420_10bit},
    {"300m_422_10bit", SCRSDK::CrRecordingSettingMovie_300M_422_10bit},
    {"280m_422_10bit", SCRSDK::CrRecordingSettingMovie_280M_422_10bit},
    {"250m_422_10bit", SCRSDK::CrRecordingSettingMovie_250M_422_10bit},
    {"240m_422_10bit", SCRSDK::CrRecordingSettingMovie_240M_422_10bit},
    {"222m_422_10bit", SCRSDK::CrRecordingSettingMovie_222M_422_10bit},
    {"200m_422_10bit", SCRSDK::CrRecordingSettingMovie_200M_422_10bit},
    {"200m_420_10bit", SCRSDK::CrRecordingSettingMovie_200M_420_10bit},
    {"200m_420_8bit", SCRSDK::CrRecordingSettingMovie_200M_420_8bit},
    {"185m_422_10bit", SCRSDK::CrRecordingSettingMovie_185M_422_10bit},
    {"150m_420_10bit", SCRSDK::CrRecordingSettingMovie_150M_420_10bit},
    {"150m_420_8bit", SCRSDK::CrRecordingSettingMovie_150M_420_8bit},
    {"140m_422_10bit", SCRSDK::CrRecordingSettingMovie_140M_422_10bit},
    {"111m_422_10bit", SCRSDK::CrRecordingSettingMovie_111M_422_10bit},
    {"100m_422_10bit", SCRSDK::CrRecordingSettingMovie_100M_422_10bit},
    {"100m_420_10bit", SCRSDK::CrRecordingSettingMovie_100M_420_10bit},
    {"100m_420_8bit", SCRSDK::CrRecordingSettingMovie_100M_420_8bit},
    {"93m_422_10bit", SCRSDK::CrRecordingSettingMovie_93M_422_10bit},
    {"89m_422_10bit", SCRSDK::CrRecordingSettingMovie_89M_422_10bit},
    {"75m_420_10bit", SCRSDK::CrRecordingSettingMovie_75M_420_10bit},
    {"60m_420_8bit", SCRSDK::CrRecordingSettingMovie_60M_420_8bit},
    {"50m_422_10bit", SCRSDK::CrRecordingSettingMovie_50M_422_10bit},
    {"50m_420_10bit", SCRSDK::CrRecordingSettingMovie_50M_420_10bit},
    {"50m_420_8bit", SCRSDK::CrRecordingSettingMovie_50M_420_8bit},
    {"45m_420_10bit", SCRSDK::CrRecordingSettingMovie_45M_420_10bit},
    {"30m_420_10bit", SCRSDK::CrRecordingSettingMovie_30M_420_10bit},
    {"25m_420_8bit", SCRSDK::CrRecordingSettingMovie_25M_420_8bit},
    {"16m_420_8bit", SCRSDK::CrRecordingSettingMovie_16M_420_8bit},
    {"520m_422_10bit", SCRSDK::CrRecordingSettingMovie_520M_422_10bit},
    {"260m_422_10bit", SCRSDK::CrRecordingSettingMovie_260M_422_10bit},
};

const EnumEntry kPictureProfile[] = {
    {"off", SCRSDK::CrPictureProfile_Off},
    {"pp1", SCRSDK::CrPictureProfile_Number1},
    {"pp2", SCRSDK::CrPictureProfile_Number2},
    {"pp3", SCRSDK::CrPictureProfile_Number3},
    {"pp4", SCRSDK::CrPictureProfile_Number4},
    {"pp5", SCRSDK::CrPictureProfile_Number5},
    {"pp6", SCRSDK::CrPictureProfile_Number6},
    {"pp7", SCRSDK::CrPictureProfile_Number7},
    {"pp8", SCRSDK::CrPictureProfile_Number8},
    {"pp9", SCRSDK::CrPictureProfile_Number9},
    {"pp10", SCRSDK::CrPictureProfile_Number10},
    {"pp11", SCRSDK::CrPictureProfile_Number11},
    {"pplut1", SCRSDK::CrPictureProfile_LUT_Number1},
    {"pplut2", SCRSDK::CrPictureProfile_LUT_Number2},
    {"pplut3", SCRSDK::CrPictureProfile_LUT_Number3},
    {"pplut4", SCRSDK::CrPictureProfile_LUT_Number4},
};

const EnumEntry kLogShooting[] = {
    {"off", SCRSDK::CrLogShootingMode_Off},
    {"flexibleiso", SCRSDK::CrLogShootingMode_FlexibleISO},
};

const EnumEntry kSubjectRecognition[] = {
    {"off", SCRSDK::CrSubjectRecognitionInAF_Off},
    {"on", SCRSDK::CrSubjectRecognitionInAF_On},
};

const EnumEntry kRecognitionTarget[] = {
    {"auto", SCRSDK::CrRecognitionTarget_Auto},
    {"human", SCRSDK::CrRecognitionTarget_Person},
    {"animal_bird", SCRSDK::CrRecognitionTarget_AnimalBird},
    {"animal", SCRSDK::CrRecognitionTarget_Animal},
    {"bird", SCRSDK::CrRecognitionTarget_Bird},
    {"insect", SCRSDK::CrRecognitionTarget_Insect},
    {"car_train", SCRSDK::CrRecognitionTarget_CarTrain},
    {"plane", SCRSDK::CrRecognitionTarget_Plane},
};

const EnumEntry kEyeSelect[] = {
    {"auto", SCRSDK::CrRightLeftEyeSelect_Auto},
    {"right", SCRSDK::CrRightLeftEyeSelect_RightEye},
    {"left", SCRSDK::CrRightLeftEyeSelect_LeftEye},
};

const EnumEntry kSteadyShotMovie[] = {
    {"off", SCRSDK::CrImageStabilizationSteadyShotMovie_Off},
    {"standard", SCRSDK::CrImageStabilizationSteadyShotMovie_Standard},
    {"active", SCRSDK::CrImageStabilizationSteadyShotMovie_Active},
    {"dynamic_active", SCRSDK::CrImageStabilizationSteadyShotMovie_DynamicActive},
};

const EnumEntry kZoomRange[] = {
    {"optical_only", SCRSDK::CrZoomSetting_OpticalZoomOnly},
    {"smart_only", SCRSDK::CrZoomSetting_SmartZoomOnly},
    {"clear_image", SCRSDK::CrZoomSetting_On_ClearImageZoom},
    {"digital", SCRSDK::CrZoomSetting_On_DigitalZoom},
};

const EnumEntry kTouchOperation[] = {
    {"off", SCRSDK::CrTouchOperation_Off},
    {"on", SCRSDK::CrTouchOperation_On},
    {"playback_only", SCRSDK::CrTouchOperation_PlaybackOnly},
};

const EnumEntry kAutoPowerOffTemp[] = {
    {"standard", SCRSDK::CrAutoPowerOffTemperature_Standard},
    {"high", SCRSDK::CrAutoPowerOffTemperature_High},
};

std::string enumToString(const EnumEntry* table, size_t n, std::uint64_t v) {
    for (size_t i = 0; i < n; ++i)
        if (table[i].value == v) return table[i].name;
    if (v == 0xffffffffull) return "-";  // camera reports "no value" sentinel
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

std::string plainToString(std::uint64_t v) {
    return std::to_string(v);
}

bool plainFromString(const std::string& s, std::uint64_t& out) {
    try { out = std::stoull(s); } catch (...) { return false; }
    return true;
}

std::string kelvinToString(std::uint64_t v) {
    return std::to_string(static_cast<std::uint16_t>(v)) + "K";
}

bool kelvinFromString(const std::string& in, std::uint64_t& out) {
    std::string s = in;
    if (!s.empty() && (s.back() == 'K' || s.back() == 'k')) s.pop_back();
    try { out = std::stoul(s); } catch (...) { return false; }
    return true;
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
    {"color_temp", SCRSDK::CrDeviceProperty_Colortemp,
     SCRSDK::CrDataType_UInt16, nullptr, 0, kelvinToString, kelvinFromString},
    {"file_format", SCRSDK::CrDeviceProperty_FileType,
     SCRSDK::CrDataType_UInt16, kFileFormat, std::size(kFileFormat),
     nullptr, nullptr},
    {"image_quality", SCRSDK::CrDeviceProperty_StillImageQuality,
     SCRSDK::CrDataType_UInt16, kImageQuality, std::size(kImageQuality),
     nullptr, nullptr},
    {"iso_auto_min", SCRSDK::CrDeviceProperty_IsoAutoRangeLimitMin,
     SCRSDK::CrDataType_UInt32, nullptr, 0, isoToString, isoFromString},
    {"iso_auto_max", SCRSDK::CrDeviceProperty_IsoAutoRangeLimitMax,
     SCRSDK::CrDataType_UInt32, nullptr, 0, isoToString, isoFromString},
    {"movie_format", SCRSDK::CrDeviceProperty_Movie_File_Format,
     SCRSDK::CrDataType_UInt8, kMovieFormat, std::size(kMovieFormat),
     nullptr, nullptr},
    {"movie_fps", SCRSDK::CrDeviceProperty_Movie_Recording_FrameRateSetting,
     SCRSDK::CrDataType_UInt8, kMovieFps, std::size(kMovieFps),
     nullptr, nullptr},
    {"movie_quality", SCRSDK::CrDeviceProperty_Movie_Recording_Setting,
     SCRSDK::CrDataType_UInt16, kMovieQuality, std::size(kMovieQuality),
     nullptr, nullptr},
    {"picture_profile", SCRSDK::CrDeviceProperty_PictureProfile,
     SCRSDK::CrDataType_UInt8, kPictureProfile, std::size(kPictureProfile),
     nullptr, nullptr},
    {"log_shooting", SCRSDK::CrDeviceProperty_LogShootingMode,
     SCRSDK::CrDataType_UInt16, kLogShooting, std::size(kLogShooting),
     nullptr, nullptr},
    {"subject_recognition", SCRSDK::CrDeviceProperty_SubjectRecognitionInAF,
     SCRSDK::CrDataType_UInt8, kSubjectRecognition,
     std::size(kSubjectRecognition), nullptr, nullptr},
    {"recognition_target", SCRSDK::CrDeviceProperty_RecognitionTarget,
     SCRSDK::CrDataType_UInt16, kRecognitionTarget,
     std::size(kRecognitionTarget), nullptr, nullptr},
    {"eye_select", SCRSDK::CrDeviceProperty_RightLeftEyeSelect,
     SCRSDK::CrDataType_UInt8, kEyeSelect, std::size(kEyeSelect),
     nullptr, nullptr},
    {"af_transition_speed", SCRSDK::CrDeviceProperty_AFTransitionSpeed,
     SCRSDK::CrDataType_UInt8, nullptr, 0, plainToString, plainFromString},
    {"af_shift_sensitivity", SCRSDK::CrDeviceProperty_AFSubjShiftSens,
     SCRSDK::CrDataType_UInt8, nullptr, 0, plainToString, plainFromString},
    {"steadyshot_movie",
     SCRSDK::CrDeviceProperty_Movie_ImageStabilizationSteadyShot,
     SCRSDK::CrDataType_UInt8, kSteadyShotMovie, std::size(kSteadyShotMovie),
     nullptr, nullptr},
    {"zoom_range", SCRSDK::CrDeviceProperty_Zoom_Setting,
     SCRSDK::CrDataType_UInt8, kZoomRange, std::size(kZoomRange),
     nullptr, nullptr},
    {"touch_operation", SCRSDK::CrDeviceProperty_TouchOperation,
     SCRSDK::CrDataType_UInt8, kTouchOperation, std::size(kTouchOperation),
     nullptr, nullptr},
    {"auto_power_off_temp", SCRSDK::CrDeviceProperty_AutoPowerOffTemperature,
     SCRSDK::CrDataType_UInt8, kAutoPowerOffTemp, std::size(kAutoPowerOffTemp),
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
        reconnecting_ = false;
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
    void OnWarning(CrInt32u warning) override {
        if (warning == SCRSDK::CrNotify_Captured_Event) {
            std::lock_guard<std::mutex> lk(m_);
            capturedEvent_ = true;
            cv_.notify_all();
            return;
        }
        if (warning == SCRSDK::CrWarning_Connect_Reconnecting) {
            std::lock_guard<std::mutex> lk(m_);
            if (!reconnecting_)
                std::fprintf(stderr,
                             "sonycamd: camera connection lost (USB unplugged "
                             "or camera off)\n");
            reconnecting_ = true;
            return;
        }
        if (warning == SCRSDK::CrWarning_Connect_Reconnected) {
            std::lock_guard<std::mutex> lk(m_);
            reconnecting_ = false;
            std::fprintf(stderr, "sonycamd: camera reconnected\n");
            return;
        }
        if (warning == SCRSDK::CrWarning_CustomWBCapture_Result_OK) {
            std::lock_guard<std::mutex> lk(m_);
            wbResult_ = 1;
            cv_.notify_all();
            return;
        }
        if (warning == SCRSDK::CrWarning_CustomWBCapture_Result_NG ||
            warning == SCRSDK::CrWarning_CustomWBCapture_Result_Invalid) {
            std::lock_guard<std::mutex> lk(m_);
            wbResult_ = -1;
            cv_.notify_all();
            return;
        }
        if (warning == SCRSDK::CrWarning_CameraSettings_Read_Result_OK) {
            std::lock_guard<std::mutex> lk(m_);
            settingsResult_ = 1;
            cv_.notify_all();
            return;
        }
        if (warning == SCRSDK::CrWarning_CameraSettings_Read_Result_NG ||
            warning == SCRSDK::CrWarning_CameraSettings_Read_Result_Invalid ||
            warning == SCRSDK::CrWarning_CameraSettings_Save_Result_NG) {
            std::lock_guard<std::mutex> lk(m_);
            settingsResult_ = -1;
            cv_.notify_all();
            return;
        }
        std::fprintf(stderr, "sonycamd: camera warning 0x%08x\n", warning);
    }
    void OnCompleteDownload(CrChar* filename, CrInt32u) override {
        std::lock_guard<std::mutex> lk(m_);
        downloadedFile_ = filename ? reinterpret_cast<const char*>(filename) : "";
        downloadDone_ = true;
        cv_.notify_all();
    }
    // Contents-transfer pulls signal completion through this callback
    // instead of OnCompleteDownload.
    void OnNotifyContentsTransfer(CrInt32u notify, SCRSDK::CrContentHandle,
                                  CrChar* filename) override {
        if (notify != SCRSDK::CrNotify_ContentsTransfer_Complete) return;
        std::lock_guard<std::mutex> lk(m_);
        downloadedFile_ = filename ? reinterpret_cast<const char*>(filename) : "";
        downloadDone_ = true;
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
        reconnecting_ = false;
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
    bool isReconnecting() {
        std::lock_guard<std::mutex> lk(m_);
        return reconnecting_;
    }
    void armDownload() {
        std::lock_guard<std::mutex> lk(m_);
        downloadDone_ = false;
        capturedEvent_ = false;
        downloadedFile_.clear();
    }
    void armSettingsResult() {
        std::lock_guard<std::mutex> lk(m_);
        settingsResult_ = 0;
    }
    void armWbResult() {
        std::lock_guard<std::mutex> lk(m_);
        wbResult_ = 0;
    }
    int waitWbResult(int timeoutSec) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(timeoutSec),
                     [&] { return wbResult_ != 0; });
        return wbResult_;
    }
    // 1 = ok, -1 = rejected, 0 = no confirmation within the timeout
    int waitSettingsResult(int timeoutSec) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(timeoutSec),
                     [&] { return settingsResult_ != 0; });
        return settingsResult_;
    }
    // True once the camera reports the shutter actually fired (or the file
    // already arrived, for bodies that skip the captured event).
    bool waitCaptured(int timeoutMs) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [&] { return capturedEvent_ || downloadDone_; });
    }
    bool waitDownload(int timeoutSec, std::string& file) {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, std::chrono::seconds(timeoutSec),
                          [&] { return downloadDone_; }))
            return false;
        file = downloadedFile_;
        return true;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool connected_ = false;
    bool reconnecting_ = false;
    CrInt32u lastError_ = 0;
    CrInt32u disconnectReason_ = 0;
    bool downloadDone_ = false;
    bool capturedEvent_ = false;
    int settingsResult_ = 0;
    int wbResult_ = 0;
    std::string downloadedFile_;
};

std::string crErrorString(CrError e) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "CrError 0x%04x", static_cast<unsigned>(e));
    return buf;
}

std::string recordingStateName(std::uint64_t v) {
    switch (v) {
        case SCRSDK::CrMovie_Recording_State_Not_Recording: return "not_recording";
        case SCRSDK::CrMovie_Recording_State_Recording: return "recording";
        case SCRSDK::CrMovie_Recording_State_Recording_Failed: return "failed";
        case SCRSDK::CrMovie_Recording_State_IntervalRec_Waiting_Record:
            return "interval_waiting";
        default: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llx",
                          static_cast<unsigned long long>(v));
            return buf;
        }
    }
}

std::string focusIndicationName(std::uint64_t v) {
    switch (v) {
        case SCRSDK::CrFocusIndicator_Unlocked: return "unlocked";
        case SCRSDK::CrFocusIndicator_Focused_AF_S:
        case SCRSDK::CrFocusIndicator_Focused_AF_C: return "focused";
        case SCRSDK::CrFocusIndicator_NotFocused_AF_S:
        case SCRSDK::CrFocusIndicator_NotFocused_AF_C: return "not_focused";
        case SCRSDK::CrFocusIndicator_TrackingSubject_AF_C: return "tracking";
        default: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llx",
                          static_cast<unsigned long long>(v));
            return buf;
        }
    }
}

class CrsdkBackend : public CameraBackend {
public:
    ~CrsdkBackend() override {
        disconnect();
        if (sdkInit_) SCRSDK::Release();
    }

    Result connect() override {
        return connectMode(SCRSDK::CrSdkControlMode_Remote);
    }

    Result connectMode(SCRSDK::CrSdkControlMode mode) {
        userDisconnected_ = false;
        if (handle_ != 0) {
            if (callback_.isConnected() && !callback_.isReconnecting() &&
                mode_ == mode)
                return Result::success();
            teardown();  // stale handle or wrong SDK mode
        }
        connecting_ = true;
        Result r = connectImpl(mode);
        connecting_ = false;
        if (r.ok) mode_ = mode;
        return r;
    }

    Result connectImpl(SCRSDK::CrSdkControlMode mode) {
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
        transport_ = reinterpret_cast<const char*>(found->GetConnectionTypeName());
        for (auto& c : transport_) c = static_cast<char>(std::tolower(c));
        enumInfo->Release();
        if (!camera_) return Result::fail("CreateCameraObjectInfo failed");

        callback_.reset();
        err = SCRSDK::Connect(camera_, &callback_, &handle_, mode);
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

        if (mode != SCRSDK::CrSdkControlMode_Remote)
            return Result::success();  // property setup is Remote-mode only

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
        userDisconnected_ = true;
        return teardown();
    }

    // Recovers dropped connections (USB unplug/replug, camera slept) by
    // tearing down and reconnecting. Explicit `disconnect` stays sticky.
    Result ensureConnected() {
        // connect() itself uses getProp/setProp for readiness checks; don't
        // recurse into another connect attempt from those calls.
        if (connecting_)
            return handle_ != 0 ? Result::success()
                                : Result::fail("not connected");
        if (handle_ != 0 && callback_.isConnected() &&
            !callback_.isReconnecting() &&
            mode_ == SCRSDK::CrSdkControlMode_Remote)
            return Result::success();
        if (userDisconnected_)
            return Result::fail("not connected (run 'sonycam connect')");
        if (handle_ != 0) {
            std::fprintf(stderr,
                         "sonycamd: camera connection lost; reconnecting\n");
            teardown();
        }
        return connect();
    }

    CameraInfo info() override {
        CameraInfo ci;
        ci.connected = handle_ != 0 && callback_.isConnected() &&
                       !callback_.isReconnecting();
        ci.model = model_;
        ci.transport = transport_.empty() ? "-" : transport_;
        return ci;
    }

    Result gearInfo(std::vector<PropInfo>& out) override {
        Result conn = ensureConnected();
        if (!conn.ok) return conn;
        auto add = [&](const char* name, const std::string& v) {
            if (!v.empty()) out.push_back(PropInfo{name, v, false, {}});
        };
        std::string model = readStringProp(SCRSDK::CrDeviceProperty_ModelName);
        add("model", model.empty() ? model_ : model);
        add("body_serial",
            readStringProp(SCRSDK::CrDeviceProperty_BodySerialNumber));
        add("body_firmware",
            readStringProp(SCRSDK::CrDeviceProperty_SoftwareVersion));
        add("lens", readStringProp(SCRSDK::CrDeviceProperty_LensModelName));
        add("lens_serial",
            readStringProp(SCRSDK::CrDeviceProperty_LensSerialNumber));
        add("lens_firmware",
            readStringProp(SCRSDK::CrDeviceProperty_LensVersionNumber));

        std::uint64_t zoomEnabled = 0;
        if (readNumericProp(SCRSDK::CrDeviceProperty_Zoom_Operation_Status,
                            zoomEnabled))
            add("remote_zoom", zoomEnabled ? "yes" : "no");
        return Result::success();
    }

    Result listProps(std::vector<PropInfo>& out) override {
        Result conn = ensureConnected();
        if (!conn.ok) return conn;
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
        Result conn = ensureConnected();
        if (!conn.ok) return conn;
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
        Result conn = ensureConnected();
        if (!conn.ok) return conn;
        const PropDef* def = findProp(name);
        if (!def) return Result::fail("unknown property: " + name);
        std::uint64_t raw = 0;
        if (!valueFromString(*def, value, raw))
            return Result::fail("cannot parse value '" + value + "' for " + name);

        // The camera reports everything as read-only for a couple of seconds
        // after a capture or mode change, so poll before giving up.
        CrDeviceProperty* current = nullptr;
        CrInt32 num = 0;
        CrInt32u code = def->code;
        SCRSDK::CrDataType valueType{};
        bool writable = false;
        for (int i = 0; i < 13; ++i) {
            if (i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            CrError rerr = SCRSDK::GetSelectDeviceProperties(
                handle_, 1, &code, &current, &num);
            if (rerr != SCRSDK::CrError_None || num == 0 || !current) {
                if (current) SCRSDK::ReleaseDeviceProperties(handle_, current);
                current = nullptr;
                if (i == 0)
                    return Result::fail("cannot read " + name +
                                        " before writing: " + crErrorString(rerr));
                continue;
            }
            valueType = current[0].GetValueType();
            writable = current[0].IsSetEnableCurrentValue();
            SCRSDK::ReleaseDeviceProperties(handle_, current);
            current = nullptr;
            if (writable) break;
        }
        if (!writable)
            return Result::fail(name +
                                " is not writable in the current camera mode");
        CrError err;

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

    Result record(const std::string& op, std::string& outState) override {
        Result conn = ensureConnected();
        if (!conn.ok) return conn;

        auto readState = [&](std::uint64_t& v) {
            return readNumericProp(SCRSDK::CrDeviceProperty_RecordingState, v);
        };
        std::uint64_t state = 0;
        if (!readState(state))
            return Result::fail("recording state not available");
        outState = recordingStateName(state);

        if (op == "status") return Result::success();
        if (op != "start" && op != "stop")
            return Result::fail("unknown record op: " + op);

        const bool wantRecording = op == "start";
        if ((state == SCRSDK::CrMovie_Recording_State_Recording) ==
            wantRecording)
            return Result::success();  // already in the requested state

        CrError err = SCRSDK::SendCommand(
            handle_, SCRSDK::CrCommandId_MovieRecord, SCRSDK::CrCommandParam_Down);
        if (err != SCRSDK::CrError_None)
            return Result::fail("record command failed: " + crErrorString(err));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SCRSDK::SendCommand(handle_, SCRSDK::CrCommandId_MovieRecord,
                            SCRSDK::CrCommandParam_Up);

        // Starting is quick; stopping can take a while to finalize the file.
        const int tries = wantRecording ? 50 : 100;
        for (int i = 0; i < tries; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!readState(state)) continue;
            outState = recordingStateName(state);
            if (state == SCRSDK::CrMovie_Recording_State_Recording_Failed)
                return Result::fail(
                    "camera reports recording failed (check the memory card "
                    "and that the camera is in a movie mode)");
            if ((state == SCRSDK::CrMovie_Recording_State_Recording) ==
                wantRecording)
                return Result::success();
        }
        return Result::fail("camera did not " + op + " recording (state: " +
                            outState +
                            "); is the camera in a movie mode with a card?");
    }

    Result zoom(const std::string& op, int ms) override {
        Result conn = ensureConnected();
        if (!conn.ok) return conn;
        std::int8_t dir;
        if (op == "in") dir = SCRSDK::CrZoomOperation_Tele;
        else if (op == "out") dir = SCRSDK::CrZoomOperation_Wide;
        else if (op == "stop") dir = SCRSDK::CrZoomOperation_Stop;
        else return Result::fail("unknown zoom op: " + op);

        // Zoom_Operation reports writable even for mechanical lenses; the
        // authoritative signal is Zoom_Operation_Status.
        std::uint64_t zoomEnabled = 0;
        if (readNumericProp(SCRSDK::CrDeviceProperty_Zoom_Operation_Status,
                            zoomEnabled) &&
            zoomEnabled == SCRSDK::CrZoomOperationEnableStatus_Disable)
            return Result::fail(
                "lens does not support remote zoom (see 'sonycam info')");

        CrDeviceProperty* props = nullptr;
        CrInt32 num = 0;
        CrInt32u code = SCRSDK::CrDeviceProperty_Zoom_Operation;
        CrError err = SCRSDK::GetSelectDeviceProperties(
            handle_, 1, &code, &props, &num);
        if (err != SCRSDK::CrError_None || num == 0 || !props) {
            if (props) SCRSDK::ReleaseDeviceProperties(handle_, props);
            return Result::fail("zoom not available: " + crErrorString(err));
        }
        SCRSDK::CrDataType valueType = props[0].GetValueType();
        bool writable = props[0].IsSetEnableCurrentValue();
        SCRSDK::ReleaseDeviceProperties(handle_, props);
        if (!writable)
            return Result::fail(
                "lens does not support remote zoom (see 'sonycam info')");

        auto drive = [&](std::int8_t d) {
            CrDeviceProperty p;
            p.SetCode(code);
            p.SetValueType(valueType);
            p.SetCurrentValue(static_cast<std::uint8_t>(d));
            return SCRSDK::SetDeviceProperty(handle_, &p);
        };
        err = drive(dir);
        if (err != SCRSDK::CrError_None)
            return Result::fail("zoom failed: " + crErrorString(err));
        if (dir != SCRSDK::CrZoomOperation_Stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            drive(SCRSDK::CrZoomOperation_Stop);
        }
        return Result::success();
    }

    Result preset(const std::string& op, const std::string& path) override {
        Result conn = ensureConnected();
        if (!conn.ok) return conn;
        std::error_code ec;
        std::filesystem::path p = std::filesystem::absolute(path, ec);

        if (op == "save") {
            std::filesystem::create_directories(p.parent_path(), ec);
            std::string dir = p.parent_path().string();
            std::string name = p.filename().string();
            std::filesystem::remove(p, ec);
            callback_.armSettingsResult();
            CrError err = SCRSDK::DownloadSettingFile(
                handle_, SCRSDK::CrDownloadSettingFileType_Setup,
                const_cast<CrChar*>(dir.c_str()),
                const_cast<CrChar*>(name.c_str()));
            if (err != SCRSDK::CrError_None)
                return Result::fail("preset save failed: " +
                                    crErrorString(err));
            for (int i = 0; i < 100; ++i) {  // wait for the file to arrive
                if (std::filesystem::exists(p) &&
                    std::filesystem::file_size(p, ec) > 0)
                    return Result::success();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return Result::fail(
                "camera did not deliver a settings file within 10s");
        }

        if (op == "load") {
            if (!std::filesystem::exists(p))
                return Result::fail("no such preset file: " + p.string());
            callback_.armSettingsResult();
            std::string full = p.string();
            CrError err = SCRSDK::UploadSettingFile(
                handle_, SCRSDK::CrUploadSettingFileType_Setup,
                const_cast<CrChar*>(full.c_str()));
            if (err != SCRSDK::CrError_None)
                return Result::fail("preset load failed: " +
                                    crErrorString(err));
            int r = callback_.waitSettingsResult(30);
            if (r < 0)
                return Result::fail(
                    "camera rejected the settings file (wrong model or "
                    "firmware version?)");
            if (r == 0)
                return Result::fail(
                    "no confirmation from the camera within 30s");
            return Result::success();
        }

        return Result::fail("unknown preset op: " + op);
    }

    // Runs `fn` with the camera connected in ContentsTransfer mode, then
    // restores the normal Remote session.
    template <typename Fn>
    Result withContentsMode(Fn fn) {
        Result conn = connectMode(SCRSDK::CrSdkControlMode_ContentsTransfer);
        if (!conn.ok) return conn;
        Result out = fn();
        teardown();
        connect();  // best effort; the next command reconnects anyway
        return out;
    }

    // The camera rejects contents requests while it is still building its
    // MTP database right after the mode switch, so poll patiently.
    CrError getDateFoldersRetry(SCRSDK::CrMtpFolderInfo** folders,
                                CrInt32u* nFolders) {
        CrError err = SCRSDK::CrError_Generic_Unknown;
        for (int i = 0; i < 30; ++i) {  // up to ~15s
            err = SCRSDK::GetDateFolderList(handle_, folders, nFolders);
            if (err == SCRSDK::CrError_None) return err;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return err;
    }

    Result filesList(std::vector<FileEntry>& out) override {
        return withContentsMode([&]() -> Result {
            SCRSDK::CrMtpFolderInfo* folders = nullptr;
            CrInt32u nFolders = 0;
            CrError err = getDateFoldersRetry(&folders, &nFolders);
            if (err != SCRSDK::CrError_None)
                return Result::fail("cannot list card folders: " +
                                    crErrorString(err));
            for (CrInt32u f = 0; f < nFolders; ++f) {
                SCRSDK::CrContentHandle* contents = nullptr;
                CrInt32u nContents = 0;
                err = SCRSDK::GetContentsHandleList(handle_, folders[f].handle,
                                                    &contents, &nContents);
                if (err != SCRSDK::CrError_None || !contents) continue;
                for (CrInt32u c = 0; c < nContents; ++c) {
                    SCRSDK::CrMtpContentsInfo info;
                    if (SCRSDK::GetContentsDetailInfo(handle_, contents[c],
                                                      &info) !=
                        SCRSDK::CrError_None)
                        continue;
                    FileEntry e;
                    e.name = info.fileName
                                 ? reinterpret_cast<const char*>(info.fileName)
                                 : "";
                    e.size = info.contentSize;
                    e.date = std::string(info.dateChar,
                                         strnlen(info.dateChar, 16));
                    out.push_back(std::move(e));
                }
                SCRSDK::ReleaseContentsHandleList(handle_, contents);
            }
            if (folders) SCRSDK::ReleaseDateFolderList(handle_, folders);
            return Result::success();
        });
    }

    Result filesPull(const std::string& name, const std::string& dir,
                     std::string& outFile) override {
        std::string saveDir = dir.empty() ? std::string(".") : dir;
        std::error_code ec;
        std::filesystem::create_directories(saveDir, ec);
        if (ec)
            return Result::fail("cannot create " + saveDir + ": " + ec.message());
        saveDir = std::filesystem::absolute(saveDir, ec).string();

        return withContentsMode([&]() -> Result {
            SCRSDK::CrMtpFolderInfo* folders = nullptr;
            CrInt32u nFolders = 0;
            CrError err = getDateFoldersRetry(&folders, &nFolders);
            if (err != SCRSDK::CrError_None)
                return Result::fail("cannot list card folders: " +
                                    crErrorString(err));
            SCRSDK::CrContentHandle target = 0;
            for (CrInt32u f = 0; f < nFolders && !target; ++f) {
                SCRSDK::CrContentHandle* contents = nullptr;
                CrInt32u nContents = 0;
                err = SCRSDK::GetContentsHandleList(handle_, folders[f].handle,
                                                    &contents, &nContents);
                if (err != SCRSDK::CrError_None || !contents) continue;
                for (CrInt32u c = 0; c < nContents && !target; ++c) {
                    SCRSDK::CrMtpContentsInfo info;
                    if (SCRSDK::GetContentsDetailInfo(handle_, contents[c],
                                                      &info) !=
                        SCRSDK::CrError_None)
                        continue;
                    if (info.fileName &&
                        name == reinterpret_cast<const char*>(info.fileName))
                        target = contents[c];
                }
                SCRSDK::ReleaseContentsHandleList(handle_, contents);
            }
            if (folders) SCRSDK::ReleaseDateFolderList(handle_, folders);
            if (!target)
                return Result::fail("no file named '" + name +
                                    "' on the memory card");

            callback_.armDownload();
            err = SCRSDK::PullContentsFile(
                handle_, target, SCRSDK::CrPropertyStillImageTransSize_Original,
                const_cast<CrChar*>(saveDir.c_str()));
            if (err != SCRSDK::CrError_None)
                return Result::fail("pull failed: " + crErrorString(err));
            if (!callback_.waitDownload(300, outFile))  // videos can be huge
                return Result::fail("transfer did not finish within 5 minutes");
            return Result::success();
        });
    }

    Result wbCapture(std::string& outStatus) override {
        Result conn = ensureConnected();
        if (!conn.ok) return conn;
        auto setU16 = [&](CrInt32u code, std::uint64_t v, SCRSDK::CrDataType t) {
            CrDeviceProperty p;
            p.SetCode(code);
            p.SetValueType(t);
            p.SetCurrentValue(v);
            return SCRSDK::SetDeviceProperty(handle_, &p);
        };
        CrError err = setU16(SCRSDK::CrDeviceProperty_CustomWB_Capture_Standby,
                             SCRSDK::CrPropertyCustomWBOperation_Enable,
                             SCRSDK::CrDataType_UInt16);
        if (err != SCRSDK::CrError_None)
            return Result::fail(
                "WB capture standby failed: " + crErrorString(err) +
                " (run 'set white_balance custom_1' first)");
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        callback_.armWbResult();
        const std::uint64_t center = (320u << 16) | 240u;  // frame center
        err = setU16(SCRSDK::CrDeviceProperty_CustomWB_Capture, center,
                     SCRSDK::CrDataType_UInt32);
        int result = 0;
        if (err == SCRSDK::CrError_None)
            result = callback_.waitWbResult(15);

        setU16(SCRSDK::CrDeviceProperty_CustomWB_Capture_Standby_Cancel,
               SCRSDK::CrPropertyCustomWBOperation_Enable,
               SCRSDK::CrDataType_UInt16);

        if (err != SCRSDK::CrError_None)
            return Result::fail("WB capture failed: " + crErrorString(err));
        if (result < 0)
            return Result::fail(
                "camera rejected the WB capture (aim at a neutral surface "
                "with enough light and retry)");
        if (result == 0)
            return Result::fail("no WB capture confirmation within 15s");
        outStatus = "captured (stored in the camera's custom WB slot; "
                    "'set white_balance custom' to use it)";
        return Result::success();
    }

    Result focus(const std::string& op, int steps,
                 std::string& outStatus) override {
        Result conn = ensureConnected();
        if (!conn.ok) return conn;

        if (op == "status") {
            std::uint64_t v = 0;
            if (!readNumericProp(SCRSDK::CrDeviceProperty_FocusIndication, v))
                return Result::fail("focus indication not available");
            outStatus = focusIndicationName(v);
            return Result::success();
        }

        if (op == "af") {
            CrDeviceProperty s1;
            s1.SetCode(SCRSDK::CrDeviceProperty_S1);
            s1.SetValueType(SCRSDK::CrDataType_UInt16);
            s1.SetCurrentValue(SCRSDK::CrLockIndicator_Locked);
            CrError err = SCRSDK::SetDeviceProperty(handle_, &s1);
            if (err != SCRSDK::CrError_None)
                return Result::fail("AF half-press failed: " + crErrorString(err));

            std::string state = "unknown";
            bool locked = false;
            for (int i = 0; i < 50; ++i) {  // up to 5s
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::uint64_t v = 0;
                if (!readNumericProp(SCRSDK::CrDeviceProperty_FocusIndication, v))
                    continue;
                state = focusIndicationName(v);
                if (state == "focused" || state == "tracking") {
                    locked = true;
                    break;
                }
            }
            s1.SetCurrentValue(SCRSDK::CrLockIndicator_Unlocked);
            SCRSDK::SetDeviceProperty(handle_, &s1);
            outStatus = state;
            if (!locked)
                return Result::fail(
                    "autofocus did not lock (" + state +
                    "); try more light, a different focus_area, or focus_mode "
                    "mf with 'focus near/far'");
            return Result::success();
        }

        if (op == "at") {  // steps = (x << 16 | y), 640x480 space
            CrDeviceProperty p;
            p.SetCode(SCRSDK::CrDeviceProperty_AF_Area_Position);
            p.SetValueType(SCRSDK::CrDataType_UInt32);
            p.SetCurrentValue(static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(steps)));
            CrError err = SCRSDK::SetDeviceProperty(handle_, &p);
            if (err != SCRSDK::CrError_None)
                return Result::fail(
                    "cannot move the AF area: " + crErrorString(err) +
                    " (try 'set focus_area spot_m' or a tracking area first)");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return focus("af", 1, outStatus);  // lock on the new position
        }

        if (op == "save" || op == "recall") {  // steps = memory slot
            CrInt32u code = op == "save"
                                ? SCRSDK::CrDeviceProperty_ZoomAndFocusPosition_Save
                                : SCRSDK::CrDeviceProperty_ZoomAndFocusPosition_Load;
            CrDeviceProperty* props = nullptr;
            CrInt32 num = 0;
            CrError err = SCRSDK::GetSelectDeviceProperties(
                handle_, 1, &code, &props, &num);
            if (err != SCRSDK::CrError_None || num == 0 || !props) {
                if (props) SCRSDK::ReleaseDeviceProperties(handle_, props);
                return Result::fail(
                    "zoom/focus position memories are not supported here");
            }
            SCRSDK::CrDataType valueType = props[0].GetValueType();
            bool writable = props[0].IsSetEnableCurrentValue();
            SCRSDK::ReleaseDeviceProperties(handle_, props);
            if (!writable)
                return Result::fail(
                    "focus memories are not available in the current mode");
            CrDeviceProperty p;
            p.SetCode(code);
            p.SetValueType(valueType);
            p.SetCurrentValue(static_cast<std::uint64_t>(steps));
            err = SCRSDK::SetDeviceProperty(handle_, &p);
            if (err != SCRSDK::CrError_None)
                return Result::fail("focus memory " + op + " failed: " +
                                    crErrorString(err));
            if (op == "recall") {  // wait for the lens drive to finish
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                for (int i = 0; i < 100; ++i) {
                    std::uint64_t driving = 0;
                    if (readNumericProp(
                            SCRSDK::CrDeviceProperty_FocusDrivingStatus,
                            driving) &&
                        driving != SCRSDK::CrFocusDrivingStatus_Driving)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            outStatus = op + " slot " + std::to_string(steps);
            return Result::success();
        }

        if (op == "position") {
            CrDeviceProperty* props = nullptr;
            CrInt32 num = 0;
            CrInt32u code = SCRSDK::CrDeviceProperty_FocusPositionSetting;
            CrError err = SCRSDK::GetSelectDeviceProperties(
                handle_, 1, &code, &props, &num);
            if (err != SCRSDK::CrError_None || num == 0 || !props) {
                if (props) SCRSDK::ReleaseDeviceProperties(handle_, props);
                return Result::fail(
                    "absolute focus positioning is not supported here");
            }
            SCRSDK::CrDataType valueType = props[0].GetValueType();
            bool writable = props[0].IsSetEnableCurrentValue();
            std::uint16_t rangeMin = 0, rangeMax = 0;
            {
                CrInt8u* values = props[0].GetValues();
                if (values && props[0].GetValueSize() >= 4) {
                    std::memcpy(&rangeMin, values, 2);
                    std::memcpy(&rangeMax, values + 2, 2);
                }
            }
            SCRSDK::ReleaseDeviceProperties(handle_, props);

            std::uint64_t cur = 0;
            readNumericProp(SCRSDK::CrDeviceProperty_FocusPositionCurrentValue,
                            cur);
            if (steps < 0) {  // read-only query
                outStatus = "position " + std::to_string(cur) + " (range " +
                            std::to_string(rangeMin) + ".." +
                            std::to_string(rangeMax) + ")";
                return Result::success();
            }
            if (!writable)
                return Result::fail(
                    "focus position is not settable in the current mode "
                    "(set focus_mode mf)");
            if (steps < rangeMin || steps > rangeMax)
                return Result::fail(
                    "position out of range " + std::to_string(rangeMin) +
                    ".." + std::to_string(rangeMax));

            const std::uint64_t target = static_cast<std::uint64_t>(steps);
            const std::uint64_t start = cur;
            auto diff = [](std::uint64_t a, std::uint64_t b) {
                return a > b ? a - b : b - a;
            };
            if (start == target) {
                outStatus = "position " + std::to_string(cur);
                return Result::success();
            }
            // FocusDrivingStatus lags reality, so treat the position itself
            // as ground truth: wait until it converges on the target,
            // re-issuing the (occasionally dropped) drive request on stall.
            auto issue = [&]() {
                CrDeviceProperty p;
                p.SetCode(code);
                p.SetValueType(valueType);
                p.SetCurrentValue(target);
                return SCRSDK::SetDeviceProperty(handle_, &p);
            };
            err = issue();
            if (err != SCRSDK::CrError_None)
                return Result::fail("set focus position failed: " +
                                    crErrorString(err));
            std::uint64_t last = start;
            int stall = 0, resends = 0;
            for (int i = 0; i < 300; ++i) {  // up to 30s of lens travel
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!readNumericProp(
                        SCRSDK::CrDeviceProperty_FocusPositionCurrentValue,
                        cur))
                    continue;
                if (diff(cur, target) <= 200) break;  // arrived
                if (cur == last) {
                    if (++stall >= 15) {  // 1.5s without movement
                        if (resends >= 2) break;  // physical limit reached
                        issue();
                        ++resends;
                        stall = 0;
                    }
                } else {
                    stall = 0;
                    last = cur;
                }
            }
            readNumericProp(SCRSDK::CrDeviceProperty_FocusPositionCurrentValue,
                            cur);
            outStatus = "position " + std::to_string(cur);
            if (diff(cur, target) > 2000)
                return Result::fail(
                    "lens stopped at " + std::to_string(cur) + " instead of " +
                    std::to_string(target) +
                    " (probably a physical focus limit at this zoom)");
            return Result::success();
        }

        if (op == "near" || op == "far") {
            CrDeviceProperty* props = nullptr;
            CrInt32 num = 0;
            CrInt32u code = SCRSDK::CrDeviceProperty_NearFar;
            CrError err = SCRSDK::GetSelectDeviceProperties(
                handle_, 1, &code, &props, &num);
            if (err != SCRSDK::CrError_None || num == 0 || !props) {
                if (props) SCRSDK::ReleaseDeviceProperties(handle_, props);
                return Result::fail("manual focus drive not available: " +
                                    crErrorString(err));
            }
            SCRSDK::CrDataType valueType = props[0].GetValueType();
            bool writable = props[0].IsSetEnableCurrentValue();
            SCRSDK::ReleaseDeviceProperties(handle_, props);
            if (!writable)
                return Result::fail(
                    "focus nudge requires focus_mode mf (set focus_mode mf)");

            const std::int16_t step = (op == "near")
                                          ? SCRSDK::CrPropValueMinus1
                                          : SCRSDK::CrPropValuePlus1;
            for (int i = 0; i < steps; ++i) {
                CrDeviceProperty p;
                p.SetCode(code);
                p.SetValueType(valueType);
                p.SetCurrentValue(static_cast<std::uint16_t>(step));
                err = SCRSDK::SetDeviceProperty(handle_, &p);
                if (err != SCRSDK::CrError_None)
                    return Result::fail("focus nudge failed: " +
                                        crErrorString(err));
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            outStatus = op + " x" + std::to_string(steps);
            return Result::success();
        }

        return Result::fail("unknown focus op: " + op);
    }

    Result capture(const std::string& saveDir, std::string& outFile) override {
        Result conn = ensureConnected();
        if (!conn.ok) return conn;
        std::string dir = saveDir.empty() ? std::string(".") : saveDir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return Result::fail("cannot create " + dir + ": " + ec.message());
        dir = std::filesystem::absolute(dir, ec).string();
        SCRSDK::SetSaveInfo(handle_,
                            const_cast<CrChar*>(dir.c_str()),
                            const_cast<CrChar*>(""), -1);

        // The camera only transfers the shot when the still image store
        // destination includes the host PC.
        Result dest = ensureHostPcStoreDestination();
        if (!dest.ok) return dest;

        // The camera silently drops the release while it settles after a
        // mode change. CrNotify_Captured_Event tells us whether the shutter
        // actually fired: no event within ~2s means the release was dropped,
        // so retry; once it fires, just wait for the download (never
        // re-release, to avoid taking a second shot).
        callback_.armDownload();
        bool fired = false;
        for (int attempt = 0; attempt < 5 && !fired; ++attempt) {
            CrError err = SCRSDK::SendCommand(
                handle_, SCRSDK::CrCommandId_Release, SCRSDK::CrCommandParam_Down);
            if (err != SCRSDK::CrError_None)
                return Result::fail("shutter down failed: " + crErrorString(err));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            SCRSDK::SendCommand(handle_, SCRSDK::CrCommandId_Release,
                                SCRSDK::CrCommandParam_Up);
            fired = callback_.waitCaptured(2000);
        }
        if (!fired)
            return Result::fail(
                "the camera refused the shutter release: it may not be in a "
                "still-image mode, or autofocus could not lock (try 'focus "
                "af' first, or 'set focus_mode mf')");
        if (!callback_.waitDownload(30, outFile))
            return Result::fail(
                "the shot was taken but no file arrived within 30s (check "
                "the memory card and USB connection)");
        return Result::success();
    }

    Result liveviewFrame(const std::string& path) override {
        Result conn = ensureConnected();
        if (!conn.ok) return conn;
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
        // Write atomically so streaming readers never see a partial frame.
        const std::string tmp = path + ".tmp";
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return Result::fail("cannot write " + tmp);
        f.write(reinterpret_cast<const char*>(block.GetImageData()),
                block.GetImageSize());
        f.close();
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) return Result::fail("cannot rename to " + path + ": " + ec.message());
        return Result::success();
    }

private:
    Result teardown() {
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

    // Reads a CrDataType_STR property (length-prefixed UTF-16). Returns ""
    // when the property is unsupported or empty.
    std::string readStringProp(CrInt32u code) {
        CrDeviceProperty* props = nullptr;
        CrInt32 num = 0;
        CrError err = SCRSDK::GetSelectDeviceProperties(
            handle_, 1, &code, &props, &num);
        std::string out;
        if (err == SCRSDK::CrError_None && num > 0 && props) {
            if (props[0].GetValueType() == SCRSDK::CrDataType_STR) {
                CrInt16u* s = props[0].GetCurrentStr();
                if (s) {
                    int len = static_cast<int>(*s);
                    for (int i = 1; i < len && s[i]; ++i)
                        out.push_back(static_cast<char>(s[i]));
                }
            }
        }
        if (props) SCRSDK::ReleaseDeviceProperties(handle_, props);
        return out;
    }

    bool readNumericProp(CrInt32u code, std::uint64_t& value) {
        CrDeviceProperty* props = nullptr;
        CrInt32 num = 0;
        CrError err = SCRSDK::GetSelectDeviceProperties(
            handle_, 1, &code, &props, &num);
        bool ok = err == SCRSDK::CrError_None && num > 0 && props;
        if (ok) value = props[0].GetCurrentValue();
        if (props) SCRSDK::ReleaseDeviceProperties(handle_, props);
        return ok;
    }

    Result ensureHostPcStoreDestination() {
        CrDeviceProperty* props = nullptr;
        CrInt32 num = 0;
        CrInt32u code = SCRSDK::CrDeviceProperty_StillImageStoreDestination;
        CrError err = SCRSDK::GetSelectDeviceProperties(
            handle_, 1, &code, &props, &num);
        if (err != SCRSDK::CrError_None || num == 0 || !props) {
            if (props) SCRSDK::ReleaseDeviceProperties(handle_, props);
            return Result::success();  // property unsupported: nothing to do
        }
        std::uint64_t current = props[0].GetCurrentValue();
        bool writable = props[0].IsSetEnableCurrentValue();
        SCRSDK::CrDataType valueType = props[0].GetValueType();
        SCRSDK::ReleaseDeviceProperties(handle_, props);
        if (current == SCRSDK::CrStillImageStoreDestination_HostPC ||
            current == SCRSDK::CrStillImageStoreDestination_HostPCAndMemoryCard)
            return Result::success();
        if (!writable)
            return Result::fail(
                "camera saves stills to the memory card only and the store "
                "destination is not writable in the current mode");

        CrDeviceProperty prop;
        prop.SetCode(code);
        prop.SetValueType(valueType);
        prop.SetCurrentValue(
            SCRSDK::CrStillImageStoreDestination_HostPCAndMemoryCard);
        err = SCRSDK::SetDeviceProperty(handle_, &prop);
        if (err != SCRSDK::CrError_None)
            return Result::fail("cannot set still image store destination: " +
                                crErrorString(err));
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            props = nullptr;
            num = 0;
            err = SCRSDK::GetSelectDeviceProperties(
                handle_, 1, &code, &props, &num);
            if (err == SCRSDK::CrError_None && num > 0 && props) {
                bool applied =
                    props[0].GetCurrentValue() ==
                    SCRSDK::CrStillImageStoreDestination_HostPCAndMemoryCard;
                SCRSDK::ReleaseDeviceProperties(handle_, props);
                if (applied) return Result::success();
            } else if (props) {
                SCRSDK::ReleaseDeviceProperties(handle_, props);
            }
        }
        return Result::fail(
            "camera did not switch the still image store destination to PC");
    }

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
        std::vector<std::uint64_t> raw;
        for (size_t off = 0; off + stride <= byteSize; off += stride) {
            std::uint64_t v = 0;
            std::memcpy(&v, values + off, stride);
            raw.push_back(v);
        }
        if ((prop.GetValueType() & SCRSDK::CrDataType_RangeBit) &&
            raw.size() == 3) {
            // ranges arrive as [min, max, step]
            out.push_back(valueToString(def, raw[0]) + ".." +
                          valueToString(def, raw[1]) + " step " +
                          valueToString(def, raw[2]));
            return out;
        }
        for (std::uint64_t v : raw) out.push_back(valueToString(def, v));
        return out;
    }

    bool sdkInit_ = false;
    bool userDisconnected_ = false;
    bool connecting_ = false;
    SCRSDK::CrSdkControlMode mode_ = SCRSDK::CrSdkControlMode_Remote;
    SCRSDK::ICrCameraObjectInfo* camera_ = nullptr;
    SCRSDK::CrDeviceHandle handle_ = 0;
    Callback callback_;
    std::string model_;
    std::string transport_;
};

}  // namespace

std::unique_ptr<CameraBackend> makeCrsdkBackend(std::string& error) {
    (void)error;
    return std::make_unique<CrsdkBackend>();
}

}  // namespace sonycam

#endif  // SONYCAM_WITH_CRSDK
