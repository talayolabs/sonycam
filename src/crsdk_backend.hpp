#pragma once

#include "backend.hpp"

#include <memory>

namespace sonycam {

// Factory for the real Sony Camera Remote SDK backend.
// Only available when the project is built with -DSONY_SDK_DIR=...;
// returns nullptr (with an explanatory message in `error`) otherwise.
std::unique_ptr<CameraBackend> makeCrsdkBackend(std::string& error);

}  // namespace sonycam
