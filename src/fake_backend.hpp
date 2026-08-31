#pragma once

#include "backend.hpp"

#include <map>

namespace sonycam {

// In-memory simulated camera. Same property surface as the CrSDK backend,
// used for development, tests and CI without hardware or the Sony SDK.
class FakeBackend : public CameraBackend {
public:
    FakeBackend();

    Result connect() override;
    Result disconnect() override;
    CameraInfo info() override;

    Result gearInfo(std::vector<PropInfo>& out) override;
    Result listProps(std::vector<PropInfo>& out) override;
    Result getProp(const std::string& name, PropInfo& out) override;
    Result setProp(const std::string& name, const std::string& value) override;

    Result focus(const std::string& op, int steps, std::string& outStatus) override;
    Result capture(const std::string& saveDir, std::string& outFile) override;
    Result liveviewFrame(const std::string& path) override;

private:
    bool connected_ = false;
    int captureCount_ = 0;
    struct FakeProp {
        std::string value;
        std::vector<std::string> choices;  // empty => free-form numeric
        bool writable = true;
    };
    std::map<std::string, FakeProp> props_;
};

}  // namespace sonycam
