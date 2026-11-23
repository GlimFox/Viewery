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
