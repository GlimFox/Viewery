#pragma once

#include <filesystem>
#include <string_view>

namespace viewery::runtime {

std::filesystem::path executableDirectory();
std::filesystem::path userDataDirectory();
std::filesystem::path mediaToolsDirectory();
std::filesystem::path mediaToolPath(std::wstring_view filename);

}
