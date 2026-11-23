#pragma once

#include "library.hpp"

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace viewery {

enum class CollectionSort { artist, number, title, version, name, date, size, extension, custom };
enum class CollectionGroup { none, artist, artwork, format, directory };

struct CollectionFilter {
    std::wstring text, artist, title, version, format;
    bool unassigned = false;
    bool untitled = false;
    bool unorganized = false;
};

struct CollectionViewOptions {
    CollectionFilter filter;
    CollectionSort sort = CollectionSort::name;
    CollectionGroup group = CollectionGroup::none;
    bool descending = false;
    std::set<std::wstring> collapsed;
};

struct CollectionEntry {
    bool group = false;
    std::wstring key, label;
    std::vector<std::wstring> ancestorKeys;
    LibraryId mediaId;
    std::filesystem::path path;
    std::size_t files = 0, artworks = 0, versions = 0;
};

struct CollectionViewResult {
    std::vector<CollectionEntry> rows;
    std::size_t fileCount = 0, artworkCount = 0, versionCount = 0;
    std::size_t artistCount = 0, additionalVersions = 0;
};

// Paths define the current eligible file set and its custom order. Metadata is
// optional. Unindexed files remain visible with an empty mediaId and their path.
// Group counters describe all matching descendants, including collapsed rows.
CollectionViewResult buildCollectionView(const CollectionSnapshot& collection,
                                         const std::vector<std::filesystem::path>& paths,
                                         const CollectionViewOptions& options = {});

} // namespace viewery
