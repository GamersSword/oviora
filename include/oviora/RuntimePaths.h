#pragma once

#include <filesystem>

namespace oviora::runtime {

std::filesystem::path applicationRoot();
std::filesystem::path shaderDirectory();
std::filesystem::path localRuntimeDirectory();
std::filesystem::path localDataDirectory();
std::filesystem::path logsDirectory();

} // namespace oviora::runtime
