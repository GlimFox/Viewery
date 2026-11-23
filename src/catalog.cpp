#include "catalog.hpp"
#include "media.hpp"

#include <shobjidl.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

namespace viewery {
namespace fs = std::filesystem;

namespace {

std::vector<CatalogFile> listFiles(const fs::path& directory, std::stop_token stop)
{
    std::vector<CatalogFile> files;
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileExW((directory / L"*").c_str(), FindExInfoBasic, &entry,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE) return files;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        fs::path path = directory / entry.cFileName;
        if (!classifyMedia(path)) continue;
        const std::uint64_t bytes = (static_cast<std::uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
        std::wstring extension = path.extension().wstring();
        files.push_back({std::move(path), entry.cFileName, std::move(extension), bytes,
                         entry.ftCreationTime, entry.ftLastWriteTime});
    } while (!stop.stop_requested() && FindNextFileW(search, &entry));
    FindClose(search);
    return files;
}

Thumbnail thumbnail(const fs::path& path)
{
    Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return {};
    HBITMAP bitmap = nullptr;
    if (FAILED(factory->GetImage({200, 128}, SIIGBF_THUMBNAILONLY, &bitmap))) return {};
    return Thumbnail(bitmap, [](HBITMAP image) { DeleteObject(image); });
}

}

CatalogWorker::CatalogWorker(HWND window, UINT message)
    : window_(window), message_(message), thread_([this](std::stop_token stop) { run(stop); })
{
}

CatalogWorker::~CatalogWorker() = default;

void CatalogWorker::scan(fs::path directory, unsigned generation)
{
    std::lock_guard guard(mutex_);
    directory_ = std::move(directory);
    generation_ = generation;
    needed_.clear();
    update_ = {};
    update_.generation = generation;
    scanPending_ = true;
    work_.notify_one();
}

void CatalogWorker::request(std::vector<fs::path> paths, unsigned generation)
{
    std::lock_guard guard(mutex_);
    if (generation != generation_) return;
    needed_ = std::move(paths);
    std::reverse(needed_.begin(), needed_.end());
    work_.notify_one();
}

CatalogUpdate CatalogWorker::take()
{
    std::lock_guard guard(mutex_);
    return std::exchange(update_, CatalogUpdate{generation_, false, {}, {}});
}

void CatalogWorker::run(std::stop_token stop)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        fs::path directory;
        fs::path image;
        unsigned generation;
        {
            std::unique_lock lock(mutex_);
            if (!work_.wait(lock, stop, [this] { return scanPending_ || !needed_.empty(); })) break;
            if (stop.stop_requested()) break;
            generation = generation_;
            if (scanPending_) {
                directory = directory_;
                scanPending_ = false;
            } else {
                image = std::move(needed_.back());
                needed_.pop_back();
            }
        }
        if (!directory.empty()) {
            auto files = listFiles(directory, stop);
            std::lock_guard guard(mutex_);
            if (generation != generation_) continue;
            update_.files = std::move(files);
            update_.scanned = true;
        } else {
            Thumbnail bitmap = SUCCEEDED(com) ? thumbnail(image) : Thumbnail{};
            std::lock_guard guard(mutex_);
            if (generation != generation_) continue;
            update_.images.emplace_back(std::move(image), std::move(bitmap));
        }
        PostMessageW(window_, message_, 0, 0);
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

void sortCatalog(std::vector<CatalogFile>& files, CatalogSort sort)
{
    std::ranges::sort(files, [sort](const CatalogFile& left, const CatalogFile& right) {
        if (sort == CatalogSort::newest) {
            const LONG result = CompareFileTime(&left.modified, &right.modified);
            if (result != 0) return result > 0;
        }
        if (sort == CatalogSort::size && left.bytes != right.bytes) return left.bytes > right.bytes;
        if (sort == CatalogSort::type) {
            const int result = CompareStringOrdinal(left.extension.c_str(), -1, right.extension.c_str(), -1, TRUE);
            if (result != CSTR_EQUAL) return result == CSTR_LESS_THAN;
        }
        const int natural = StrCmpLogicalW(left.name.c_str(), right.name.c_str());
        return natural != 0 ? natural < 0 : left.name < right.name;
    });
}

}
