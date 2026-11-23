#include "collection_view.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {

using namespace viewery;

void expect(bool result, const char* description)
{
    if (!result) throw std::runtime_error(description);
}

struct Fixture {
    CollectionSnapshot snapshot;
    std::vector<std::filesystem::path> paths;
};

Fixture collection()
{
    Fixture fixture;
    fixture.snapshot.artists = {
        {L"a", L"Artist A", L"", L"Лис, Fox", 0, {}},
        {L"b", L"Художник Б", L"", L"Bird", 1, {}}
    };
    fixture.snapshot.artworks = {
        {L"a1", {L"a"}, 1, L"First Light", L"", true, {}},
        {L"a2", {L"a"}, 2, L"Second Work", L"", true, {}},
        {L"a3", {L"a"}, 3, L"Third Work", L"", true, {}}
    };
    for (int index = 0; index < 20; ++index) {
        std::wstring work;
        if (index < 8) work = L"a1";
        else if (index == 8) work = L"a2";
        else if (index == 9) work = L"a3";
        else {
            work = L"b" + std::to_wstring(index);
            fixture.snapshot.artworks.push_back({work, {L"b"}, index - 9, L"Работа " + std::to_wstring(index - 9), L"", true, {}});
        }
        const auto version = L"version-" + std::to_wstring(index);
        fixture.snapshot.versions.push_back({version, work, index < 8 ? index + 1 : 1, L"", index != 1, {}});
        MediaRecord file;
        file.id = L"file-" + std::to_wstring(index);
        file.path = L"C:\\media\\photo" + std::to_wstring(index + 1) + L".png";
        file.originalName = file.path.filename().wstring();
        file.extension = L".png";
        file.artworkId = work;
        file.versionId = version;
        file.bytes = static_cast<std::uint64_t>(index * 17);
        file.modified = index;
        fixture.paths.push_back(file.path);
        fixture.snapshot.files.push_back(std::move(file));
    }
    return fixture;
}

const CollectionEntry* groupFor(const CollectionViewResult& view, std::wstring_view prefix)
{
    const auto found = std::ranges::find_if(view.rows, [prefix](const CollectionEntry& row) {
        return row.group && row.label.starts_with(prefix);
    });
    return found == view.rows.end() ? nullptr : &*found;
}

std::vector<std::wstring> leafNames(const CollectionViewResult& view)
{
    std::vector<std::wstring> names;
    for (const auto& row : view.rows) if (!row.group) names.push_back(row.path.filename().wstring());
    return names;
}

void checkGroups()
{
    auto fixture = collection();
    CollectionViewOptions options;
    options.group = CollectionGroup::artist;
    auto view = buildCollectionView(fixture.snapshot, fixture.paths, options);
    expect(view.fileCount == 20 && view.artworkCount == 13 && view.additionalVersions == 7,
           "Global counters match 20 files, 13 works and seven extra versions");
    const auto* artist = groupFor(view, L"Artist A");
    expect(artist && artist->files == 10 && artist->artworks == 3,
           "First artist has three unique works and ten files");
    const auto* work = groupFor(view, L"001 · First Light");
    expect(work && work->versions == 8 && work->files == 8,
           "First work has eight versions including the main version");
    expect(std::ranges::any_of(view.rows, [](const CollectionEntry& row) {
        return !row.group && row.ancestorKeys.size() == 3 && !row.mediaId.empty();
    }), "Artist to work to version to file hierarchy is present");

    const auto artistKey = artist->key;
    options.collapsed.insert(artistKey);
    view = buildCollectionView(fixture.snapshot, fixture.paths, options);
    expect(view.fileCount == 20 && groupFor(view, L"Artist A")->files == 10 &&
           std::ranges::none_of(view.rows, [&artistKey](const CollectionEntry& row) {
               return std::ranges::find(row.ancestorKeys, artistKey) != row.ancestorKeys.end();
           }), "Collapse hides descendants while retaining counters");

    fixture.snapshot.artworks.back().artists.push_back(L"a");
    options.collapsed.clear();
    view = buildCollectionView(fixture.snapshot, fixture.paths, options);
    expect(view.fileCount == 20 && groupFor(view, L"Artist A")->files == 11 &&
           groupFor(view, L"Художник Б")->files == 10 && leafNames(view).size() == 21,
           "A shared work appears under both artists without inflating global file count");

    options.group = CollectionGroup::artwork;
    view = buildCollectionView(fixture.snapshot, fixture.paths, options);
    expect(view.fileCount == 20 && leafNames(view).size() == 20 && !groupFor(view, L"Artist A"),
           "Work grouping operates independently of artist grouping");
}

void checkFiltersAndSort()
{
    const auto fixture = collection();
    CollectionViewOptions options;
    options.filter.artist = L"лИС";
    auto view = buildCollectionView(fixture.snapshot, fixture.paths, options);
    expect(view.fileCount == 10, "Artist filter matches case-insensitive Cyrillic aliases");
    options.filter = {};
    options.filter.title = L"LIGHT";
    options.filter.version = L"v02";
    options.filter.format = L"PNG";
    view = buildCollectionView(fixture.snapshot, fixture.paths, options);
    expect(view.fileCount == 1 && view.rows.front().mediaId == L"file-1",
           "Title, numeric version and extension filters combine");
    options.filter = {};
    options.filter.text = L"рАБоТа";
    expect(buildCollectionView(fixture.snapshot, fixture.paths, options).fileCount == 10,
           "General search includes case-insensitive Unicode work titles");
    options.filter = {};
    options.sort = CollectionSort::name;
    view = buildCollectionView(fixture.snapshot, fixture.paths, options);
    expect(view.rows[1].label == L"photo2.png" && view.rows[9].label == L"photo10.png", "Names use natural ordering");
    options.sort = CollectionSort::size;
    options.descending = true;
    const auto none = buildCollectionView(fixture.snapshot, fixture.paths, options);
    options.group = CollectionGroup::format;
    const auto grouped = buildCollectionView(fixture.snapshot, fixture.paths, options);
    expect(leafNames(none) == leafNames(grouped) && grouped.rows.front().group,
           "Grouping leaves the independently requested file sorting intact");

    auto paths = fixture.paths;
    paths.push_back(L"C:\\media\\PHOTO1.PNG");
    paths.push_back(L"C:\\media\\unindexed.gif");
    options = {};
    options.filter.unassigned = true;
    view = buildCollectionView(fixture.snapshot, paths, options);
    expect(view.fileCount == 1 && view.rows.front().path.filename() == L"unindexed.gif" && view.rows.front().mediaId.empty(),
           "Unindexed files remain visible and duplicate paths are folded like Windows");
    options.filter = {};
    expect(buildCollectionView(fixture.snapshot, paths, options).fileCount == 21,
           "One file is counted once even if its path differs only in case");
}

void checkNamesWithoutMetadata()
{
    CollectionSnapshot snapshot;
    const std::vector<std::filesystem::path> paths{
        L"C:\\media\\Худ2_001_v01_Название.png",L"C:\\media\\Худ1_002_v01_Рассвет.png",
        L"C:\\media\\nekoxl-100601298-2740294027.png",L"C:\\media\\[Artist A] First Light.jpg",
        L"C:\\media\\image-05-07.png",L"C:\\media\\randomhash12345.jpg"};
    CollectionViewOptions options;options.group=CollectionGroup::artist;options.sort=CollectionSort::artist;
    const auto grouped=buildCollectionView(snapshot,paths,options);
    expect(grouped.fileCount==6&&leafNames(grouped).size()==6&&grouped.artistCount==4,
        "Artist groups infer common filename patterns and keep all unrecognized files");
    expect(groupFor(grouped,L"Худ1")&&groupFor(grouped,L"nekoxl")&&groupFor(grouped,L"Без художника"),
        "Unicode, social-export prefixes and unknown artist group are displayed");
    options.filter.artist=L"Худ1";
    expect(buildCollectionView(snapshot,paths,options).fileCount==1,"Artist filter includes filenames without stored metadata");
    options={};
    expect(buildCollectionView(snapshot,paths,options).fileCount==6,"All files restores the full folder independently of metadata");
    expect(snapshot.artists.empty()&&snapshot.files.empty(),"Grouping by filename never modifies persistent metadata");
}

void checkTenThousand()
{
    Fixture fixture;
    fixture.snapshot.artists.push_back({L"a", L"Large artist", L"", L"", 0, {}});
    fixture.snapshot.artworks.push_back({L"w", {L"a"}, 1, L"Large work", L"", true, {}});
    for (int index = 0; index < 10000; ++index) {
        const auto id = std::to_wstring(index);
        fixture.snapshot.versions.push_back({id, L"w", index + 1, L"", index == 0, {}});
        MediaRecord file;
        file.id = id;
        file.path = L"C:\\large\\image" + id + L".png";
        file.artworkId = L"w";
        file.versionId = id;
        fixture.paths.push_back(file.path);
        fixture.snapshot.files.push_back(std::move(file));
    }
    CollectionViewOptions options;
    options.group = CollectionGroup::artist;
    const auto started = std::chrono::steady_clock::now();
    auto view = buildCollectionView(fixture.snapshot, fixture.paths, options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    expect(view.fileCount == 10000 && view.additionalVersions == 9999 && view.rows.size() == 20002,
           "Ten-thousand-file collection is grouped using metadata only");
    std::cout << "Grouped 10000 metadata records in " << elapsed.count() << " ms.\n";
}

} // namespace

int main()
{
    try {
        checkGroups();
        checkFiltersAndSort();
        checkNamesWithoutMetadata();
        checkTenThousand();
        std::cout << "Collection view checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
