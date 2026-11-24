#include "updater.hpp"
#include "runtime_paths.hpp"
#include "third_party/json.hpp"

#include <winhttp.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifndef VIEWERY_VERSION
#define VIEWERY_VERSION "0.2.0-beta.9"
#endif

namespace viewery {
namespace {

using namespace std::chrono_literals;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Management::Deployment;
using Json = nlohmann::json;
namespace fs = std::filesystem;

struct Apartment {
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};

constexpr wchar_t settingsKey[] = L"Software\\Viewery";
constexpr wchar_t automaticValue[] = L"AutomaticUpdateChecks";
constexpr wchar_t repositoryPath[] = L"/repos/GlimFox/Viewery/releases?per_page=30";
constexpr wchar_t releasePrefix[] = L"https://github.com/GlimFox/Viewery/releases/";
constexpr wchar_t packageIdentity[] = L"Viewery.Explorer";
constexpr wchar_t packagePublisher[] = L"CN=Viewery";

std::string_view jsonText(const Json& object, const char* key)
{
    const auto field = object.find(key);
    return field != object.end() && field->is_string() ? std::string_view(field->get_ref<const std::string&>()) : std::string_view{};
}

struct Version {
    std::array<unsigned, 3> parts{};
    std::vector<std::string> prerelease;
};

bool digits(std::string_view value)
{
    return !value.empty() && value.find_first_not_of("0123456789") == std::string_view::npos;
}

std::optional<Version> version(std::string_view text)
{
    if (text.starts_with('v')) text.remove_prefix(1);
    if (text.empty() || text.size() > 128) return {};
    const auto plus = text.find('+');
    if (plus != std::string_view::npos) {
        auto metadata = text.substr(plus + 1);
        if (metadata.empty()) return {};
        while (!metadata.empty()) {
            const auto dot = metadata.find('.');
            const auto part = metadata.substr(0, dot);
            if (part.empty() || part.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") !=
                                    std::string_view::npos) return {};
            if (dot == std::string_view::npos) break;
            metadata.remove_prefix(dot + 1);
            if (metadata.empty()) return {};
        }
        text = text.substr(0, plus);
    }
    Version parsed;
    const auto dash = text.find('-');
    std::string_view numeric = text.substr(0, dash);
    for (unsigned index = 0; index != parsed.parts.size(); ++index) {
        const auto dot = numeric.find('.');
        const auto part = numeric.substr(0, dot);
        if (!digits(part) || part.size() > 6 || (part.size() > 1 && part[0] == '0')) return {};
        for (char character : part) parsed.parts[index] = parsed.parts[index] * 10 + character - '0';
        if (index == parsed.parts.size() - 1) {
            if (dot != std::string_view::npos) return {};
        } else {
            if (dot == std::string_view::npos) return {};
            numeric.remove_prefix(dot + 1);
        }
    }
    if (dash != std::string_view::npos) {
        auto suffix = text.substr(dash + 1);
        if (suffix.empty()) return {};
        while (!suffix.empty()) {
            const auto dot = suffix.find('.');
            const auto part = suffix.substr(0, dot);
            if (part.empty() || part.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") !=
                                    std::string_view::npos || (digits(part) && part.size() > 1 && part[0] == '0')) return {};
            parsed.prerelease.emplace_back(part);
            if (dot == std::string_view::npos) break;
            suffix.remove_prefix(dot + 1);
            if (suffix.empty()) return {};
        }
    }
    return parsed;
}

int compare(const Version& left, const Version& right)
{
    if (left.parts != right.parts) return left.parts < right.parts ? -1 : 1;
    if (left.prerelease.empty() || right.prerelease.empty()) {
        if (left.prerelease.empty() == right.prerelease.empty()) return 0;
        return left.prerelease.empty() ? 1 : -1;
    }
    const auto count = std::min(left.prerelease.size(), right.prerelease.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& a = left.prerelease[index];
        const auto& b = right.prerelease[index];
        if (a == b) continue;
        const bool aNumeric = digits(a), bNumeric = digits(b);
        if (aNumeric != bNumeric) return aNumeric ? -1 : 1;
        if (aNumeric && a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        return a < b ? -1 : 1;
    }
    return left.prerelease.size() < right.prerelease.size() ? -1 :
           left.prerelease.size() > right.prerelease.size() ? 1 : 0;
}

struct InternetHandle {
    explicit InternetHandle(HINTERNET handle = nullptr) : handle(handle) {}
    ~InternetHandle() { if (handle) WinHttpCloseHandle(handle); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET handle;
};

class ReleaseCheckError : public std::runtime_error {
public:
    explicit ReleaseCheckError(std::wstring message):std::runtime_error("Release check failed"),message(std::move(message)) {}
    std::wstring message;
};

[[noreturn]] void failReleaseNetwork(DWORD code)
{
    throw ReleaseCheckError(L"Не удалось обратиться к GitHub (WinHTTP "+std::to_wstring(code)+L"). Повторите проверку позже.");
}

std::wstring releaseHttpFailure(DWORD status)
{
    if(status==403||status==429)
        return L"GitHub отклонил запрос или ограничил частоту проверок (HTTP "+std::to_wstring(status)+L"). Повторите позже.";
    return L"Сервис обновлений GitHub вернул HTTP "+std::to_wstring(status)+L". Повторите проверку позже.";
}

std::string releaseFeed(std::stop_token stop)
{
    if(stop.stop_requested()) throw ReleaseCheckError(L"Проверка обновлений отменена.");
    InternetHandle session(WinHttpOpen(L"Viewery/" L"0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle) failReleaseNetwork(GetLastError());
    WinHttpSetTimeouts(session.handle, 3500, 3500, 3500, 6500);
    InternetHandle connection(WinHttpConnect(session.handle, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.handle) failReleaseNetwork(GetLastError());
    InternetHandle request(WinHttpOpenRequest(connection.handle, L"GET", repositoryPath, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.handle) failReleaseNetwork(GetLastError());
    const DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, const_cast<DWORD*>(&redirects), sizeof(redirects));
    constexpr wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (stop.stop_requested() || !WinHttpSendRequest(request.handle, headers, static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.handle, nullptr)) {
        failReleaseNetwork(GetLastError());
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        failReleaseNetwork(GetLastError());
    }
    if(status!=200) throw ReleaseCheckError(releaseHttpFailure(status));
    std::string body;
    std::array<char, 16384> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            failReleaseNetwork(GetLastError());
        }
        if (!read) return body;
        if (body.size() + read > 2 * 1024 * 1024) throw ReleaseCheckError(L"Сервис обновлений вернул слишком большой ответ.");
        body.append(buffer.data(), read);
    }
    throw ReleaseCheckError(stop.stop_requested()?L"Проверка обновлений отменена.":L"GitHub не ответил вовремя. Повторите проверку позже.");
}

UpdateInfo releaseInfo(const Json& releases,std::string_view installedVersion)
{
    const auto installed=version(installedVersion);
    if(!installed) throw std::invalid_argument("Invalid app version");
    if(!releases.is_array()) throw ReleaseCheckError(L"Сервис обновлений вернул некорректные данные.");
    UpdateInfo result;
    std::optional<Version> newest;
    for(const auto& release:releases) {
        if(!release.is_object()) continue;
        const auto draft=release.find("draft"),prerelease=release.find("prerelease");
        if(draft==release.end()||!draft->is_boolean()||draft->get<bool>()||
           (prerelease!=release.end()&&!prerelease->is_boolean())||
           (installed->prerelease.empty()&&prerelease!=release.end()&&prerelease->get<bool>())) continue;
        auto candidate=version(jsonText(release,"tag_name"));
        if(!candidate||(installed->prerelease.empty()&&!candidate->prerelease.empty())||
           compare(*candidate,*installed)<0||(newest&&compare(*candidate,*newest)<=0)) continue;
        const auto url=winrt::to_hstring(jsonText(release,"html_url"));
        if(!std::wstring_view(url).starts_with(releasePrefix)) continue;
        newest=std::move(candidate);
        result.version=winrt::to_hstring(jsonText(release,"tag_name"));
        result.notes=winrt::to_hstring(jsonText(release,"body"));
        if(result.notes.size()>12000) result.notes.resize(12000);
        result.releaseUrl=url;
    }
    if(newest&&compare(*newest,*installed)>0) {
        result.status=UpdateStatus::available;
        result.message=L"Доступна версия "+result.version;
    } else {
        result.status=UpdateStatus::current;
        result.message=L"Установлена актуальная версия "+std::wstring(winrt::to_hstring(installedVersion));
    }
    return result;
}

bool allowedInstallerUri(std::wstring_view value)
{
    return value == L"https://github.com/GlimFox/Viewery/releases/download/updates-beta/Viewery.appinstaller" ||
           value == L"https://github.com/GlimFox/Viewery/releases/download/updates-stable/Viewery.appinstaller";
}

Package installedPackage()
{
    try {
        const auto current = Package::Current();
        if (current.Id().Name() != packageIdentity || current.Id().Publisher() != packagePublisher) return nullptr;
        PackageManager manager;
        return manager.FindPackageForUser(L"", current.Id().FullName());
    } catch (const winrt::hresult_error&) {
        return nullptr;
    }
}

template<class Operation>
bool awaitOperation(const Operation& operation, std::stop_token stop, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (operation.Status() == AsyncStatus::Started) {
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
            operation.Cancel();
            return false;
        }
        operation.wait_for(100ms);
    }
    return operation.Status() == AsyncStatus::Completed;
}

std::wstring quoteArgument(std::wstring_view input)
{
    std::wstring output(1, L'"');
    std::size_t backslashes = 0;
    for (wchar_t character : input) {
        if (character == L'\\') { ++backslashes; continue; }
        output.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
        backslashes = 0;
        if (character == L'"') output += L'\\';
        output += character;
    }
    output.append(backslashes * 2, L'\\');
    output += L'"';
    return output;
}

bool preparePortableUpdate(const std::wstring& version,std::stop_token stop,HANDLE& helper)
{
    const auto directory=runtime::executableDirectory();
    const auto script=directory/L"Update-Portable.ps1";
    const auto eventName=L"Local\\Viewery.Update."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str());
    if(!ready) return false;
    wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    const auto powershell=fs::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
    auto command=quoteArgument(powershell.native())+L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+quoteArgument(script.native())+
        L" -Version "+quoteArgument(version)+L" -Destination "+quoteArgument(directory.native())+L" -ParentPid "+std::to_wstring(GetCurrentProcessId())+L" -ReadyEvent "+quoteArgument(eventName);
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    if(!CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process)) {CloseHandle(ready);return false;}
    CloseHandle(process.hThread);
    HANDLE handles[]{ready,process.hProcess};bool staged=false;
    const auto deadline=std::chrono::steady_clock::now()+5min;
    while(!stop.stop_requested()&&std::chrono::steady_clock::now()<deadline) {
        const auto result=WaitForMultipleObjects(2,handles,FALSE,100);
        if(result==WAIT_OBJECT_0) {staged=true;break;}
        if(result!=WAIT_TIMEOUT) break;
    }
    if(!staged) {TerminateProcess(process.hProcess,ERROR_CANCELLED);WaitForSingleObject(process.hProcess,3000);}
    if(staged) helper=process.hProcess;else CloseHandle(process.hProcess);
    CloseHandle(ready);return staged;
}

bool runMediaInstaller(std::stop_token stop)
{
    const auto script = runtime::executableDirectory() / L"Get-MediaTools.ps1";
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const auto destination = runtime::mediaToolsDirectory();
    if (destination.empty()) return false;
    const bool playback = !runtime::mediaToolPath(L"libmpv-2.dll").empty() || !runtime::mediaToolPath(L"mpv-2.dll").empty();
    const bool converter = !runtime::mediaToolPath(L"ffmpeg.exe").empty();
    if (playback && converter) return true;
    const wchar_t* component = !playback && !converter ? L"All" : playback ? L"Converter" : L"Playback";
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) return false;
    const fs::path powershell = fs::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
    std::wstring command = quoteArgument(powershell.native()) +
        L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + quoteArgument(script.native()) +
        L" -Destination " + quoteArgument(destination.native()) + L" -Component " + component;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, script.parent_path().c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    DWORD exitCode = 1;
    const auto deadline = std::chrono::steady_clock::now() + 10min;
    for (;;) {
        const DWORD waited = WaitForSingleObject(process.hProcess, 100);
        if (waited == WAIT_OBJECT_0) { GetExitCodeProcess(process.hProcess, &exitCode); break; }
        if (waited == WAIT_FAILED || stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, 3000);
            break;
        }
    }
    CloseHandle(process.hProcess);
    return exitCode == 0 && (!runtime::mediaToolPath(L"libmpv-2.dll").empty() ||
                             !runtime::mediaToolPath(L"mpv-2.dll").empty()) &&
           !runtime::mediaToolPath(L"ffmpeg.exe").empty();
}

} // namespace

UpdateService::UpdateService(HWND owner, UINT message) : owner_(owner), message_(message) {}

UpdateService::~UpdateService()
{
    worker_.request_stop();
    if(worker_.joinable()) worker_.join();
    if(portableHelper_) CloseHandle(portableHelper_);
}

void UpdateService::cancelPreparedUpdate()
{
    if(portableHelper_) {TerminateProcess(portableHelper_,ERROR_CANCELLED);WaitForSingleObject(portableHelper_,3000);CloseHandle(portableHelper_);portableHelper_=nullptr;}
}

bool UpdateService::busy() const { return busy_.load(); }

UpdateInfo UpdateService::take() const
{
    std::lock_guard guard(mutex_);
    return result_;
}

void UpdateService::publish(UpdateInfo result)
{
    {
        std::lock_guard guard(mutex_);
        result_ = std::move(result);
    }
    busy_ = false;
    PostMessageW(owner_, message_, 0, 0);
}

bool UpdateService::check(bool manual)
{
    if (busy_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard guard(mutex_);
        result_ = {};
        result_.status = UpdateStatus::checking;
        result_.message=L"Проверка обновлений…";
        result_.manual = manual;
        installerUri_.clear();
    }
    worker_ = std::jthread([this, manual](std::stop_token stop) {
        UpdateInfo result;
        result.manual = manual;
        try {
            Apartment apartment;
            if(!version(VIEWERY_VERSION)) throw std::invalid_argument("Invalid app version");
            const Json releases = Json::parse(releaseFeed(stop));
            result=releaseInfo(releases,VIEWERY_VERSION);
            result.manual=manual;
            if (result.status==UpdateStatus::available) {
                const auto package = installedPackage();
                if (package) {
                    const auto installer = package.GetAppInstallerInfo();
                    if (installer && allowedInstallerUri(installer.Uri().AbsoluteUri())) {
                        const auto operation = package.CheckUpdateAvailabilityAsync();
                        if (awaitOperation(operation, stop, 20s)) {
                            const auto availability = operation.GetResults().Availability();
                            result.canInstall = availability == PackageUpdateAvailability::Available ||
                                                availability == PackageUpdateAvailability::Required;
                            if (result.canInstall) {
                                std::lock_guard guard(mutex_);
                                installerUri_ = installer.Uri().AbsoluteUri();
                            }
                        }
                    }
                }
                if (!result.canInstall) {
                    if(!package&&fs::exists(runtime::executableDirectory()/L"Update-Portable.ps1")) {
                        result.canInstall=true;result.message+=L". Можно скачать и установить обновление автоматически.";
                    } else result.message+=L". Скачайте новую версию со страницы релиза.";
                }
            }
        } catch(const std::invalid_argument&) {
            result.status=UpdateStatus::failed;
            result.message=L"В сборке Viewery указан некорректный номер версии. Установите официальную сборку из раздела релизов.";
        } catch(const ReleaseCheckError& error) {
            result.status=UpdateStatus::failed;result.message=error.message;
        } catch(const Json::exception&) {
            result.status=UpdateStatus::failed;result.message=L"Сервис обновлений вернул некорректные данные. Повторите проверку позже.";
        } catch(const winrt::hresult_error& error) {
            wchar_t code[16]{};swprintf_s(code,L"0x%08X",static_cast<unsigned>(error.code().value));
            result.status=UpdateStatus::failed;result.message=L"Ошибка Windows при проверке обновлений ("+std::wstring(code)+L").";
        } catch (...) {
            result.status = UpdateStatus::failed;
            result.message = L"Не удалось проверить обновления. Проверьте подключение и повторите позже.";
        }
        if (!stop.stop_requested()) publish(std::move(result));
        else busy_ = false;
    });
    return true;
}

bool UpdateService::install()
{
    std::wstring uri;
    {
        std::lock_guard guard(mutex_);
        if (!result_.canInstall) return false;
        uri = installerUri_;
        if(uri.empty()) uri=L"portable:"+result_.version;
        else if(!allowedInstallerUri(uri)) return false;
    }
    if (busy_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard guard(mutex_);
        result_.status = UpdateStatus::installing;
        result_.message = L"Загрузка и проверка обновления…";
    }
    worker_ = std::jthread([this, uri](std::stop_token stop) {
        UpdateInfo result;
        result.manual = true;
        try {
            if(uri.starts_with(L"portable:")) {
                if(!preparePortableUpdate(uri.substr(9),stop,portableHelper_)) throw std::runtime_error("Portable update preparation failed");
                result.status=UpdateStatus::portableReady;result.message=L"Обновление проверено. Перезапуск…";
                if(!stop.stop_requested()) publish(std::move(result));else busy_=false;
                return;
            }
            Apartment apartment;
            PackageManager manager;
            auto operation = manager.RequestAddPackageByAppInstallerFileAsync(Uri(uri),
                AddPackageByAppInstallerOptions::ForceTargetAppShutdown, nullptr);
            if (!awaitOperation(operation, stop, 600s)) throw std::runtime_error("Update interrupted");
            const auto deployment = operation.GetResults();
            if (FAILED(deployment.ExtendedErrorCode())) throw std::runtime_error("Update rejected");
            result.status = UpdateStatus::current;
            result.message = L"Обновление установлено. Перезапустите Viewery.";
        } catch (...) {
            result.status = UpdateStatus::failed;
            result.message = uri.starts_with(L"portable:") ?
                L"Не удалось подготовить обновление. Проверьте интернет и доступ к папке Viewery. Текущая версия сохранена." :
                L"Windows не установила обновление. Подпись или загрузка не прошли проверку. Текущая версия сохранена.";
        }
        if (!stop.stop_requested()) publish(std::move(result));
        else busy_ = false;
    });
    return true;
}

bool UpdateService::installMediaTools()
{
    if (busy_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard guard(mutex_);
        result_ = {};
        result_.status = UpdateStatus::mediaInstalling;
        result_.message = L"Загрузка медиакомпонентов…";
    }
    worker_ = std::jthread([this](std::stop_token stop) {
        UpdateInfo result;
        result.manual = true;
        result.status = runMediaInstaller(stop) ? UpdateStatus::mediaReady : UpdateStatus::failed;
        result.message = result.status == UpdateStatus::mediaReady ? L"Медиакомпоненты установлены." :
            L"Не удалось установить медиакомпоненты. Проверьте интернет и наличие Get-MediaTools.ps1 рядом с программой.";
        if (!stop.stop_requested()) publish(std::move(result));
        else busy_ = false;
    });
    return true;
}

bool UpdateService::automaticChecks()
{
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, settingsKey, automaticValue, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

bool UpdateService::setAutomaticChecks(bool enabled)
{
    const DWORD value = enabled;
    return RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, automaticValue, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

} // namespace viewery
