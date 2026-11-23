#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace viewery {

using LibraryId = std::wstring;
using Metadata = std::map<std::wstring, std::wstring>;

struct Artist {
    LibraryId id;
    std::wstring name, fileName, aliases;
    std::int64_t order = 0;
    Metadata fields;
};

struct Artwork {
    LibraryId id;
    std::vector<LibraryId> artists;
    std::int64_t number = 0;
    std::wstring title, userTitle;
    bool standalone = true;
    Metadata fields;
};

struct MediaVersion {
    LibraryId id, artworkId;
    std::int64_t number = 1;
    std::wstring title;
    bool main = true;
    Metadata fields;
};

struct MediaRecord {
    LibraryId id;
    std::filesystem::path path;
    std::wstring originalName, extension;
    LibraryId artworkId, versionId;
    std::uint64_t bytes = 0;
    std::int64_t modified = 0; // Unix seconds, UTC.
    Metadata fields;
};

struct CollectionSnapshot {
    std::vector<Artist> artists;
    std::vector<Artwork> artworks;
    std::vector<MediaVersion> versions;
    std::vector<MediaRecord> files;
};

struct LibraryPreset {
    LibraryId id;
    std::wstring name, json;
};

struct RenameProposal {
    LibraryId fileId;
    std::filesystem::path source;
    std::wstring newName;
    bool enabled = true;
};

struct RenamePreview {
    RenameProposal proposal;
    std::filesystem::path target;
    std::wstring problem;
    bool ready = false;
};

struct RenameOperation {
    LibraryId id;
    std::wstring created, status, message;
    std::size_t count = 0;
    bool canUndo = false;
};

struct RenameResult {
    bool success = false, cancelled = false;
    LibraryId operationId;
    std::wstring message;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> changes;
};

class LibraryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::filesystem::path defaultDatabasePath();
std::wstring sanitizeWindowsName(std::wstring_view value);
std::vector<RenamePreview> previewRenames(const std::vector<RenameProposal>& proposals);

// Construct only when the collection module is used. No file is created at
// static initialization. Calls on one store are serialized internally.
class CollectionStore {
public:
    explicit CollectionStore(std::filesystem::path database = defaultDatabasePath());
    ~CollectionStore();
    CollectionStore(const CollectionStore&) = delete;
    CollectionStore& operator=(const CollectionStore&) = delete;

    CollectionSnapshot snapshot() const;
    std::vector<MediaRecord> indexFiles(const std::vector<std::filesystem::path>& paths,
                                      std::stop_token cancellation = {});
    LibraryId saveArtist(Artist artist);
    LibraryId saveArtwork(Artwork artwork);
    LibraryId saveVersion(MediaVersion version);
    void saveFile(MediaRecord file);
    void assignFiles(const std::vector<LibraryId>& files, const LibraryId& artwork,
                     const LibraryId& version);
    std::int64_t nextArtworkNumber(const LibraryId& artist = {}) const;
    std::int64_t nextVersionNumber(const LibraryId& artwork) const;

    LibraryId savePreset(LibraryPreset preset);
    std::vector<LibraryPreset> presets() const;
    void deletePreset(const LibraryId& preset);
    std::wstring setting(std::wstring_view key, std::wstring_view fallback = {}) const;
    void setSetting(std::wstring_view key, std::wstring_view value);
    void backup(const std::filesystem::path& output) const;
    void exportMetadata(const std::filesystem::path& output) const;
    void importMetadata(const std::filesystem::path& input);

    RenameResult rename(const std::vector<RenameProposal>& proposals, std::stop_token cancellation = {});
    RenameResult undo(const LibraryId& operation, std::stop_token cancellation = {});
    RenameResult recover(const LibraryId& operation, std::stop_token cancellation = {});
    std::vector<RenameOperation> history() const;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> operationPaths(const LibraryId& operation) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace viewery
