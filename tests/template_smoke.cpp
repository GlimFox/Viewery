#include "rename_template.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {

void expect(bool result, const char* description)
{
    if (!result) throw std::runtime_error(description);
}

template<class Action>
void rejects(Action action, const char* description)
{
    try {
        action();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(description);
}

viewery::CollectionSnapshot collection()
{
    viewery::CollectionSnapshot snapshot;
    snapshot.artists = {
        {L"a", L"Artist A", L"Artist A", L"", 0, {{L"source", L"author"}}},
        {L"b", L"Художник Б", L"Художник Б", L"", 0, {}}
    };
    snapshot.artworks = {
        {L"work1", {L"a"}, 1, L"First Light", L"", true, {{L"tag", L"work"}}},
        {L"work2", {L"a"}, 2, L"Second Work", L"", true, {}},
        {L"work3", {L"b"}, 1, L"Рассвет", L"", true, {}},
        {L"joint", {L"a", L"b"}, 4, L"Совместная работа", L"", true, {}}
    };
    for (int number = 1; number <= 8; ++number) {
        const auto id = L"v" + std::to_wstring(number);
        snapshot.versions.push_back({id, L"work1", number, L"", number == 1, {{L"tag", L"version"}}});
        snapshot.files.push_back({L"f" + std::to_wstring(number), L"C:\\media\\original" + std::to_wstring(number) + L".PNG",
            L"original" + std::to_wstring(number) + L".PNG", L".PNG", L"work1", id, 0, 1735689600, {{L"tag", L"file"}}});
    }
    snapshot.versions.push_back({L"second", L"work2", 1, L"", true, {}});
    snapshot.files.push_back({L"second", L"C:\\media\\second.png", L"second.png", L".png", L"work2", L"second", 0, 0, {}});
    snapshot.versions.push_back({L"third", L"work3", 1, L"", true, {}});
    snapshot.files.push_back({L"third", L"C:\\media\\third.png", L"third.png", L".png", L"work3", L"third", 0, 0, {}});
    return snapshot;
}

viewery::RenameTemplate oneBlock(viewery::NameBlockKind kind, std::wstring text = {})
{
    viewery::RenameTemplate preset;
    preset.name = L"test";
    viewery::NameBlock block;
    block.kind = kind;
    block.text = std::move(text);
    preset.blocks.push_back(std::move(block));
    return preset;
}

void checkArtists()
{
    using namespace viewery;
    const auto snapshot = collection();
    auto preset = defaultArtistTemplate();
    const auto names = renderNames(preset, snapshot, snapshot.files);
    expect(names.size() == 10, "All files rendered");
    expect(names[0] == L"Artist A_001_v01_First Light.PNG", "Default pattern uses stored work and version numbers");
    expect(names[7] == L"Artist A_001_v08_First Light.PNG", "Eight versions preserve one stable work number");
    expect(names[8] == L"Artist A_002_v01_Second Work.png", "Second work has a stable independent number");
    expect(names[9] == L"Художник Б_001_v01_Рассвет.png", "Cyrillic data remains Unicode");

    auto file = snapshot.files.front();
    file.versionId.clear();
    expect(renderName(preset, snapshot, file) == L"Artist A_001_First Light.PNG", "Absent optional version does not leave a doubled separator");
    file.artworkId = L"joint";
    expect(renderName(preset, snapshot, file) == L"Artist A & Художник Б_004_Совместная работа.PNG", "Coauthors use the configured joining format");
    file.artworkId = L"missing";
    expect(renderName(preset, snapshot, file) == L"Без автора_Без названия.PNG", "Missing metadata uses configured fallbacks");

    preset = oneBlock(NameBlockKind::customField, L"tag");
    expect(renderName(preset, snapshot, snapshot.files.front()) == L"file.PNG", "File custom fields override inherited values");
    preset.blocks.front().text = L"source";
    expect(renderName(preset, snapshot, snapshot.files.front()) == L"author.PNG", "Artist custom fields are inherited");

    preset = oneBlock(NameBlockKind::original);
    preset.blocks.front().letterCase = NameCase::upper;
    expect(renderName(preset, snapshot, snapshot.files.front()) == L"ORIGINAL1.PNG", "Case conversion preserves the original extension");
    preset = oneBlock(NameBlockKind::date, L"YYYY-MM-DD_hh-mm-ss");
    expect(renderName(preset, snapshot, snapshot.files.front()) == L"2025-01-01_00-00-00.PNG", "Date tokens render the stored UTC timestamp");
}

void checkCounters()
{
    using namespace viewery;
    const auto snapshot = collection();
    auto preset = oneBlock(NameBlockKind::counter);
    preset.blocks.front().digits = 3;
    preset.counterStart = 4;
    auto names = renderNames(preset, snapshot, snapshot.files);
    expect(names[0] == L"004.PNG" && names[8] == L"012.png", "Selection counter honors a custom start");
    preset.counterScope = CounterScope::artist;
    names = renderNames(preset, snapshot, snapshot.files);
    expect(names[8] == L"012.png" && names[9] == L"004.png", "Artist counter resets only at an artist group");
    preset.counterScope = CounterScope::artwork;
    names = renderNames(preset, snapshot, snapshot.files);
    expect(names[7] == L"011.PNG" && names[8] == L"004.png", "Artwork counter restarts independently of stable work numbers");
    rejects([&] { renderName(preset, snapshot, snapshot.files.front(), 0); }, "A zero ordinal is rejected");
}

void checkWindowsNames()
{
    using namespace viewery;
    const auto snapshot = collection();
    auto preset = oneBlock(NameBlockKind::literal, L"CON");
    expect(renderName(preset, snapshot, snapshot.files.front()) == L"_CON.PNG", "Reserved Windows basename is repaired");
    preset.blocks.front().text = L"abc<>:\"/\\|?*def. ";
    const auto name = renderName(preset, snapshot, snapshot.files.front());
    expect(name.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos && name.ends_with(L"def.PNG"),
           "Invalid Windows characters and trailing stem punctuation are handled");
    preset.blocks.front().text = L"  first\t  second  ";
    expect(renderName(preset, snapshot, snapshot.files.front()) == L"first second.PNG", "Whitespace normalization is configurable");
    preset.blocks.front().text = std::wstring(252, L'x') + L"\U0001F98A";
    const auto longName = renderName(preset, snapshot, snapshot.files.front());
    expect(longName.size() <= 255 && longName.ends_with(L".PNG"), "Long generated filenames fit one Windows component");
    expect(longName.size() < 5 || longName[longName.size() - 5] < 0xd800 || longName[longName.size() - 5] > 0xdbff,
           "Long filename truncation never leaves an unpaired high surrogate");
}

void checkJson()
{
    using namespace viewery;
    const auto snapshot = collection();
    auto preset = defaultArtistTemplate();
    preset.name = L"Кириллица, кавычки \" и \\ пути";
    preset.authorSeparator = L" + ";
    preset.counterStart = 7;
    preset.counterScope = CounterScope::artwork;
    preset.blocks.front().letterCase = NameCase::lower;
    const auto encoded = serializeTemplate(preset);
    const auto decoded = parseTemplate(encoded);
    expect(decoded.name == preset.name && decoded.counterStart == 7 && decoded.counterScope == CounterScope::artwork,
           "JSON roundtrip preserves preset configuration");
    expect(renderName(decoded, snapshot, snapshot.files.front()) == renderName(preset, snapshot, snapshot.files.front()),
           "JSON roundtrip preserves generated filenames");
    rejects([] { parseTemplate("{}"); }, "Missing schema rejected");
    rejects([] { parseTemplate("{\"vieweryRenamePreset\":4294967297,\"blocks\":[{\"kind\":\"artist\"}]}"); },
            "Oversized future schema cannot wrap to supported version");
    rejects([] { parseTemplate("{\"vieweryRenamePreset\":1,\"blocks\":[{\"kind\":\"counter\",\"digits\":4294967296}]}"); },
            "Oversized digit count cannot wrap to zero");
    rejects([] { parseTemplate("{\"vieweryRenamePreset\":1,\"blocks\":[{\"kind\":\"futureKind\"}]}"); },
            "Unknown template block rejected");
    rejects([] { parseTemplate(std::string(1024 * 1024 + 1, 'x')); }, "Large JSON input rejected");
    rejects([] { parseTemplate("{\"vieweryRenamePreset\":1,\"blocks\":[{\"kind\":\"artist\",\"omitIfEmpty\":1}]}"); },
            "Wrong parameter types rejected");
}

} // namespace

int main()
{
    try {
        checkArtists();
        checkCounters();
        checkWindowsNames();
        checkJson();
        std::cout << "Rename template checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
