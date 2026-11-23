#include "library.hpp"
#include "third_party/json.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace viewery {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

std::string utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw LibraryError("Invalid UTF-16 text.");
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw LibraryError("Invalid UTF-8 text.");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), count);
    return result;
}

std::wstring systemError(std::wstring_view action, DWORD code = GetLastError())
{
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result(action);
    result += L" (" + std::to_wstring(code) + L")";
    if (message) {
        result += L": ";
        result += message;
        LocalFree(message);
    }
    return result;
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value);
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE && value; }
};

std::wstring guid()
{
    GUID value{};
    wchar_t text[40]{};
    if (FAILED(CoCreateGuid(&value)) || !StringFromGUID2(value, text, static_cast<int>(std::size(text)))) {
        throw LibraryError("Could not create a collection identifier.");
    }
    return text;
}

std::wstring utcNow()
{
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t value[32]{};
    swprintf_s(value, L"%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay,
               utc.wHour, utc.wMinute, utc.wSecond);
    return value;
}

fs::path absolutePath(const fs::path& path)
{
    std::error_code error;
    auto result = fs::absolute(path, error).lexically_normal();
    if (error || result.empty()) throw LibraryError("Could not resolve a file path.");
    return result;
}

std::wstring pathKey(const fs::path& path)
{
    std::wstring value = absolutePath(path).wstring();
    if (value.starts_with(L"\\\\?\\UNC\\")) value = L"\\\\" + value.substr(8);
    else if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    std::replace(value.begin(), value.end(), L'/', L'\\');
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, value.data(),
                                  static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!size) throw LibraryError("Could not compare a file path.");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, value.data(), static_cast<int>(value.size()),
                  result.data(), size, nullptr, nullptr, 0);
    return result;
}

bool fileExists(const fs::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    throw LibraryError(utf8(systemError(L"Не удалось проверить путь", error)));
}

std::wstring nameProblem(std::wstring_view name)
{
    if (name.empty()) return L"Пустое имя файла";
    if (name.size() > 255) return L"Имя длиннее 255 символов UTF-16";
    if (name == L"." || name == L"..") return L"Недопустимое имя файла";
    if (name.back() == L'.' || name.back() == L' ') return L"Имя заканчивается точкой или пробелом";
    if (std::ranges::any_of(name, [](wchar_t ch) {
            return ch < 32 || std::wstring_view(L"<>:\"/\\|?*").find(ch) != std::wstring_view::npos;
        })) return L"В имени есть недопустимые символы Windows";
    std::wstring device(name.substr(0, name.find(L'.')));
    std::ranges::transform(device, device.begin(), [](wchar_t ch) {
        return ch >= L'a' && ch <= L'z' ? static_cast<wchar_t>(ch - L'a' + L'A') : ch;
    });
    const bool numbered = device.size() == 4 && (device.starts_with(L"COM") || device.starts_with(L"LPT")) &&
        ((device.back() >= L'1' && device.back() <= L'9') || device.back() == L'¹' || device.back() == L'²' || device.back() == L'³');
    if (device == L"CON" || device == L"PRN" || device == L"AUX" || device == L"NUL" || numbered) {
        return L"Имя зарезервировано Windows";
    }
    return {};
}

std::wstring hexBytes(const void* memory, std::size_t size)
{
    constexpr wchar_t digits[] = L"0123456789abcdef";
    const auto* bytes = static_cast<const unsigned char*>(memory);
    std::wstring value;
    value.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        value.push_back(digits[bytes[index] >> 4]);
        value.push_back(digits[bytes[index] & 15]);
    }
    return value;
}

struct FileState {
    std::wstring identity, hash;
    std::uint64_t bytes = 0;
    std::int64_t modified = 0;
};

FileState fileState(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION basic{};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandle(handle, &basic) || (basic.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        ((basic.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
         (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
          IsReparseTagNameSurrogate(tag.ReparseTag)))) {
        throw LibraryError("The collection requires a regular media file rather than a symbolic link.");
    }
    FileState state;
    state.bytes = (static_cast<std::uint64_t>(basic.nFileSizeHigh) << 32) | basic.nFileSizeLow;
    const auto time = (static_cast<std::uint64_t>(basic.ftLastWriteTime.dwHighDateTime) << 32) | basic.ftLastWriteTime.dwLowDateTime;
    state.modified = static_cast<std::int64_t>(time / 10000000) - 11644473600LL;
    FILE_ID_INFO id{};
    const FILE_ID_128 empty{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) &&
        std::memcmp(&id.FileId, &empty, sizeof(empty)) != 0) {
        state.identity = hexBytes(&id.VolumeSerialNumber, sizeof(id.VolumeSerialNumber)) + L":" +
                         hexBytes(&id.FileId, sizeof(id.FileId));
    } else if (basic.nFileIndexHigh || basic.nFileIndexLow) {
        const auto index = (static_cast<std::uint64_t>(basic.nFileIndexHigh) << 32) | basic.nFileIndexLow;
        state.identity = hexBytes(&basic.dwVolumeSerialNumber, sizeof(basic.dwVolumeSerialNumber)) + L":" +
                         hexBytes(&index, sizeof(index));
    }
    return state;
}

Handle readFile(const fs::path& path, bool forRename = false)
{
    const DWORD access = FILE_READ_ATTRIBUTES | (forRename ? GENERIC_READ | DELETE : 0);
    const DWORD share = forRename ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    Handle handle(CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle) throw LibraryError(utf8(systemError(L"Не удалось открыть " + path.filename().wstring())));
    return handle;
}

std::wstring hashFile(HANDLE file, std::stop_token cancellation = {})
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw LibraryError("SHA-256 is unavailable.");
    }
    std::array<unsigned char, 32> digest{};
    std::array<unsigned char, 65536> buffer{};
    bool success = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    LARGE_INTEGER start{};
    success = success && SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
    DWORD read = 0;
    while (success) {
        if (cancellation.stop_requested()) { success = false; break; }
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            success = false;
            break;
        }
        if (!read) break;
        success = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    success = success && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (cancellation.stop_requested()) throw LibraryError("The collection operation was canceled.");
    if (!success) throw LibraryError("Could not verify file contents with SHA-256.");
    return hexBytes(digest.data(), digest.size());
}

FileState indexedState(const fs::path& path, HANDLE handle, std::stop_token cancellation)
{
    FileState state = fileState(handle);
    if (state.identity.empty()) {
        Handle contents(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!contents) throw LibraryError("This filesystem has no stable IDs and its file could not be fingerprinted.");
        state.hash = hashFile(contents.value, cancellation);
    }
    return state;
}

void renameHandle(HANDLE handle, const fs::path& target)
{
    const std::wstring path = absolutePath(target).wstring();
    const std::size_t size = offsetof(FILE_RENAME_INFO, FileName) + (path.size() + 1) * sizeof(wchar_t);
    std::vector<unsigned char> storage(size);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(path.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    if (!SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(size))) {
        throw LibraryError(utf8(systemError(L"Не удалось переименовать в " + target.filename().wstring())));
    }
}

Json fieldsJson(const Metadata& fields)
{
    Json result = Json::object();
    for (const auto& [key, value] : fields) result[utf8(key)] = utf8(value);
    return result;
}

Metadata decodeFields(const Json& fields)
{
    if (!fields.is_object()) throw LibraryError("Metadata fields must be a JSON object.");
    Metadata result;
    for (const auto& item : fields.items()) result.emplace(wide(item.key()), wide(item.value().get<std::string>()));
    return result;
}

Metadata readFields(std::wstring_view fields)
{
    return decodeFields(Json::parse(utf8(fields)));
}

struct SqliteApi {
    HMODULE module = nullptr;
    decltype(&sqlite3_open16) open = nullptr;
    decltype(&sqlite3_close) close = nullptr;
    decltype(&sqlite3_prepare16_v2) prepare = nullptr;
    decltype(&sqlite3_step) step = nullptr;
    decltype(&sqlite3_finalize) finalize = nullptr;
    decltype(&sqlite3_bind_text16) bindText = nullptr;
    decltype(&sqlite3_bind_int64) bindInteger = nullptr;
    decltype(&sqlite3_bind_null) bindNull = nullptr;
    decltype(&sqlite3_column_text16) text = nullptr;
    decltype(&sqlite3_column_int64) integer = nullptr;
    decltype(&sqlite3_errmsg16) error = nullptr;
    decltype(&sqlite3_exec) exec = nullptr;
    decltype(&sqlite3_busy_timeout) busy = nullptr;
    decltype(&sqlite3_backup_init) backupInit = nullptr;
    decltype(&sqlite3_backup_step) backupStep = nullptr;
    decltype(&sqlite3_backup_finish) backupFinish = nullptr;

    SqliteApi()
    {
        module = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw LibraryError("Windows SQLite is unavailable.");
#define VIEWERY_SQLITE(member, name) \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(module, name)); \
        if (!member) throw LibraryError("Windows SQLite lacks " name ".")
        VIEWERY_SQLITE(open, "sqlite3_open16");
        VIEWERY_SQLITE(close, "sqlite3_close");
        VIEWERY_SQLITE(prepare, "sqlite3_prepare16_v2");
        VIEWERY_SQLITE(step, "sqlite3_step");
        VIEWERY_SQLITE(finalize, "sqlite3_finalize");
        VIEWERY_SQLITE(bindText, "sqlite3_bind_text16");
        VIEWERY_SQLITE(bindInteger, "sqlite3_bind_int64");
        VIEWERY_SQLITE(bindNull, "sqlite3_bind_null");
        VIEWERY_SQLITE(text, "sqlite3_column_text16");
        VIEWERY_SQLITE(integer, "sqlite3_column_int64");
        VIEWERY_SQLITE(error, "sqlite3_errmsg16");
        VIEWERY_SQLITE(exec, "sqlite3_exec");
        VIEWERY_SQLITE(busy, "sqlite3_busy_timeout");
        VIEWERY_SQLITE(backupInit, "sqlite3_backup_init");
        VIEWERY_SQLITE(backupStep, "sqlite3_backup_step");
        VIEWERY_SQLITE(backupFinish, "sqlite3_backup_finish");
#undef VIEWERY_SQLITE
    }
};

SqliteApi& api()
{
    static SqliteApi instance;
    return instance;
}

[[noreturn]] void databaseError(sqlite3* database)
{
    const auto* message = static_cast<const wchar_t*>(api().error(database));
    throw LibraryError(message ? utf8(message) : "Collection database error.");
}

void execute(sqlite3* database, const char* query)
{
    if (api().exec(database, query, nullptr, nullptr, nullptr) != SQLITE_OK) databaseError(database);
}

struct Statement {
    sqlite3* database;
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* connection, std::wstring_view query) : database(connection)
    {
        if (api().prepare(database, query.data(), static_cast<int>(query.size() * sizeof(wchar_t)), &value, nullptr) != SQLITE_OK) {
            databaseError(database);
        }
    }
    ~Statement() { if (value) api().finalize(value); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void text(int index, std::wstring_view item) {
        const wchar_t* text = item.empty() ? L"" : item.data();
        if (api().bindText(value, index, text, static_cast<int>(item.size() * sizeof(wchar_t)), SQLITE_TRANSIENT) != SQLITE_OK) {
            databaseError(database);
        }
    }
    void optional(int index, std::wstring_view item) {
        if (item.empty()) api().bindNull(value, index);
        else text(index, item);
    }
    void integer(int index, std::int64_t item) {
        if (api().bindInteger(value, index, item) != SQLITE_OK) databaseError(database);
    }
    bool row() {
        const int result = api().step(value);
        if (result == SQLITE_ROW) return true;
        if (result != SQLITE_DONE) databaseError(database);
        return false;
    }
    void done() { if (row()) throw LibraryError("Unexpected database result."); }
    std::wstring text(int index) const {
        const auto* result = static_cast<const wchar_t*>(api().text(value, index));
        return result ? result : L"";
    }
    std::int64_t integer(int index) const { return api().integer(value, index); }
};

struct Transaction {
    sqlite3* database;
    bool committed = false;
    std::string name;
    explicit Transaction(sqlite3* connection) : database(connection) {
        static std::atomic<unsigned long long> sequence = 0;
        name = "viewery_" + std::to_string(++sequence);
        execute(database, ("SAVEPOINT " + name).c_str());
    }
    ~Transaction() {
        if (!committed) {
            api().exec(database, ("ROLLBACK TO " + name).c_str(), nullptr, nullptr, nullptr);
            api().exec(database, ("RELEASE " + name).c_str(), nullptr, nullptr, nullptr);
        }
    }
    void commit() { execute(database, ("RELEASE " + name).c_str()); committed = true; }
};

} // namespace

fs::path defaultDatabasePath()
{
    wchar_t* local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local))) {
        throw LibraryError("The user data directory is unavailable.");
    }
    fs::path result(local);
    CoTaskMemFree(local);
    return result / L"Viewery" / L"collection.sqlite3";
}

std::wstring sanitizeWindowsName(std::wstring_view name)
{
    std::wstring result(name);
    std::ranges::replace_if(result, [](wchar_t ch) {
        return ch < 32 || std::wstring_view(L"<>:\"/\\|?*").find(ch) != std::wstring_view::npos;
    }, L'_');
    while (!result.empty() && (result.back() == L'.' || result.back() == L' ')) result.pop_back();
    if (result.empty() || result == L"." || result == L"..") result = L"Без названия";
    if (!nameProblem(result).empty()) result.insert(result.begin(), L'_');
    return result;
}

std::vector<RenamePreview> previewRenames(const std::vector<RenameProposal>& proposals)
{
    std::vector<RenamePreview> rows;
    std::map<std::wstring, std::size_t> sources, targets;
    for (const auto& proposal : proposals) {
        RenamePreview row{proposal, {}, {}, false};
        if (!proposal.enabled) { rows.push_back(std::move(row)); continue; }
        try {
            row.proposal.source = absolutePath(proposal.source);
            row.target = row.proposal.source.parent_path() / proposal.newName;
            row.problem = nameProblem(proposal.newName);
            if (row.problem.empty() && fs::path(proposal.newName).extension().wstring() != row.proposal.source.extension().wstring()) {
                row.problem = L"Сохраните исходное расширение файла";
            }
            if (row.problem.empty()) {
                const auto key = pathKey(row.proposal.source);
                if (!sources.emplace(key, rows.size()).second) row.problem = L"Файл выбран дважды";
                readFile(row.proposal.source);
            }
        } catch (const std::exception& error) { row.problem = wide(error.what()); }
        rows.push_back(std::move(row));
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        auto& row = rows[index];
        if (!row.proposal.enabled || !row.problem.empty()) continue;
        try {
            const auto key = pathKey(row.target);
            const auto [found, added] = targets.emplace(key, index);
            if (!added) {
                row.problem = L"Несколько файлов получают одинаковое имя";
                rows[found->second].problem = row.problem;
            } else if (fileExists(row.target) && !sources.contains(key)) {
                row.problem = L"Файл с таким именем уже существует";
            }
        } catch (const std::exception& error) { row.problem = wide(error.what()); }
    }
    for (auto& row : rows) row.ready = row.proposal.enabled && row.problem.empty();
    return rows;
}

struct CollectionStore::Impl {
    fs::path path;
    sqlite3* database = nullptr;
    mutable std::recursive_mutex mutex;
    Handle operationMutex;

    explicit Impl(fs::path location);
    ~Impl() { if (database) api().close(database); }
    void saveMedia(const MediaRecord& file, const FileState* state = nullptr);
    MediaRecord media(Statement& query) const;
    std::vector<MediaRecord> indexed(const std::vector<fs::path>& files, std::stop_token cancellation);
    struct RenameStep {
        LibraryId fileId;
        fs::path source, temporary, target, current, previous;
        FileState expected;
        std::wstring stage;
        Handle handle;
    };
    void operationStatus(const LibraryId& operation, std::wstring_view status, std::wstring_view message = {});
    void stepStage(const LibraryId& operation, std::size_t index, std::wstring_view stage);
    void updateMediaPaths(const std::vector<RenameStep>& steps, bool original);
    bool rollback(const LibraryId& operation, std::vector<RenameStep>& steps, std::wstring& problem);
    RenameResult perform(const std::vector<RenameProposal>& proposals, std::stop_token cancellation,
                         const LibraryId& parent = {});
    std::vector<RenameStep> operationSteps(const LibraryId& operation);
};

CollectionStore::Impl::Impl(fs::path location) : path(absolutePath(location))
{
    fs::create_directories(path.parent_path());
    const auto result = api().open(path.c_str(), &database);
    if (result != SQLITE_OK) {
        const std::string error = database ? utf8(static_cast<const wchar_t*>(api().error(database))) : "Could not open collection database.";
        if (database) api().close(database);
        database = nullptr;
        throw LibraryError(error);
    }
    try {
        api().busy(database, 5000);
        execute(database, "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
        Statement version(database, L"PRAGMA user_version");
        version.row();
        const auto schema = version.integer(0);
        if (schema > 1) throw LibraryError("The collection requires a newer Viewery version.");
        if (schema == 0) {
            Statement count(database, L"SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'");
            count.row();
            if (count.integer(0)) throw LibraryError("This database does not contain a recognized Viewery collection.");
            Transaction transaction(database);
            execute(database,
                "CREATE TABLE artists(id TEXT PRIMARY KEY,name TEXT NOT NULL,file_name TEXT NOT NULL,aliases TEXT NOT NULL,sort_order INTEGER NOT NULL,fields TEXT NOT NULL);"
                "CREATE TABLE artworks(id TEXT PRIMARY KEY,number INTEGER NOT NULL,title TEXT NOT NULL,user_title TEXT NOT NULL,standalone INTEGER NOT NULL,fields TEXT NOT NULL);"
                "CREATE TABLE artwork_artists(artwork_id TEXT NOT NULL REFERENCES artworks(id),artist_id TEXT NOT NULL REFERENCES artists(id),position INTEGER NOT NULL,PRIMARY KEY(artwork_id,artist_id));"
                "CREATE TABLE versions(id TEXT PRIMARY KEY,artwork_id TEXT NOT NULL REFERENCES artworks(id),number INTEGER NOT NULL,title TEXT NOT NULL,is_main INTEGER NOT NULL,fields TEXT NOT NULL,UNIQUE(artwork_id,number));"
                "CREATE TABLE media(id TEXT PRIMARY KEY,path TEXT NOT NULL,path_key TEXT NOT NULL UNIQUE,original_name TEXT NOT NULL,extension TEXT NOT NULL,artwork_id TEXT REFERENCES artworks(id),version_id TEXT REFERENCES versions(id),bytes INTEGER NOT NULL,modified INTEGER NOT NULL,identity TEXT NOT NULL,hash TEXT NOT NULL,fields TEXT NOT NULL);"
                "CREATE INDEX media_identity ON media(identity);"
                "CREATE TABLE presets(id TEXT PRIMARY KEY,name TEXT NOT NULL,json TEXT NOT NULL);"
                "CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                "CREATE TABLE operations(id TEXT PRIMARY KEY,created TEXT NOT NULL,status TEXT NOT NULL,message TEXT NOT NULL,count INTEGER NOT NULL,parent_id TEXT);"
                "CREATE TABLE rename_steps(operation_id TEXT NOT NULL REFERENCES operations(id),position INTEGER NOT NULL,file_id TEXT NOT NULL REFERENCES media(id),source TEXT NOT NULL,temporary TEXT NOT NULL,target TEXT NOT NULL,identity TEXT NOT NULL,hash TEXT NOT NULL,bytes INTEGER NOT NULL,stage TEXT NOT NULL,previous TEXT NOT NULL,PRIMARY KEY(operation_id,position));"
                "PRAGMA user_version=1; PRAGMA application_id=1448563289;");
            transaction.commit();
        }
        Statement check(database, L"PRAGMA quick_check");
        if (!check.row() || check.text(0) != L"ok") throw LibraryError("The collection database failed its integrity check.");
        const auto key = pathKey(path);
        std::uint64_t hash = 1469598103934665603ULL;
        for (wchar_t ch : key) { hash ^= static_cast<unsigned short>(ch); hash *= 1099511628211ULL; }
        const std::wstring name = L"Local\\Viewery.Collection." + std::to_wstring(hash);
        operationMutex = Handle(CreateMutexW(nullptr, FALSE, name.c_str()));
        if (!operationMutex) throw LibraryError("Could not serialize collection operations.");
    } catch (...) {
        api().close(database);
        database = nullptr;
        throw;
    }
}

CollectionStore::CollectionStore(fs::path database) : impl_(std::make_unique<Impl>(std::move(database))) {}
CollectionStore::~CollectionStore() = default;

MediaRecord CollectionStore::Impl::media(Statement& query) const
{
    MediaRecord file;
    file.id = query.text(0);
    file.path = query.text(1);
    file.originalName = query.text(2);
    file.extension = query.text(3);
    file.artworkId = query.text(4);
    file.versionId = query.text(5);
    file.bytes = static_cast<std::uint64_t>(query.integer(6));
    file.modified = query.integer(7);
    file.fields = readFields(query.text(8));
    return file;
}

CollectionSnapshot CollectionStore::snapshot() const
{
    std::lock_guard lock(impl_->mutex);
    CollectionSnapshot result;
    {
        Statement query(impl_->database, L"SELECT id,name,file_name,aliases,sort_order,fields FROM artists ORDER BY sort_order,name,id");
        while (query.row()) result.artists.push_back({query.text(0), query.text(1), query.text(2), query.text(3),
                                                    query.integer(4), readFields(query.text(5))});
    }
    {
        Statement query(impl_->database, L"SELECT id,number,title,user_title,standalone,fields FROM artworks ORDER BY number,id");
        while (query.row()) {
            Artwork artwork{query.text(0), {}, query.integer(1), query.text(2), query.text(3), query.integer(4) != 0,
                            readFields(query.text(5))};
            Statement artists(impl_->database, L"SELECT artist_id FROM artwork_artists WHERE artwork_id=? ORDER BY position");
            artists.text(1, artwork.id);
            while (artists.row()) artwork.artists.push_back(artists.text(0));
            result.artworks.push_back(std::move(artwork));
        }
    }
    {
        Statement query(impl_->database, L"SELECT id,artwork_id,number,title,is_main,fields FROM versions ORDER BY number,id");
        while (query.row()) result.versions.push_back({query.text(0), query.text(1), query.integer(2), query.text(3),
                                                     query.integer(4) != 0, readFields(query.text(5))});
    }
    {
        Statement query(impl_->database, L"SELECT id,path,original_name,extension,artwork_id,version_id,bytes,modified,fields FROM media ORDER BY path_key");
        while (query.row()) result.files.push_back(impl_->media(query));
    }
    return result;
}

LibraryId CollectionStore::saveArtist(Artist artist)
{
    std::lock_guard lock(impl_->mutex);
    if (artist.name.empty()) throw LibraryError("Enter the artist's name.");
    if (artist.id.empty()) artist.id = guid();
    if (artist.fileName.empty()) artist.fileName = artist.name;
    Statement query(impl_->database, L"INSERT INTO artists VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name,file_name=excluded.file_name,aliases=excluded.aliases,sort_order=excluded.sort_order,fields=excluded.fields");
    query.text(1, artist.id);
    query.text(2, artist.name);
    query.text(3, artist.fileName);
    query.text(4, artist.aliases);
    query.integer(5, artist.order);
    query.text(6, wide(fieldsJson(artist.fields).dump()));
    query.done();
    return artist.id;
}

LibraryId CollectionStore::saveArtwork(Artwork artwork)
{
    std::lock_guard lock(impl_->mutex);
    if (artwork.number < 0) throw LibraryError("An artwork number cannot be negative.");
    if (artwork.id.empty()) artwork.id = guid();
    std::set<LibraryId> artists(artwork.artists.begin(), artwork.artists.end());
    if (artists.size() != artwork.artists.size() || artists.contains(L"")) throw LibraryError("Invalid artist assignment.");
    Transaction transaction(impl_->database);
    Statement query(impl_->database, L"INSERT INTO artworks VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET number=excluded.number,title=excluded.title,user_title=excluded.user_title,standalone=excluded.standalone,fields=excluded.fields");
    query.text(1, artwork.id);
    query.integer(2, artwork.number);
    query.text(3, artwork.title);
    query.text(4, artwork.userTitle);
    query.integer(5, artwork.standalone);
    query.text(6, wide(fieldsJson(artwork.fields).dump()));
    query.done();
    Statement clear(impl_->database, L"DELETE FROM artwork_artists WHERE artwork_id=?");
    clear.text(1, artwork.id);
    clear.done();
    for (std::size_t index = 0; index < artwork.artists.size(); ++index) {
        Statement link(impl_->database, L"INSERT INTO artwork_artists VALUES(?,?,?)");
        link.text(1, artwork.id);
        link.text(2, artwork.artists[index]);
        link.integer(3, static_cast<std::int64_t>(index));
        link.done();
    }
    transaction.commit();
    return artwork.id;
}

LibraryId CollectionStore::saveVersion(MediaVersion version)
{
    std::lock_guard lock(impl_->mutex);
    if (version.artworkId.empty() || version.number < 1) throw LibraryError("Choose a work and a positive version number.");
    if (version.id.empty()) version.id = guid();
    Transaction transaction(impl_->database);
    if (version.main) {
        Statement previous(impl_->database, L"UPDATE versions SET is_main=0 WHERE artwork_id=?");
        previous.text(1, version.artworkId);
        previous.done();
    }
    Statement query(impl_->database, L"INSERT INTO versions VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET artwork_id=excluded.artwork_id,number=excluded.number,title=excluded.title,is_main=excluded.is_main,fields=excluded.fields");
    query.text(1, version.id);
    query.text(2, version.artworkId);
    query.integer(3, version.number);
    query.text(4, version.title);
    query.integer(5, version.main);
    query.text(6, wide(fieldsJson(version.fields).dump()));
    query.done();
    transaction.commit();
    return version.id;
}

void CollectionStore::Impl::saveMedia(const MediaRecord& file, const FileState* state)
{
    Statement query(database, L"INSERT INTO media VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET path=excluded.path,path_key=excluded.path_key,original_name=excluded.original_name,extension=excluded.extension,artwork_id=excluded.artwork_id,version_id=excluded.version_id,bytes=excluded.bytes,modified=excluded.modified,identity=CASE WHEN ?=1 THEN excluded.identity ELSE media.identity END,hash=CASE WHEN ?=1 THEN excluded.hash ELSE media.hash END,fields=excluded.fields");
    query.text(1, file.id);
    query.text(2, file.path.wstring());
    query.text(3, pathKey(file.path));
    query.text(4, file.originalName);
    query.text(5, file.extension);
    query.optional(6, file.artworkId);
    query.optional(7, file.versionId);
    query.integer(8, static_cast<std::int64_t>(file.bytes));
    query.integer(9, file.modified);
    query.text(10, state ? state->identity : L"");
    query.text(11, state ? state->hash : L"");
    query.text(12, wide(fieldsJson(file.fields).dump()));
    // The table has 12 columns. Keep the two update flags out of the INSERT list.
    query.integer(13, state != nullptr);
    query.integer(14, state != nullptr);
    query.done();
}

void CollectionStore::saveFile(MediaRecord file)
{
    std::lock_guard lock(impl_->mutex);
    if (file.id.empty() || file.path.empty()) throw LibraryError("Index the file before editing its metadata.");
    if (!file.versionId.empty()) {
        Statement version(impl_->database, L"SELECT artwork_id FROM versions WHERE id=?");
        version.text(1, file.versionId);
        if (!version.row() || version.text(0) != file.artworkId) throw LibraryError("The version belongs to a different work.");
    }
    impl_->saveMedia(file);
}

void CollectionStore::assignFiles(const std::vector<LibraryId>& files, const LibraryId& artwork, const LibraryId& version)
{
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->database);
    if (!version.empty()) {
        Statement work(impl_->database, L"SELECT artwork_id FROM versions WHERE id=?");
        work.text(1, version);
        if (!work.row() || work.text(0) != artwork) throw LibraryError("The version belongs to a different work.");
    }
    for (const auto& file : files) {
        Statement query(impl_->database, L"UPDATE media SET artwork_id=?,version_id=? WHERE id=?");
        query.optional(1, artwork);
        query.optional(2, version);
        query.text(3, file);
        query.done();
    }
    transaction.commit();
}

std::vector<MediaRecord> CollectionStore::Impl::indexed(const std::vector<fs::path>& paths, std::stop_token cancellation)
{
    std::vector<MediaRecord> result;
    result.reserve(paths.size());
    std::set<std::wstring> visited;
    Transaction transaction(database);
    for (const auto& input : paths) {
        if (cancellation.stop_requested()) throw LibraryError("Indexing was canceled.");
        const fs::path file = absolutePath(input);
        const std::wstring key = pathKey(file);
        if (!visited.insert(key).second) continue;
        Handle handle = readFile(file);
        FileState state = indexedState(file, handle.value, cancellation);
        MediaRecord record;
        {
            Statement exact(database, L"SELECT id,path,original_name,extension,artwork_id,version_id,bytes,modified,fields,identity FROM media WHERE path_key=?");
            exact.text(1, key);
            if (exact.row()) {
                if (!exact.text(9).empty() && !state.identity.empty() && exact.text(9) != state.identity) {
                    throw LibraryError("A different file now occupies an indexed path. Resolve it before changing metadata.");
                }
                record = media(exact);
            }
        }
        if (record.id.empty() && !state.identity.empty()) {
            Statement identity(database, L"SELECT id,path,original_name,extension,artwork_id,version_id,bytes,modified,fields FROM media WHERE identity=?");
            identity.text(1, state.identity);
            std::vector<MediaRecord> matches;
            while (identity.row()) matches.push_back(media(identity));
            if (matches.size() == 1 && !fileExists(matches.front().path)) record = std::move(matches.front());
        }
        if (record.id.empty() && state.identity.empty() && !state.hash.empty()) {
            Statement fingerprint(database, L"SELECT id,path,original_name,extension,artwork_id,version_id,bytes,modified,fields FROM media WHERE identity='' AND hash=? AND bytes=?");
            fingerprint.text(1, state.hash);
            fingerprint.integer(2, static_cast<std::int64_t>(state.bytes));
            std::vector<MediaRecord> matches;
            while (fingerprint.row()) matches.push_back(media(fingerprint));
            if (matches.size() == 1 && !fileExists(matches.front().path)) record = std::move(matches.front());
        }
        if (record.id.empty()) {
            record.id = guid();
            record.originalName = file.stem().wstring();
        }
        record.path = file;
        record.extension = file.extension().wstring();
        record.bytes = state.bytes;
        record.modified = state.modified;
        saveMedia(record, &state);
        result.push_back(std::move(record));
    }
    transaction.commit();
    return result;
}

std::vector<MediaRecord> CollectionStore::indexFiles(const std::vector<fs::path>& paths, std::stop_token cancellation)
{
    std::lock_guard lock(impl_->mutex);
    return impl_->indexed(paths, cancellation);
}

std::int64_t CollectionStore::nextArtworkNumber(const LibraryId& artist) const
{
    std::lock_guard lock(impl_->mutex);
    Statement query(impl_->database, artist.empty()
        ? L"SELECT COALESCE(MAX(number),0)+1 FROM artworks"
        : L"SELECT COALESCE(MAX(a.number),0)+1 FROM artworks a JOIN artwork_artists j ON j.artwork_id=a.id WHERE j.artist_id=?");
    if (!artist.empty()) query.text(1, artist);
    query.row();
    return query.integer(0);
}

std::int64_t CollectionStore::nextVersionNumber(const LibraryId& artwork) const
{
    std::lock_guard lock(impl_->mutex);
    Statement query(impl_->database, L"SELECT COALESCE(MAX(number),0)+1 FROM versions WHERE artwork_id=?");
    query.text(1, artwork);
    query.row();
    return query.integer(0);
}

LibraryId CollectionStore::savePreset(LibraryPreset preset)
{
    std::lock_guard lock(impl_->mutex);
    if (preset.name.empty()) throw LibraryError("Enter a preset name.");
    if (!Json::accept(utf8(preset.json))) throw LibraryError("The preset is not valid JSON.");
    if (preset.id.empty()) preset.id = guid();
    Statement query(impl_->database, L"INSERT INTO presets VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name,json=excluded.json");
    query.text(1, preset.id);
    query.text(2, preset.name);
    query.text(3, preset.json);
    query.done();
    return preset.id;
}

std::vector<LibraryPreset> CollectionStore::presets() const
{
    std::lock_guard lock(impl_->mutex);
    std::vector<LibraryPreset> result;
    Statement query(impl_->database, L"SELECT id,name,json FROM presets ORDER BY name,id");
    while (query.row()) result.push_back({query.text(0), query.text(1), query.text(2)});
    return result;
}

void CollectionStore::deletePreset(const LibraryId& preset)
{
    std::lock_guard lock(impl_->mutex);
    Statement query(impl_->database, L"DELETE FROM presets WHERE id=?");
    query.text(1, preset);
    query.done();
}

std::wstring CollectionStore::setting(std::wstring_view key, std::wstring_view fallback) const
{
    std::lock_guard lock(impl_->mutex);
    Statement query(impl_->database, L"SELECT value FROM settings WHERE key=?");
    query.text(1, key);
    return query.row() ? query.text(0) : std::wstring(fallback);
}

void CollectionStore::setSetting(std::wstring_view key, std::wstring_view value)
{
    std::lock_guard lock(impl_->mutex);
    Statement query(impl_->database, L"INSERT INTO settings VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    query.text(1, key);
    query.text(2, value);
    query.done();
}

namespace {

struct OperationLock {
    HANDLE handle;
    explicit OperationLock(HANDLE mutex) : handle(mutex) {
        const DWORD result = WaitForSingleObject(handle, 0);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            throw LibraryError("Another collection operation is in progress.");
        }
    }
    ~OperationLock() { ReleaseMutex(handle); }
};

class RenameCancelled : public std::exception {
public:
    const char* what() const noexcept override { return "Renaming was canceled."; }
};

bool sameIdentity(const FileState& expected, const FileState& actual)
{
    return expected.bytes == actual.bytes &&
        (expected.identity.empty() || expected.identity == actual.identity);
}

} // namespace

void CollectionStore::Impl::operationStatus(const LibraryId& operation, std::wstring_view status, std::wstring_view message)
{
    Statement query(database, L"UPDATE operations SET status=?,message=? WHERE id=?");
    query.text(1, status);
    query.text(2, message);
    query.text(3, operation);
    query.done();
}

void CollectionStore::Impl::stepStage(const LibraryId& operation, std::size_t index, std::wstring_view stage)
{
    Statement query(database, L"UPDATE rename_steps SET stage=? WHERE operation_id=? AND position=?");
    query.text(1, stage);
    query.text(2, operation);
    query.integer(3, static_cast<std::int64_t>(index));
    query.done();
}

void CollectionStore::Impl::updateMediaPaths(const std::vector<RenameStep>& steps, bool original)
{
    Transaction transaction(database);
    const auto temporaryKey = L"@rename:" + guid();
    for (std::size_t index = 0; index < steps.size(); ++index) {
        Statement clear(database, L"UPDATE media SET path_key=? WHERE id=?");
        clear.text(1, temporaryKey + L":" + std::to_wstring(index));
        clear.text(2, steps[index].fileId);
        clear.done();
    }
    for (const auto& step : steps) {
        const auto& target = original ? step.source : step.target;
        Statement query(database, L"UPDATE media SET path=?,path_key=?,extension=? WHERE id=?");
        query.text(1, target.wstring());
        query.text(2, pathKey(target));
        query.text(3, target.extension().wstring());
        query.text(4, step.fileId);
        query.done();
    }
    transaction.commit();
}

bool CollectionStore::Impl::rollback(const LibraryId& operation, std::vector<RenameStep>& steps, std::wstring& problem)
{
    try {
        // Stage every file again, including steps that already reached their
        // original path. This makes rollback safe for cycles and case changes.
        for (std::size_t index = 0; index < steps.size(); ++index) {
            auto& step = steps[index];
            const fs::path rollback = step.source.parent_path() / (L".viewery-rollback-" + guid() + L".tmp");
            Statement intent(database, L"UPDATE rename_steps SET temporary=?,previous=?,stage='rollback_intent' WHERE operation_id=? AND position=?");
            intent.text(1, rollback.wstring());
            intent.text(2, step.current.wstring());
            intent.text(3, operation);
            intent.integer(4, static_cast<std::int64_t>(index));
            intent.done();
            renameHandle(step.handle.value, rollback);
            step.current = rollback;
            step.temporary = rollback;
            stepStage(operation, index, L"rollback_staged");
        }
        for (std::size_t index = 0; index < steps.size(); ++index) {
            auto& step = steps[index];
            stepStage(operation, index, L"restore_intent");
            renameHandle(step.handle.value, step.source);
            step.current = step.source;
            stepStage(operation, index, L"restored");
        }
        updateMediaPaths(steps, true);
        operationStatus(operation, L"rolled_back", L"Исходные имена восстановлены");
        return true;
    } catch (const std::exception& error) {
        problem = wide(error.what());
        try { operationStatus(operation, L"recovery_required", problem); } catch (...) {}
        return false;
    }
}

RenameResult CollectionStore::Impl::perform(const std::vector<RenameProposal>& proposals, std::stop_token cancellation,
                                           const LibraryId& parent)
{
    RenameResult result;
    if (cancellation.stop_requested()) return {false, true, {}, L"Переименование отменено", {}};
    Statement pending(database, L"SELECT count(*) FROM operations WHERE status IN('planned','running','recovery_required')");
    pending.row();
    if (pending.integer(0)) return {false, false, {}, L"Сначала восстановите незавершённую операцию из истории", {}};
    const auto preview = previewRenames(proposals);
    for (const auto& row : preview) {
        if (row.proposal.enabled && !row.ready) return {false, false, {}, row.problem, {}};
    }
    std::vector<RenameStep> steps;
    std::set<std::wstring> identities;
    bool journalCommitted = false;
    try {
        for (const auto& row : preview) {
            if (!row.ready || row.proposal.source == row.target) continue;
            if (cancellation.stop_requested()) throw RenameCancelled();
            auto file = row.proposal.fileId;
            if (file.empty()) file = indexed({row.proposal.source}, {}).front().id;
            Handle handle = readFile(row.proposal.source, true);
            FileState state = fileState(handle.value);
            state.hash = hashFile(handle.value, cancellation);
            if (!parent.empty()) {
                Statement original(database, L"SELECT hash FROM rename_steps WHERE operation_id=? AND file_id=?");
                original.text(1, parent);
                original.text(2, file);
                if (!original.row() || original.text(0) != state.hash) {
                    throw LibraryError("A renamed file changed before undo. Refresh the operation.");
                }
            }
            if (!state.identity.empty() && !identities.insert(state.identity).second) {
                throw LibraryError("The selection contains multiple hard links to the same file.");
            }
            Statement known(database, L"SELECT path_key,identity FROM media WHERE id=?");
            known.text(1, file);
            if (!known.row() || known.text(0) != pathKey(row.proposal.source) ||
                (!known.text(1).empty() && known.text(1) != state.identity)) {
                throw LibraryError("The indexed file changed after preview. Refresh the selection.");
            }
            const auto temporary = row.proposal.source.parent_path() /
                (L".viewery-rename-" + guid() + L".tmp");
            steps.push_back({file, row.proposal.source, temporary, row.target, row.proposal.source, {},
                             std::move(state), L"planned", std::move(handle)});
        }
        if (steps.empty()) return {true, false, {}, L"Имена файлов уже соответствуют предпросмотру", {}};
        result.operationId = guid();
        {
            Transaction transaction(database);
            Statement operation(database, L"INSERT INTO operations VALUES(?,?,'planned','',?,?)");
            operation.text(1, result.operationId);
            operation.text(2, utcNow());
            operation.integer(3, static_cast<std::int64_t>(steps.size()));
            operation.optional(4, parent);
            operation.done();
            for (std::size_t index = 0; index < steps.size(); ++index) {
                const auto& step = steps[index];
                Statement query(database, L"INSERT INTO rename_steps VALUES(?,?,?,?,?,?,?,?,?,'planned','')");
                query.text(1, result.operationId);
                query.integer(2, static_cast<std::int64_t>(index));
                query.text(3, step.fileId);
                query.text(4, step.source.wstring());
                query.text(5, step.temporary.wstring());
                query.text(6, step.target.wstring());
                query.text(7, step.expected.identity);
                query.text(8, step.expected.hash);
                query.integer(9, static_cast<std::int64_t>(step.expected.bytes));
                query.done();
            }
            transaction.commit();
        }
        journalCommitted = true;
        operationStatus(result.operationId, L"running");
        for (std::size_t index = 0; index < steps.size(); ++index) {
            if (cancellation.stop_requested()) throw RenameCancelled();
            auto& step = steps[index];
            stepStage(result.operationId, index, L"stage_intent");
            renameHandle(step.handle.value, step.temporary);
            step.current = step.temporary;
            stepStage(result.operationId, index, L"staged");
        }
        for (std::size_t index = 0; index < steps.size(); ++index) {
            if (cancellation.stop_requested()) throw RenameCancelled();
            auto& step = steps[index];
            stepStage(result.operationId, index, L"target_intent");
            renameHandle(step.handle.value, step.target);
            step.current = step.target;
            stepStage(result.operationId, index, L"done");
        }
        {
            Transaction transaction(database);
            updateMediaPaths(steps, false);
            operationStatus(result.operationId, L"complete", L"Переименование завершено");
            if (!parent.empty()) operationStatus(parent, L"undone", L"Операция отменена");
            transaction.commit();
        }
        result.success = true;
        result.message = L"Переименовано файлов: " + std::to_wstring(steps.size());
        for (const auto& step : steps) result.changes.emplace_back(step.source, step.target);
    } catch (const std::exception& error) {
        result.cancelled = cancellation.stop_requested() || dynamic_cast<const RenameCancelled*>(&error) != nullptr;
        result.message = wide(error.what());
        if (journalCommitted) {
            std::wstring problem;
            if (rollback(result.operationId, steps, problem)) result.message += L". Исходные имена восстановлены.";
            else result.message += L". Требуется восстановление из истории: " + problem;
        }
    }
    return result;
}

RenameResult CollectionStore::rename(const std::vector<RenameProposal>& proposals, std::stop_token cancellation)
{
    std::lock_guard lock(impl_->mutex);
    OperationLock operation(impl_->operationMutex.value);
    return impl_->perform(proposals, cancellation);
}

std::vector<CollectionStore::Impl::RenameStep> CollectionStore::Impl::operationSteps(const LibraryId& operation)
{
    std::vector<RenameStep> result;
    Statement query(database, L"SELECT file_id,source,temporary,target,identity,hash,bytes,stage,previous FROM rename_steps WHERE operation_id=? ORDER BY position");
    query.text(1, operation);
    while (query.row()) {
        RenameStep step;
        step.fileId = query.text(0);
        step.source = query.text(1);
        step.temporary = query.text(2);
        step.target = query.text(3);
        step.expected.identity = query.text(4);
        step.expected.hash = query.text(5);
        step.expected.bytes = static_cast<std::uint64_t>(query.integer(6));
        step.stage = query.text(7);
        step.previous = query.text(8);
        result.push_back(std::move(step));
    }
    return result;
}

RenameResult CollectionStore::undo(const LibraryId& operation, std::stop_token cancellation)
{
    std::lock_guard lock(impl_->mutex);
    OperationLock exclusive(impl_->operationMutex.value);
    try {
        Statement status(impl_->database, L"SELECT status FROM operations WHERE id=?");
        status.text(1, operation);
        if (!status.row() || status.text(0) != L"complete") return {false, false, operation, L"Операция недоступна для отмены", {}};
        const auto steps = impl_->operationSteps(operation);
        std::vector<RenameProposal> proposals;
        for (const auto& step : steps) {
            Handle handle = readFile(step.target, true);
            const auto state = fileState(handle.value);
            if (!sameIdentity(step.expected, state) || hashFile(handle.value, cancellation) != step.expected.hash) {
                throw LibraryError("A renamed file was replaced or modified. Undo will not alter it.");
            }
            proposals.push_back({step.fileId, step.target, step.source.filename().wstring(), true});
        }
        return impl_->perform(proposals, cancellation, operation);
    } catch (const std::exception& error) { return {false, cancellation.stop_requested(), operation, wide(error.what()), {}}; }
}

RenameResult CollectionStore::recover(const LibraryId& operation, std::stop_token cancellation)
{
    std::lock_guard lock(impl_->mutex);
    OperationLock exclusive(impl_->operationMutex.value);
    RenameResult result{false, false, operation, {}, {}};
    try {
        Statement status(impl_->database, L"SELECT status FROM operations WHERE id=?");
        status.text(1, operation);
        if (!status.row() || (status.text(0) != L"planned" && status.text(0) != L"running" && status.text(0) != L"recovery_required")) {
            result.message = L"Восстановление для этой операции не требуется";
            return result;
        }
        auto steps = impl_->operationSteps(operation);
        if (steps.empty()) throw LibraryError("The operation journal contains no file steps.");
        std::set<std::wstring> opened;
        for (auto& step : steps) {
            if (cancellation.stop_requested()) return {false, true, operation, L"Восстановление отменено", {}};
            for (const auto& candidate : {step.temporary, step.target, step.source, step.previous}) {
                if (candidate.empty()) continue;
                const auto key = pathKey(candidate);
                if (opened.contains(key)) continue;
                if (!fileExists(candidate)) continue;
                Handle handle = readFile(candidate, true);
                const auto state = fileState(handle.value);
                if (sameIdentity(step.expected, state) && hashFile(handle.value, cancellation) == step.expected.hash) {
                    if (!opened.insert(key).second) throw LibraryError("Several journal entries point to the same file.");
                    step.current = candidate;
                    step.handle = std::move(handle);
                    break;
                }
            }
            if (!step.handle) throw LibraryError("An original file is missing or changed. Resolve the conflict before recovery.");
        }
        std::wstring problem;
        result.success = impl_->rollback(operation, steps, problem);
        result.message = result.success ? L"Исходные имена восстановлены" : problem;
        if (result.success) for (const auto& step : steps) result.changes.emplace_back(step.target, step.source);
    } catch (const std::exception& error) { result.message = wide(error.what()); result.cancelled = cancellation.stop_requested(); }
    return result;
}

std::vector<RenameOperation> CollectionStore::history() const
{
    std::lock_guard lock(impl_->mutex);
    std::vector<RenameOperation> result;
    Statement query(impl_->database, L"SELECT id,created,status,message,count FROM operations ORDER BY rowid DESC");
    while (query.row()) {
        const auto status = query.text(2);
        result.push_back({query.text(0), query.text(1), status, query.text(3),
                          static_cast<std::size_t>(query.integer(4)), status == L"complete"});
    }
    return result;
}

std::vector<std::pair<fs::path, fs::path>> CollectionStore::operationPaths(const LibraryId& operation) const
{
    std::lock_guard lock(impl_->mutex);
    std::vector<std::pair<fs::path, fs::path>> result;
    Statement query(impl_->database, L"SELECT source,target FROM rename_steps WHERE operation_id=? ORDER BY position");
    query.text(1, operation);
    while (query.row()) result.emplace_back(query.text(0), query.text(1));
    return result;
}

void CollectionStore::backup(const fs::path& output) const
{
    std::lock_guard lock(impl_->mutex);
    const auto target = absolutePath(output);
    if (pathKey(target) == pathKey(impl_->path)) throw LibraryError("Choose a separate database backup file.");
    Handle reservation(CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!reservation) throw LibraryError(utf8(systemError(L"Не удалось создать резервную копию базы")));
    reservation = Handle{};
    sqlite3* destination = nullptr;
    bool success = false;
    if (api().open(target.c_str(), &destination) == SQLITE_OK) {
        if (auto* backup = api().backupInit(destination, "main", impl_->database, "main")) {
            const int result = api().backupStep(backup, -1);
            const int completed = api().backupFinish(backup);
            success = result == SQLITE_DONE && completed == SQLITE_OK;
        }
    }
    if (destination) api().close(destination);
    if (!success) {
        DeleteFileW(target.c_str());
        throw LibraryError("The collection backup could not be completed.");
    }
}

namespace {

std::wstring jsonText(const Json& object, const char* field)
{
    const auto& value = object.at(field).get_ref<const std::string&>();
    if (value.size() > 524288) throw LibraryError("A metadata text field is too large.");
    return wide(value);
}

Json snapshotJson(const CollectionSnapshot& snapshot)
{
    Json document = {{"format", "Viewery.Collection"}, {"schema", 1}, {"artists", Json::array()},
        {"artworks", Json::array()}, {"versions", Json::array()}, {"files", Json::array()}, {"presets", Json::array()}};
    for (const auto& item : snapshot.artists) {
        document["artists"].push_back({{"id", utf8(item.id)}, {"name", utf8(item.name)},
            {"fileName", utf8(item.fileName)}, {"aliases", utf8(item.aliases)}, {"order", item.order},
            {"fields", fieldsJson(item.fields)}});
    }
    for (const auto& item : snapshot.artworks) {
        Json artists = Json::array();
        for (const auto& artist : item.artists) artists.push_back(utf8(artist));
        document["artworks"].push_back({{"id", utf8(item.id)}, {"artists", artists}, {"number", item.number},
            {"title", utf8(item.title)}, {"userTitle", utf8(item.userTitle)}, {"standalone", item.standalone},
            {"fields", fieldsJson(item.fields)}});
    }
    for (const auto& item : snapshot.versions) {
        document["versions"].push_back({{"id", utf8(item.id)}, {"artworkId", utf8(item.artworkId)},
            {"number", item.number}, {"title", utf8(item.title)}, {"main", item.main}, {"fields", fieldsJson(item.fields)}});
    }
    for (const auto& item : snapshot.files) {
        document["files"].push_back({{"id", utf8(item.id)}, {"path", utf8(item.path.wstring())},
            {"originalName", utf8(item.originalName)}, {"extension", utf8(item.extension)},
            {"artworkId", utf8(item.artworkId)}, {"versionId", utf8(item.versionId)}, {"bytes", item.bytes},
            {"modified", item.modified}, {"fields", fieldsJson(item.fields)}});
    }
    return document;
}

void writeJsonFile(const fs::path& output, const Json& document)
{
    const std::string text = document.dump(2);
    if (text.size() > 32 * 1024 * 1024) throw LibraryError("The metadata export exceeds 32 MiB.");
    Handle file(CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) throw LibraryError(utf8(systemError(L"Не удалось создать файл экспорта")));
    DWORD written = 0;
    if (!WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written != text.size() || !FlushFileBuffers(file.value)) {
        file = Handle{};
        DeleteFileW(output.c_str());
        throw LibraryError("The metadata export could not be written.");
    }
}

} // namespace

void CollectionStore::exportMetadata(const fs::path& output) const
{
    std::lock_guard lock(impl_->mutex);
    Json document = snapshotJson(snapshot());
    for (const auto& item : presets()) {
        document["presets"].push_back({{"id", utf8(item.id)}, {"name", utf8(item.name)},
                                       {"json", Json::parse(utf8(item.json))}});
    }
    document["settings"] = Json::object();
    Statement settings(impl_->database, L"SELECT key,value FROM settings");
    while (settings.row()) document["settings"][utf8(settings.text(0))] = utf8(settings.text(1));
    std::map<LibraryId, std::pair<std::wstring, std::wstring>> identities;
    Statement files(impl_->database, L"SELECT id,identity,hash FROM media");
    while (files.row()) identities.emplace(files.text(0), std::pair{files.text(1), files.text(2)});
    for (auto& file : document["files"]) {
        const auto& identity = identities.at(jsonText(file, "id"));
        file["fileIdentity"] = utf8(identity.first);
        file["sha256"] = utf8(identity.second);
    }
    writeJsonFile(absolutePath(output), document);
}

void CollectionStore::importMetadata(const fs::path& input)
{
    std::lock_guard lock(impl_->mutex);
    OperationLock operation(impl_->operationMutex.value);
    try {
        const auto size = fs::file_size(input);
        if (size > 32 * 1024 * 1024) throw LibraryError("The metadata file exceeds 32 MiB.");
        std::ifstream stream(input, std::ios::binary);
        if (!stream) throw LibraryError("The metadata file could not be opened.");
        const Json document = Json::parse(stream);
        if (document.at("format") != "Viewery.Collection" || document.at("schema") != 1) {
            throw LibraryError("This is not a supported Viewery collection export.");
        }
        for (const auto* list : {"artists", "artworks", "versions", "files", "presets"}) {
            const auto& items = document.at(list);
            if (!items.is_array() || items.size() > 100000) throw LibraryError("Invalid metadata entity list.");
            std::set<LibraryId> ids;
            for (const auto& item : items) {
                auto id = jsonText(item, "id");
                if (id.empty() || id.size() > 100 || !ids.insert(std::move(id)).second) {
                    throw LibraryError("Metadata contains a duplicate or invalid identifier.");
                }
            }
        }
        backup(impl_->path.parent_path() / (L"collection-before-import-" + guid() + L".sqlite3"));
        Transaction transaction(impl_->database);
        for (const auto& item : document.at("artists")) {
            saveArtist({jsonText(item, "id"), jsonText(item, "name"), jsonText(item, "fileName"),
                        jsonText(item, "aliases"), item.at("order").get<std::int64_t>(), decodeFields(item.at("fields"))});
        }
        for (const auto& item : document.at("artworks")) {
            Artwork artwork{jsonText(item, "id"), {}, item.at("number").get<std::int64_t>(), jsonText(item, "title"),
                jsonText(item, "userTitle"), item.at("standalone").get<bool>(), decodeFields(item.at("fields"))};
            for (const auto& artist : item.at("artists")) artwork.artists.push_back(wide(artist.get<std::string>()));
            saveArtwork(std::move(artwork));
        }
        for (const auto& item : document.at("versions")) {
            saveVersion({jsonText(item, "id"), jsonText(item, "artworkId"), item.at("number").get<std::int64_t>(),
                jsonText(item, "title"), item.at("main").get<bool>(), decodeFields(item.at("fields"))});
        }
        for (const auto& item : document.at("files")) {
            MediaRecord file{jsonText(item, "id"), absolutePath(jsonText(item, "path")), jsonText(item, "originalName"),
                jsonText(item, "extension"), jsonText(item, "artworkId"), jsonText(item, "versionId"),
                item.at("bytes").get<std::uint64_t>(), item.at("modified").get<std::int64_t>(), decodeFields(item.at("fields"))};
            if (file.bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                file.extension != file.path.extension().wstring()) throw LibraryError("Invalid media file metadata.");
            FileState state{jsonText(item, "fileIdentity"), jsonText(item, "sha256"), file.bytes, file.modified};
            if (fileExists(file.path)) {
                const auto handle = readFile(file.path);
                const auto actual = fileState(handle.value);
                if (!sameIdentity(state, actual)) {
                    throw LibraryError("A metadata path refers to a different file. Import was canceled.");
                }
                state = actual;
            }
            if (!file.versionId.empty()) {
                Statement version(impl_->database, L"SELECT artwork_id FROM versions WHERE id=?");
                version.text(1, file.versionId);
                if (!version.row() || version.text(0) != file.artworkId) throw LibraryError("A file references a version of another work.");
            }
            impl_->saveMedia(file, &state);
        }
        for (const auto& item : document.at("presets")) {
            savePreset({jsonText(item, "id"), jsonText(item, "name"), wide(item.at("json").dump())});
        }
        for (const auto& item : document.at("settings").items()) setSetting(wide(item.key()), wide(item.value().get<std::string>()));
        transaction.commit();
    } catch (const Json::exception& error) { throw LibraryError(std::string("Invalid collection JSON: ") + error.what()); }
}

} // namespace viewery

