#include "rename_template.hpp"
#include "media_types.hpp"
#include "third_party/json.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <limits>
#include <map>
#include <stdexcept>

namespace viewery {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maxBlocks = 64;
constexpr std::size_t maxText = 4096;
constexpr std::int64_t maxCounter = 999999999999999;
constexpr std::array<std::string_view, 12> blockKinds{
    "artist", "workNumber", "title", "version", "versionTitle", "date", "original",
    "mediaType", "literal", "customField", "counter", "separator"
};

std::string utf8(std::wstring_view text)
{
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
                                       nullptr, 0, nullptr, nullptr);
    if (!size) throw std::invalid_argument("Invalid Unicode in rename template");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf16(const std::string& text)
{
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (!size) throw std::invalid_argument("Invalid UTF-8 in rename template");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), size);
    return result;
}

void validate(const RenameTemplate& preset)
{
    if (preset.blocks.empty() || preset.blocks.size() > maxBlocks || preset.name.size() > maxText ||
        preset.id.size() > maxText || preset.authorSeparator.size() > 256 ||
        preset.counterStart < 0 || preset.counterStart > maxCounter ||
        static_cast<unsigned>(preset.counterScope) > static_cast<unsigned>(CounterScope::artwork)) {
        throw std::invalid_argument("Invalid rename template parameters");
    }
    for (const auto& block : preset.blocks) {
        if (static_cast<unsigned>(block.kind) >= blockKinds.size() || block.digits < 0 || block.digits > 18 ||
            static_cast<unsigned>(block.letterCase) > static_cast<unsigned>(NameCase::upper) ||
            block.text.size() > maxText || block.fallback.size() > maxText ||
            block.prefix.size() > maxText || block.suffix.size() > maxText) {
            throw std::invalid_argument("Invalid rename template block");
        }
    }
}

template<class Record>
const Record* byId(const std::vector<Record>& records, const LibraryId& id)
{
    const auto found = std::ranges::find(records, id, &Record::id);
    return found == records.end() ? nullptr : &*found;
}

std::wstring number(std::int64_t value, int digits)
{
    auto text = std::to_wstring(value);
    if (text.size() < static_cast<std::size_t>(digits)) text.insert(0, static_cast<std::size_t>(digits) - text.size(), L'0');
    return text;
}

std::wstring changeCase(std::wstring_view text, NameCase letterCase)
{
    if (text.empty() || letterCase == NameCase::preserve) return std::wstring(text);
    const DWORD flags = letterCase == NameCase::lower ? LCMAP_LOWERCASE : LCMAP_UPPERCASE;
    const int length = static_cast<int>(text.size());
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, flags, text.data(), length, nullptr, 0, nullptr, nullptr, 0);
    if (!size) throw std::invalid_argument("Cannot transform Unicode text case");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, flags, text.data(), length, result.data(), size, nullptr, nullptr, 0);
    return result;
}

std::wstring dateValue(std::int64_t unixSeconds, std::wstring format)
{
    constexpr std::int64_t fileTimeEpoch = 11644473600LL;
    if (unixSeconds < -fileTimeEpoch || unixSeconds > 253402300799LL) return {};
    const auto ticks = static_cast<std::uint64_t>(unixSeconds + fileTimeEpoch) * 10000000ULL;
    const FILETIME fileTime{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME date{};
    if (!FileTimeToSystemTime(&fileTime, &date)) return {};
    if (format.empty()) format = L"YYYY-MM-DD";
    const std::array replacements{
        std::pair{L"YYYY", number(date.wYear, 4)}, std::pair{L"MM", number(date.wMonth, 2)},
        std::pair{L"DD", number(date.wDay, 2)}, std::pair{L"hh", number(date.wHour, 2)},
        std::pair{L"mm", number(date.wMinute, 2)}, std::pair{L"ss", number(date.wSecond, 2)}
    };
    for (const auto& [key, value] : replacements) {
        std::size_t offset = 0;
        const std::wstring_view token(key);
        while ((offset = format.find(token, offset)) != std::wstring::npos) {
            format.replace(offset, token.size(), value);
            offset += value.size();
        }
    }
    return format;
}

std::wstring customValue(const std::wstring& key, const MediaRecord& file, const MediaVersion* version,
                         const Artwork* work, const CollectionSnapshot& collection)
{
    const auto find = [&key](const Metadata& fields) -> const std::wstring* {
        const auto value = fields.find(key);
        return value == fields.end() ? nullptr : &value->second;
    };
    if (const auto value = find(file.fields)) return *value;
    if (version) if (const auto value = find(version->fields)) return *value;
    if (work) {
        if (const auto value = find(work->fields)) return *value;
        for (const auto& id : work->artists) {
            if (const auto* artist = byId(collection.artists, id)) {
                if (const auto value = find(artist->fields)) return *value;
            }
        }
    }
    return {};
}

std::wstring blockValue(const NameBlock& block, const RenameTemplate& preset,
                        const CollectionSnapshot& collection, const MediaRecord& file,
                        const Artwork* work, const MediaVersion* version, std::int64_t sequence)
{
    switch (block.kind) {
    case NameBlockKind::artist: {
        std::wstring authors;
        if (work) for (const auto& id : work->artists) {
            if (const auto* artist = byId(collection.artists, id)) {
                const auto& name = artist->fileName.empty() ? artist->name : artist->fileName;
                if (name.empty()) continue;
                if (!authors.empty()) authors += preset.authorSeparator;
                authors += name;
            }
        }
        return authors;
    }
    case NameBlockKind::workNumber:
        return work && work->number > 0 ? number(work->number, block.digits) : std::wstring{};
    case NameBlockKind::title:
        return work ? (work->userTitle.empty() ? work->title : work->userTitle) : std::wstring{};
    case NameBlockKind::version:
        return version && version->number > 0 ? number(version->number, block.digits) : std::wstring{};
    case NameBlockKind::versionTitle:
        return version ? version->title : std::wstring{};
    case NameBlockKind::date:
        return dateValue(file.modified, block.text);
    case NameBlockKind::original:
        return file.originalName.empty() ? file.path.stem().wstring() : std::filesystem::path(file.originalName).stem().wstring();
    case NameBlockKind::mediaType:
        if (const auto kind = classifyMedia(file.path)) {
            switch (*kind) {
            case MediaKind::image: return L"Фото";
            case MediaKind::gif: return L"GIF";
            case MediaKind::video: return L"Видео";
            }
        }
        return L"Файл";
    case NameBlockKind::customField:
        return customValue(block.text, file, version, work, collection);
    case NameBlockKind::literal:
    case NameBlockKind::separator:
        return block.text;
    case NameBlockKind::counter:
        return number(preset.counterStart + sequence - 1, block.digits);
    }
    return {};
}

std::wstring collapsed(std::wstring_view value)
{
    std::wstring result;
    bool pendingSpace = false;
    for (const wchar_t character : value) {
        if (std::iswspace(character)) {
            pendingSpace = !result.empty();
        } else {
            if (pendingSpace) result.push_back(L' ');
            result.push_back(character);
            pendingSpace = false;
        }
    }
    return result;
}

std::wstring safeFilename(std::wstring stem, const std::wstring& extension)
{
    if (extension.size() > 254 || extension.find_first_of(L"/\\:\0", 0, 4) != std::wstring::npos) {
        throw std::invalid_argument("Invalid source extension");
    }
    // Windows excludes trailing dots/spaces from the basename even when an
    // extension follows. Trim those before joining the preserved extension.
    while (!stem.empty() && (stem.back() == L'.' || stem.back() == L' ')) stem.pop_back();
    if (stem.empty()) stem = L"Без имени";
    const auto limit = static_cast<std::size_t>(254) - extension.size();
    if (stem.size() > limit) {
        stem.resize(limit);
        if (!stem.empty() && stem.back() >= 0xd800 && stem.back() <= 0xdbff) stem.pop_back();
    }
    return sanitizeWindowsName(stem + extension);
}

std::wstring scopeKey(const RenameTemplate& preset, const CollectionSnapshot& collection, const MediaRecord& file)
{
    if (preset.counterScope == CounterScope::selection) return {};
    if (preset.counterScope == CounterScope::artwork) return file.artworkId;
    const auto* work = byId(collection.artworks, file.artworkId);
    if (!work) return {};
    auto artists = work->artists;
    std::ranges::sort(artists);
    std::wstring key;
    for (const auto& artist : artists) {
        key += std::to_wstring(artist.size()) + L":" + artist;
    }
    return key;
}

std::wstring getText(const Json& object, const char* key, std::wstring_view fallback = {})
{
    const auto field = object.find(key);
    if (field == object.end()) return std::wstring(fallback);
    if (!field->is_string()) throw std::invalid_argument("Template field must be a string");
    const auto& value = field->get_ref<const std::string&>();
    if (value.size() > maxText * 4) throw std::invalid_argument("Template string is too long");
    auto result = utf16(value);
    if (result.size() > maxText) throw std::invalid_argument("Template string is too long");
    return result;
}

} // namespace

RenameTemplate defaultArtistTemplate()
{
    RenameTemplate preset;
    preset.id = L"artist-work-version";
    preset.name = L"Художник · работа · версия";
    NameBlock artist;
    artist.kind = NameBlockKind::artist;
    artist.fallback = L"Без автора";
    NameBlock separator;
    separator.kind = NameBlockKind::separator;
    separator.text = L"_";
    NameBlock work;
    work.kind = NameBlockKind::workNumber;
    work.digits = 3;
    NameBlock version;
    version.kind = NameBlockKind::version;
    version.digits = 2;
    version.prefix = L"v";
    NameBlock title;
    title.kind = NameBlockKind::title;
    title.fallback = L"Без названия";
    preset.blocks = {artist, separator, work, separator, version, separator, title};
    return preset;
}

std::wstring_view nameBlockLabel(NameBlockKind kind)
{
    static constexpr std::array labels{L"Художник", L"Номер работы", L"Название работы", L"Версия",
        L"Название версии", L"Дата", L"Исходное имя", L"Тип медиа", L"Текст", L"Своё поле", L"Счётчик", L"Разделитель"};
    const auto index = static_cast<std::size_t>(kind);
    return index < labels.size() ? labels[index] : L"Неизвестный блок";
}

std::wstring renderName(const RenameTemplate& preset, const CollectionSnapshot& collection,
                        const MediaRecord& file, std::int64_t sequence)
{
    validate(preset);
    if (sequence < 1 || sequence > maxCounter || preset.counterStart > maxCounter - sequence + 1) {
        throw std::invalid_argument("Rename counter is out of range");
    }
    const auto* work = byId(collection.artworks, file.artworkId);
    const auto* version = byId(collection.versions, file.versionId);
    if (version && (!work || version->artworkId != work->id)) version = nullptr;
    std::wstring result;
    std::wstring separator;
    for (const auto& block : preset.blocks) {
        auto value = blockValue(block, preset, collection, file, work, version, sequence);
        if (block.kind == NameBlockKind::separator) {
            if (!value.empty()) separator = std::move(value);
            continue;
        }
        if (value.empty()) value = block.fallback;
        if (value.empty() && block.omitIfEmpty) continue;
        value = block.prefix + changeCase(value, block.letterCase) + block.suffix;
        if (value.empty()) continue;
        if (!result.empty()) result += separator;
        result += value;
        separator.clear();
    }
    if (preset.collapseWhitespace) result = collapsed(result);
    const auto extension = file.extension.empty() ? file.path.extension().wstring() : file.extension;
    return safeFilename(std::move(result), extension);
}

std::vector<std::wstring> renderNames(const RenameTemplate& preset, const CollectionSnapshot& collection,
                                     const std::vector<MediaRecord>& files)
{
    std::map<std::wstring, std::int64_t> counters;
    std::vector<std::wstring> result;
    result.reserve(files.size());
    for (const auto& file : files) result.push_back(renderName(preset, collection, file,
        ++counters[scopeKey(preset, collection, file)]));
    return result;
}

std::string serializeTemplate(const RenameTemplate& preset)
{
    validate(preset);
    Json blocks = Json::array();
    static constexpr std::array cases{"preserve", "lower", "upper"};
    static constexpr std::array scopes{"selection", "artist", "artwork"};
    for (const auto& block : preset.blocks) {
        blocks.push_back({{"kind", blockKinds[static_cast<std::size_t>(block.kind)]},
            {"text", utf8(block.text)}, {"fallback", utf8(block.fallback)}, {"prefix", utf8(block.prefix)},
            {"suffix", utf8(block.suffix)}, {"digits", block.digits},
            {"case", cases[static_cast<std::size_t>(block.letterCase)]}, {"omitIfEmpty", block.omitIfEmpty}});
    }
    return Json{{"vieweryRenamePreset", 1}, {"id", utf8(preset.id)}, {"name", utf8(preset.name)},
        {"blocks", std::move(blocks)}, {"authorSeparator", utf8(preset.authorSeparator)},
        {"counterStart", preset.counterStart}, {"counterScope", scopes[static_cast<std::size_t>(preset.counterScope)]},
        {"collapseWhitespace", preset.collapseWhitespace}}.dump(2);
}

RenameTemplate parseTemplate(std::string_view document)
{
    if (document.size() > 1024 * 1024) throw std::invalid_argument("Rename template JSON is too large");
    try {
        const Json root = Json::parse(document);
        if (!root.is_object() || !root.contains("vieweryRenamePreset") || !root["vieweryRenamePreset"].is_number_integer() ||
            root["vieweryRenamePreset"].get<std::int64_t>() != 1 || !root.contains("blocks") || !root["blocks"].is_array() ||
            root["blocks"].empty() || root["blocks"].size() > maxBlocks) {
            throw std::invalid_argument("Unsupported rename template schema");
        }
        RenameTemplate result;
        result.id = getText(root, "id");
        result.name = getText(root, "name", L"Импортированный шаблон");
        result.authorSeparator = getText(root, "authorSeparator", L" & ");
        if (root.contains("counterStart")) {
            if (!root["counterStart"].is_number_integer()) throw std::invalid_argument("Counter start must be an integer");
            result.counterStart = root["counterStart"].get<std::int64_t>();
        }
        const auto scope = getText(root, "counterScope", L"selection");
        if (scope == L"artist") result.counterScope = CounterScope::artist;
        else if (scope == L"artwork") result.counterScope = CounterScope::artwork;
        else if (scope != L"selection") throw std::invalid_argument("Unknown counter scope");
        if (root.contains("collapseWhitespace")) {
            if (!root["collapseWhitespace"].is_boolean()) throw std::invalid_argument("Whitespace setting must be boolean");
            result.collapseWhitespace = root["collapseWhitespace"].get<bool>();
        }
        for (const auto& item : root["blocks"]) {
            if (!item.is_object()) throw std::invalid_argument("A rename block must be an object");
            const auto kind = getText(item, "kind");
            const auto code = utf8(kind);
            const auto found = std::ranges::find(blockKinds, code);
            if (found == blockKinds.end()) throw std::invalid_argument("Unknown rename block kind");
            NameBlock block;
            block.kind = static_cast<NameBlockKind>(found - blockKinds.begin());
            block.text = getText(item, "text");
            block.fallback = getText(item, "fallback");
            block.prefix = getText(item, "prefix");
            block.suffix = getText(item, "suffix");
            if (item.contains("digits")) {
                if (!item["digits"].is_number_integer()) throw std::invalid_argument("Padding digits must be integer");
                const auto digits = item["digits"].get<std::int64_t>();
                if (digits < 0 || digits > 18) throw std::invalid_argument("Padding digits are out of range");
                block.digits = static_cast<int>(digits);
            }
            const auto letterCase = getText(item, "case", L"preserve");
            if (letterCase == L"lower") block.letterCase = NameCase::lower;
            else if (letterCase == L"upper") block.letterCase = NameCase::upper;
            else if (letterCase != L"preserve") throw std::invalid_argument("Unknown letter case");
            if (item.contains("omitIfEmpty")) {
                if (!item["omitIfEmpty"].is_boolean()) throw std::invalid_argument("Omit-empty setting must be boolean");
                block.omitIfEmpty = item["omitIfEmpty"].get<bool>();
            }
            result.blocks.push_back(std::move(block));
        }
        validate(result);
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("Invalid rename template JSON: ") + error.what());
    }
}

} // namespace viewery
