#pragma once
#include <windows.h>
#include <filesystem>
#include <vector>

namespace viewery {
struct ExplorerSelection {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> files;
};
// Pure planning: Windows case-insensitive deduplication, one selection per folder.
std::vector<ExplorerSelection> planExplorerSelection(const std::vector<std::filesystem::path>& files);
HRESULT revealInExplorer(const std::vector<std::filesystem::path>& files);
}
