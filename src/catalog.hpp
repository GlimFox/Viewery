#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace viewery {

enum class CatalogSort { name, newest, size, type };

struct CatalogFile {
    std::filesystem::path path;
    std::wstring name;
    std::wstring extension;
    std::uint64_t bytes = 0;
    FILETIME created{};
    FILETIME modified{};
};

using Thumbnail = std::shared_ptr<std::remove_pointer_t<HBITMAP>>;

struct CatalogUpdate {
    unsigned generation = 0;
    bool scanned = false;
    std::vector<CatalogFile> files;
    std::vector<std::pair<std::filesystem::path, Thumbnail>> images;
};

class CatalogWorker {
public:
    CatalogWorker(HWND window, UINT message);
    ~CatalogWorker();

    void scan(std::filesystem::path directory, unsigned generation);
    void request(std::vector<std::filesystem::path> paths, unsigned generation);
    CatalogUpdate take();

private:
    void run(std::stop_token stop);

    HWND window_;
    UINT message_;
    std::mutex mutex_;
    std::condition_variable_any work_;
    std::filesystem::path directory_;
    unsigned generation_ = 0;
    bool scanPending_ = false;
    std::vector<std::filesystem::path> needed_;
    CatalogUpdate update_;
    std::jthread thread_;
};

void sortCatalog(std::vector<CatalogFile>& files, CatalogSort sort);

}
