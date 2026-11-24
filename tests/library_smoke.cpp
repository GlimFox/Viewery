#include "library.hpp"

#include <windows.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace viewery;

namespace {

void require(bool value, const char* description)
{
    if (!value) throw std::runtime_error(description);
    std::cout << "OK  " << description << '\n';
}

std::string read(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void write(const fs::path& path, const std::string& data)
{
    std::ofstream stream(path, std::ios::binary);
    stream << data;
    if (!stream) throw std::runtime_error("Could not write a fixture.");
}

std::wstring wideNumber(int value)
{
    wchar_t name[32]{};
    swprintf_s(name, L"image%03d.png", value);
    return name;
}

struct DirectDatabase {
    sqlite3* value = nullptr;
    explicit DirectDatabase(const fs::path& path) {
        if (sqlite3_open16(path.c_str(), &value) != SQLITE_OK) throw std::runtime_error("Direct test database open failed.");
    }
    ~DirectDatabase() { sqlite3_close(value); }
    void execute(const std::wstring& sql) {
        sqlite3_stmt* query = nullptr;
        if (sqlite3_prepare16_v2(value, sql.c_str(), -1, &query, nullptr) != SQLITE_OK) throw std::runtime_error("Direct test prepare failed.");
        const int result = sqlite3_step(query);
        sqlite3_finalize(query);
        if (result != SQLITE_DONE) throw std::runtime_error("Direct test SQL failed.");
    }
    std::vector<std::vector<std::wstring>> rows(const std::wstring& sql, int columns) {
        sqlite3_stmt* query = nullptr;
        if (sqlite3_prepare16_v2(value, sql.c_str(), -1, &query, nullptr) != SQLITE_OK) throw std::runtime_error("Direct query prepare failed.");
        std::vector<std::vector<std::wstring>> result;
        while (sqlite3_step(query) == SQLITE_ROW) {
            std::vector<std::wstring> row;
            for (int index = 0; index < columns; ++index) {
                const auto* text = static_cast<const wchar_t*>(sqlite3_column_text16(query, index));
                row.emplace_back(text ? text : L"");
            }
            result.push_back(std::move(row));
        }
        sqlite3_finalize(query);
        return result;
    }
};

Artist findArtist(const CollectionStore& store,const LibraryId& id)
{
    for(const auto& artist:store.snapshot().artists) if(artist.id==id) return artist;
    throw std::runtime_error("Artist fixture not found.");
}

std::vector<RenameProposal> artistPlan(const CollectionStore& store,const Artist& artist)
{
    std::vector<RenameProposal> result;
    for(const auto& row:store.previewArtistRename(artist)) result.push_back(row.proposal);
    return result;
}

void checkArtistRegistry(const fs::path& root)
{
    const auto folder=root/L"artist-registry";
    fs::create_directories(folder);
    const auto database=folder/L"artists.sqlite3";
    CollectionStore store(database);
    const auto artistId=store.saveArtist({{},L"Ольга",L"Artist_A",L"legacy; Другой псевдоним",7,{{L"note",L"A 'quoted' note"}}});
    const auto coauthorId=store.saveArtist({{},L"Coauthor",L"Co",{},0,{}});
    const auto spareId=store.saveArtist({{},L"Predefined without files",{}, {},0,{}});
    require(store.snapshot().artists.size()==3,"Artists can be registered before any media is assigned");
    bool duplicate=false;
    try {store.saveArtist({{},L"ОЛЬГА",{}, {},0,{}});} catch(const LibraryError&) {duplicate=true;}
    require(duplicate&&store.snapshot().artists.size()==3,"Registry rejects case-insensitive Unicode duplicate names without replacing another artist");

    const std::vector<fs::path> paths={folder/L"Artist_A_001_v01_Title.PNG",folder/L"legacy & Co_002_v03_Title.webm",
                                     folder/L"restored-original-hash.gif",folder/L"Other_001_v01.png"};
    for(std::size_t index=0;index<paths.size();++index) write(paths[index],"Artist fixture "+std::to_string(index));
    const auto files=store.indexFiles(paths);
    for(std::size_t index=0;index<files.size();++index) {
        const auto artists=index==1?std::vector<LibraryId>{artistId,coauthorId}:
            std::vector<LibraryId>{index==3?coauthorId:artistId};
        const auto work=store.saveArtwork({{},artists,static_cast<std::int64_t>(index+1),L"Title",{},true,{}});
        const auto version=store.saveVersion({{},work,1,{},true,{}});
        store.assignFiles({files[index].id},work,version);
    }
    const auto original=findArtist(store,artistId);
    auto updated=original;updated.name=L"Новый художник";updated.fileName=L"New_A";
    const auto preview=store.previewArtistRename(updated);
    require(preview.size()==3&&std::ranges::all_of(preview,[](const RenamePreview& row){return row.ready;})&&
            std::ranges::count_if(preview,[](const RenamePreview& row){return row.proposal.source!=row.target;})==2,
            "Artist preview covers all linked works, coauthors and unchanged restored filenames");
    require(std::ranges::any_of(preview,[](const RenamePreview& row){return row.proposal.newName==L"New_A & Co_002_v03_Title.webm";})&&
            std::ranges::any_of(preview,[](const RenamePreview& row){return row.proposal.newName==L"New_A_001_v01_Title.PNG";}),
            "Artist filename replacement honors aliases, coauthors, original numbering and extension case");
    const auto plan=artistPlan(store,updated);
    const auto conflict=folder/L"New_A_001_v01_Title.PNG";
    write(conflict,"Unrelated target file");
    require(!store.renameArtist(original,updated,plan).success&&findArtist(store,artistId).name==original.name&&
            read(conflict)=="Unrelated target file"&&fs::exists(paths[0]),
            "Target filename conflict leaves artist metadata and existing files untouched");
    fs::remove(conflict);
    const HANDLE locked=CreateFileW(paths[1].c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(locked!=INVALID_HANDLE_VALUE,"Prepare artist rename sharing conflict");
    const auto lockedResult=store.renameArtist(original,updated,plan);
    CloseHandle(locked);
    require(!lockedResult.success&&findArtist(store,artistId).name==original.name&&
            std::ranges::all_of(paths,[](const fs::path& path){return fs::exists(path);}),
            "Locked artist file prevents all file moves and metadata changes");
    {
        CollectionStore external(database);
        auto edited=original;edited.aliases+=L"; External edit";external.saveArtist(edited);
    }
    require(!store.renameArtist(original,updated,plan).success&&findArtist(store,artistId).aliases!=original.aliases&&fs::exists(paths[0]),
            "Stale artist preview cannot overwrite metadata changed through another database connection");
    store.saveArtist(original);
    auto tampered=plan;tampered.front().newName=L"Unreviewed.png";
    require(!store.renameArtist(original,updated,tampered).success&&fs::exists(paths[0]),
            "Artist rename refuses proposals outside the regenerated artist token replacement");
    auto duplicateRename=updated;duplicateRename.name=L"COAUTHOR";
    require(!store.renameArtist(original,duplicateRename,plan).success&&findArtist(store,artistId).name==original.name,
            "Renaming an artist cannot take another registered artist's name");
    const auto renamed=store.renameArtist(original,updated,plan);
    if(!renamed.success) std::wcerr<<renamed.message<<L'\n';
    require(renamed.success&&renamed.changes.size()==2&&findArtist(store,artistId).name==updated.name&&
            fs::exists(paths[2])&&fs::exists(paths[3]),
            "Artist and tagged files commit together while original and unrelated names are preserved");
    for(const auto& [before,after]:renamed.changes) {
        const auto index=std::ranges::find(paths,before)-paths.begin();
        require(read(after)=="Artist fixture "+std::to_string(index),"Artist rename preserves media bytes");
    }
    {
        CollectionStore reopened(database);
        require(reopened.undo(renamed.operationId).success,"Artist rename Undo works after closing and reopening the database");
    }
    const auto restored=findArtist(store,artistId);
    require(restored.name==original.name&&restored.fileName==original.fileName&&restored.aliases==original.aliases&&
            restored.fields==original.fields&&restored.order==original.order&&
            std::ranges::all_of(paths,[](const fs::path& path){return fs::exists(path);}),
            "Undo restores artist fields and filenames without changing stable assignment IDs");
    auto caseOnly=original;caseOnly.fileName=L"artist_a";
    const auto caseRename=store.renameArtist(original,caseOnly,artistPlan(store,caseOnly));
    require(caseRename.success&&store.undo(caseRename.operationId).success,
            "Artist filename changes support case-only Windows renames and their Undo");

    const auto interrupted=store.renameArtist(original,updated,artistPlan(store,updated));
    require(interrupted.success,"Prepare durable artist recovery journal");
    {
        DirectDatabase direct(database);
        direct.execute(L"UPDATE operations SET status='running' WHERE id='"+interrupted.operationId+L"'");
    }
    bool refusedDelete=false;
    try {store.deleteArtist(artistId);} catch(const LibraryError&) {refusedDelete=true;}
    require(refusedDelete,"Registry deletion cannot invalidate unfinished artist recovery journals");
    bool refusedEdit=false;
    try {store.saveArtist(original);} catch(const LibraryError&) {refusedEdit=true;}
    require(refusedEdit,"Registry edits cannot overwrite metadata required by unfinished recovery journals");
    {
        CollectionStore reopened(database);
        require(reopened.recover(interrupted.operationId).success&&findArtist(reopened,artistId).name==original.name,
                "Recovery restores both artist metadata and filenames from a durable journal");
    }
    const auto spare=findArtist(store,spareId);
    auto renamedSpare=spare;renamedSpare.name=L"Renamed without files";renamedSpare.fileName=renamedSpare.name;
    const auto emptyChange=store.renameArtist(spare,renamedSpare,{});
    require(emptyChange.success&&!emptyChange.operationId.empty()&&emptyChange.changes.empty()&&
            findArtist(store,spareId).name==renamedSpare.name&&store.undo(emptyChange.operationId).success,
            "Artist rename with no linked files still journals metadata and supports Undo");
    const auto emptyRecovery=store.renameArtist(spare,renamedSpare,{});
    {
        DirectDatabase direct(database);
        direct.execute(L"UPDATE operations SET status='running' WHERE id='"+emptyRecovery.operationId+L"'");
    }
    require(store.recover(emptyRecovery.operationId).success&&findArtist(store,spareId).name==spare.name,
            "Metadata-only artist journals recover without physical file steps");

    const auto beforeDeletion=store.snapshot();
    store.deleteArtist(artistId);
    const auto afterDeletion=store.snapshot();
    require(afterDeletion.artists.size()+1==beforeDeletion.artists.size()&&afterDeletion.artworks.size()==beforeDeletion.artworks.size()&&
            afterDeletion.versions.size()==beforeDeletion.versions.size()&&afterDeletion.files.size()==beforeDeletion.files.size()&&
            std::ranges::all_of(afterDeletion.artworks,[&](const Artwork& work){return std::ranges::find(work.artists,artistId)==work.artists.end();})&&
            std::ranges::count_if(afterDeletion.artworks,[&](const Artwork& work){return std::ranges::find(work.artists,coauthorId)!=work.artists.end();})==2&&
            std::ranges::all_of(paths,[](const fs::path& path){return fs::exists(path);}),
            "Removing an artist detaches only that artist, preserving coauthors, works, versions and physical files");

    const auto migrationFolder=root/L"artist-migration";
    fs::create_directories(migrationFolder);
    const auto migrationPath=migrationFolder/L"collection.sqlite3";
    {CollectionStore fresh(migrationPath);fresh.saveArtist({{},L"Old schema artist",{}, {},0,{}});}
    {
        DirectDatabase direct(migrationPath);
        direct.execute(L"DROP TABLE operation_artist_changes");direct.execute(L"PRAGMA user_version=1");
    }
    {CollectionStore migrated(migrationPath);require(migrated.snapshot().artists.size()==1,"Schema-one collections migrate without losing registered artists");}
    std::vector<fs::path> migrationBackups;
    for(const auto& item:fs::directory_iterator(migrationFolder))
        if(item.path().filename().wstring().starts_with(L"collection-before-schema-2-")) migrationBackups.push_back(item.path());
    require(migrationBackups.size()==1,"Artist journal schema migration creates a database backup before modifying the schema");
    DirectDatabase backup(migrationBackups.front());
    require(backup.rows(L"PRAGMA user_version",1).front().front()==L"1"&&
            backup.rows(L"SELECT name FROM artists",1).front().front()==L"Old schema artist",
            "Pre-migration backup retains the original schema and artist data");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    try {
        const auto root = fs::absolute(argv[1]) / (L"library-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(root / L"Коллекция & Unicode");
        const auto folder = root / L"Коллекция & Unicode";
        const auto database = root / L"collection.sqlite3";
        std::vector<fs::path> files;
        std::vector<std::string> contents;
        for (int index = 0; index < 20; ++index) {
            files.push_back(folder / wideNumber(index));
            contents.push_back("Media fixture " + std::to_string(index) + std::string(128, static_cast<char>(index + 40)));
            write(files.back(), contents.back());
        }
        CollectionStore store(database);
        auto media = store.indexFiles(files);
        require(media.size() == 20, "Index twenty files with stable IDs");
        const auto artistA = store.saveArtist({{}, L"Artist A", L"Artist A", L"Автор А", 0, {{L"website", L"https://example.invalid"}}});
        const auto artistB = store.saveArtist({{}, L"Artist B", L"Artist B", {}, 1, {}});
        const auto first = store.saveArtwork({{}, {artistA}, 1, L"First Light", {}, true, {}});
        for (int index = 0; index < 8; ++index) {
            const auto version = store.saveVersion({{}, first, index + 1, {}, index == 0, {}});
            store.assignFiles({media[static_cast<std::size_t>(index)].id}, first, version);
        }
        for (int index = 8; index < 20; ++index) {
            const auto artist = index < 10 ? artistA : artistB;
            const auto number = store.nextArtworkNumber(artist);
            const auto artwork = store.saveArtwork({{}, {artist}, number, L"Работа " + std::to_wstring(index), {}, true, {}});
            const auto version = store.saveVersion({{}, artwork, 1, L"Основная", true, {}});
            store.assignFiles({media[static_cast<std::size_t>(index)].id}, artwork, version);
        }
        const auto snapshot = store.snapshot();
        const auto countA = std::ranges::count_if(snapshot.artworks, [&](const Artwork& item) {
            return std::ranges::find(item.artists, artistA) != item.artists.end();
        });
        require(countA == 3 && snapshot.files.size() == 20 && snapshot.versions.size() == 20,
                "Artist A has three works and ten files across eight versions of its first work");
        require(store.nextArtworkNumber(artistA) == 4, "Saved work numbering is stable");
        const auto preset = store.savePreset({{}, L"Стандарт", L"{\"version\":1,\"blocks\":[]}"});
        store.setSetting(L"grouping", L"artist");
        require(store.presets().size() == 1 && !preset.empty(), "Presets and collection settings persist");

        std::vector<RenameProposal> proposals;
        for (int index = 0; index < 8; ++index) {
            wchar_t name[96]{};
            swprintf_s(name, L"Artist A_001_v%02d_First Light.png", index + 1);
            proposals.push_back({media[static_cast<std::size_t>(index)].id, files[static_cast<std::size_t>(index)], name, true});
        }
        const auto preview = previewRenames(proposals);
        require(std::ranges::all_of(preview, [](const RenamePreview& row) { return row.ready; }), "Preview validates names before changing files");
        const auto renamed = store.rename(proposals);
        if (!renamed.success) std::wcerr << renamed.message << L'\n';
        require(renamed.success && renamed.changes.size() == 8, "Rename eight versions as one operation");
        for (std::size_t index = 0; index < renamed.changes.size(); ++index) {
            require(read(renamed.changes[index].second) == contents[index], "File content remains byte-identical after rename");
        }
        {
            CollectionStore reopened(database);
            require(reopened.undo(renamed.operationId).success, "Undo from a reopened database restores filenames");
        }
        require(std::ranges::all_of(files, [](const fs::path& file) { return fs::exists(file); }), "All original names exist after Undo");
        const auto cycle = store.rename({{media[0].id, files[0], files[1].filename().wstring(), true},
                                         {media[1].id, files[1], files[0].filename().wstring(), true}});
        require(cycle.success && read(files[1]) == contents[0] && read(files[0]) == contents[1], "Cyclic rename swaps names without overwriting content");
        require(store.undo(cycle.operationId).success, "Undo handles the cyclic operation");
        const auto caseOnly = store.rename({{media[0].id, files[0], L"IMAGE000.png", true}});
        require(caseOnly.success && store.undo(caseOnly.operationId).success, "Case-only rename and Undo work");

        const auto duplicates = previewRenames({{media[0].id, files[0], L"same.png", true},
                                                {media[1].id, files[1], L"SAME.png", true}});
        require(!duplicates[0].ready && !duplicates[1].ready, "Case-insensitive duplicate targets are rejected");
        require(!previewRenames({{media[0].id, files[0], L"CON.png", true}})[0].ready &&
                sanitizeWindowsName(L"CON.png") == L"_CON.png", "Reserved Windows names are validated and sanitized");
        write(folder / L"occupied.png", "unrelated data");
        require(!store.rename({{media[0].id, files[0], L"occupied.png", true}}).success &&
                read(folder / L"occupied.png") == "unrelated data", "Existing unrelated files are never overwritten");
        HANDLE locked = CreateFileW(files[0].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(locked != INVALID_HANDLE_VALUE, "Open fixture with a conflicting share mode");
        const auto lockedResult = store.rename({{media[0].id, files[0], L"locked.png", true}});
        CloseHandle(locked);
        require(!lockedResult.success && fs::exists(files[0]), "Locked files fail without changing names");
        std::stop_source canceled;
        canceled.request_stop();
        require(store.rename(proposals, canceled.get_token()).cancelled, "Pre-canceled operation changes no files");

        const auto external = folder / L"Внешнее переименование.png";
        require(MoveFileExW(files[2].c_str(), external.c_str(), 0) != FALSE, "Simulate an external filename change");
        const auto matched = store.indexFiles({external});
        require(matched[0].id == media[2].id && matched[0].artworkId == first, "Filesystem identity preserves metadata after an external rename");
        require(MoveFileExW(external.c_str(), files[2].c_str(), 0) != FALSE, "Restore the external rename fixture");
        store.indexFiles({files[2]});

        const auto crash = store.rename({{media[0].id, files[0], L"crash-a.png", true},
                                         {media[1].id, files[1], L"crash-b.png", true}});
        require(crash.success, "Prepare persistent operation journal for crash recovery");
        {
            DirectDatabase journal(database);
            const auto steps = journal.rows(L"SELECT source,temporary,target FROM rename_steps WHERE operation_id='" + crash.operationId + L"' ORDER BY position", 3);
            require(steps.size() == 2 && MoveFileExW(steps[0][2].c_str(), steps[0][1].c_str(), 0) &&
                    MoveFileExW(steps[1][2].c_str(), steps[1][0].c_str(), 0), "Simulate interruption between staging file renames");
            journal.execute(L"UPDATE operations SET status='running' WHERE id='" + crash.operationId + L"'");
            journal.execute(L"UPDATE rename_steps SET stage='stage_intent' WHERE operation_id='" + crash.operationId + L"'");
        }
        {
            CollectionStore reopened(database);
            require(reopened.recover(crash.operationId).success, "Recovery after reopening resolves an interrupted staging phase");
        }
        require(read(files[0]) == contents[0] && read(files[1]) == contents[1], "Recovery preserves original file bytes");

        const auto finalGap = store.rename({{media[0].id, files[0], L"final-a.png", true},
                                            {media[1].id, files[1], L"final-b.png", true}});
        require(finalGap.success, "Prepare completed physical moves for commit-gap recovery");
        {
            DirectDatabase journal(database);
            journal.execute(L"UPDATE operations SET status='running' WHERE id='" + finalGap.operationId + L"'");
        }
        {
            CollectionStore reopened(database);
            require(reopened.recover(finalGap.operationId).success && read(files[0]) == contents[0] && read(files[1]) == contents[1],
                    "Recovery handles final file moves before the operation commit");
        }

        const auto rollbackGap = store.rename({{media[0].id, files[0], files[1].filename().wstring(), true},
                                               {media[1].id, files[1], files[0].filename().wstring(), true}});
        require(rollbackGap.success, "Prepare cyclic operation for interrupted rollback");
        {
            DirectDatabase journal(database);
            const auto steps = journal.rows(L"SELECT source,temporary,target FROM rename_steps WHERE operation_id='" + rollbackGap.operationId + L"' ORDER BY position", 3);
            require(MoveFileExW(steps[0][2].c_str(), steps[0][1].c_str(), 0) &&
                    MoveFileExW(steps[1][2].c_str(), steps[1][1].c_str(), 0) &&
                    MoveFileExW(steps[0][1].c_str(), steps[0][0].c_str(), 0), "Simulate a partially restored cyclic rollback");
            journal.execute(L"UPDATE operations SET status='recovery_required' WHERE id='" + rollbackGap.operationId + L"'");
            journal.execute(L"UPDATE rename_steps SET stage='restored' WHERE operation_id='" + rollbackGap.operationId + L"' AND position=0");
            journal.execute(L"UPDATE rename_steps SET previous=temporary,temporary='" + (folder / L"never-created-recovery.png").wstring() +
                L"',stage='rollback_intent' WHERE operation_id='" + rollbackGap.operationId + L"' AND position=1");
        }
        {
            CollectionStore reopened(database);
            require(reopened.recover(rollbackGap.operationId).success && read(files[0]) == contents[0] && read(files[1]) == contents[1],
                    "Recovery locates the previous temporary path across a rollback-intent crash");
        }

        const auto modified = store.rename({{media[0].id, files[0], L"changed.png", true}});
        require(modified.success, "Prepare external-content conflict");
        write(modified.changes.front().second, "Changed by another program");
        require(!store.undo(modified.operationId).success && read(modified.changes.front().second) == "Changed by another program",
                "Undo rejects externally modified content without moving or replacing it");
        write(modified.changes.front().second, contents[0]);
        require(store.undo(modified.operationId).success, "Undo succeeds once the external conflict is resolved");

        const auto cancellationFolder = root / L"cancel";
        fs::create_directories(cancellationFolder);
        CollectionStore cancelStore(root / L"cancel.sqlite3");
        std::vector<fs::path> cancellationFiles;
        for (int index = 0; index < 80; ++index) {
            const auto file = cancellationFolder / wideNumber(index);
            write(file, "Cancellation fixture " + std::to_string(index));
            cancellationFiles.push_back(file);
        }
        const auto cancellationMedia = cancelStore.indexFiles(cancellationFiles);
        std::vector<RenameProposal> cancellationPlan;
        for (const auto& file : cancellationMedia) {
            cancellationPlan.push_back({file.id, file.path, L"renamed-" + file.path.filename().wstring(), true});
        }
        RenameResult cancellationResult;
        std::atomic<bool> finished = false;
        std::jthread running([&](std::stop_token token) {
            cancellationResult = cancelStore.rename(cancellationPlan, token);
            finished = true;
        });
        bool interrupted = false;
        {
            DirectDatabase observer(root / L"cancel.sqlite3");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!finished && std::chrono::steady_clock::now() < deadline) {
                const auto moved = observer.rows(L"SELECT count(*) FROM rename_steps WHERE stage IN('staged','done')", 1);
                if (!moved.empty() && moved.front().front() != L"0") {
                    interrupted = true;
                    running.request_stop();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        running.join();
        require(interrupted && cancellationResult.cancelled && !cancellationResult.success &&
                std::ranges::all_of(cancellationFiles, [](const fs::path& file) { return fs::exists(file); }),
                "Cancel after a physical rename rolls the whole operation back");

        store.backup(root / L"backup.sqlite3");
        store.exportMetadata(root / L"metadata.json");
        CollectionStore imported(root / L"imported.sqlite3");
        imported.importMetadata(root / L"metadata.json");
        const auto importedSnapshot = imported.snapshot();
        require(importedSnapshot.artists.size() == snapshot.artists.size() && importedSnapshot.files.size() == 20 &&
                importedSnapshot.versions.size() == 20 && imported.presets().size() == 1 && imported.setting(L"grouping") == L"artist",
                "Metadata JSON roundtrip preserves entities, presets and settings");
        CollectionStore backup(root / L"backup.sqlite3");
        require(backup.snapshot().files.size() == 20, "Database backup opens and passes integrity checks");

        checkArtistRegistry(root);

        const auto large = root / L"10000";
        fs::create_directories(large);
        std::vector<fs::path> many;
        many.reserve(10000);
        for (int index = 0; index < 10000; ++index) {
            const auto file = large / (L"fixture" + std::to_wstring(index) + L".png");
            write(file, std::to_string(index));
            many.push_back(file);
        }
        const auto start = std::chrono::steady_clock::now();
        CollectionStore largeStore(root / L"large.sqlite3");
        require(largeStore.indexFiles(many).size() == 10000 && largeStore.snapshot().files.size() == 10000,
                "Ten thousand files index and snapshot without decoding media");
        std::cout << "10000 index/snapshot milliseconds: " <<
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << '\n';
        std::cout << "Fixtures retained at " << root << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
