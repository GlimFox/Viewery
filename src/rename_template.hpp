#pragma once

#include "library.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace viewery {

enum class NameBlockKind {
    artist, workNumber, title, version, versionTitle, date, original,
    mediaType, literal, customField, counter, separator
};

enum class NameCase { preserve, lower, upper };
enum class CounterScope { selection, artist, artwork };

struct NameBlock {
    NameBlockKind kind = NameBlockKind::artist;
    std::wstring text;
    std::wstring fallback;
    std::wstring prefix;
    std::wstring suffix;
    int digits = 0;
    NameCase letterCase = NameCase::preserve;
    bool omitIfEmpty = true;
};

struct RenameTemplate {
    LibraryId id;
    std::wstring name;
    std::vector<NameBlock> blocks;
    std::wstring authorSeparator = L" & ";
    std::int64_t counterStart = 1;
    CounterScope counterScope = CounterScope::selection;
    bool collapseWhitespace = true;
};

RenameTemplate defaultArtistTemplate();
std::wstring_view nameBlockLabel(NameBlockKind kind);

// sequence is a one-based ordinal for this selected set or its counter group.
// Stored work/version numbers are independent of the sequential counter.
std::wstring renderName(const RenameTemplate& preset, const CollectionSnapshot& collection,
                        const MediaRecord& file, std::int64_t sequence = 1);

std::vector<std::wstring> renderNames(const RenameTemplate& preset,
                                     const CollectionSnapshot& collection,
                                     const std::vector<MediaRecord>& files);

// UTF-8 JSON document with a versioned schema. Parsing throws invalid_argument
// for unsupported schemas and invalid types, limits or block parameters.
std::string serializeTemplate(const RenameTemplate& preset);
RenameTemplate parseTemplate(std::string_view json);

} // namespace viewery
