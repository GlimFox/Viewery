#include "media_types.hpp"

#include <windows.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <wrl.h>

#include <atomic>
#include <memory>
#include <new>
#include <string>

namespace {

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

constexpr GUID commandClass{0x9ab9ff5c, 0xc139, 0x4fbc, {0x93, 0x7b, 0x4a, 0x73, 0xbb, 0x25, 0x22, 0xe7}};
constexpr wchar_t titleText[] = L"Открыть во Viewery";
std::atomic_ulong objects{0};
std::atomic_ulong serverLocks{0};
HINSTANCE instance = nullptr;

class ServerObject {
public:
    ServerObject() { ++objects; }
    ~ServerObject() { --objects; }
};

std::filesystem::path vieweryPath()
{
    std::wstring modulePath(32768, L'\0');
    const DWORD copied = GetModuleFileNameW(instance, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (copied == 0 || copied >= modulePath.size()) return {};
    modulePath.resize(copied);
    return std::filesystem::path(modulePath).replace_filename(L"Viewery.exe");
}

std::filesystem::path selectedMedia(IShellItemArray* selection)
{
    DWORD count = 0;
    if (!selection || FAILED(selection->GetCount(&count)) || count != 1) return {};

    ComPtr<IShellItem> item;
    if (FAILED(selection->GetItemAt(0, &item))) return {};
    if (!item) return {};
    constexpr SFGAOF mask = SFGAO_FILESYSTEM | SFGAO_FOLDER;
    SFGAOF attributes = 0;
    if (FAILED(item->GetAttributes(mask, &attributes)) || (attributes & mask) != SFGAO_FILESYSTEM) return {};

    PWSTR name = nullptr;
    const HRESULT result = item->GetDisplayName(SIGDN_FILESYSPATH, &name);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(name, CoTaskMemFree);
    if (FAILED(result) || !owned) return {};
    const std::filesystem::path path(name);
    return viewery::classifyMedia(path) ? path : std::filesystem::path{};
}

class OpenCommand final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
public:
    IFACEMETHODIMP GetTitle(IShellItemArray*, PWSTR* title) override
    {
        return title ? SHStrDupW(titleText, title) : E_POINTER;
    }

    IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* icon) override
    {
        if (!icon) return E_POINTER;
        *icon = nullptr;
        try {
            const auto exe = vieweryPath();
            if (exe.empty()) return E_NOTIMPL;
            const std::wstring path = exe.native() + L",0";
            return SHStrDupW(path.c_str(), icon);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP GetToolTip(IShellItemArray*, PWSTR* tip) override
    {
        if (!tip) return E_POINTER;
        *tip = nullptr;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP GetCanonicalName(GUID* name) override
    {
        if (!name) return E_POINTER;
        *name = commandClass;
        return S_OK;
    }

    IFACEMETHODIMP GetState(IShellItemArray* selection, BOOL, EXPCMDSTATE* state) override
    {
        if (!state) return E_POINTER;
        *state = ECS_HIDDEN;
        try {
            if (!selectedMedia(selection).empty()) *state = ECS_ENABLED;
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP Invoke(IShellItemArray* selection, IBindCtx*) override
    {
        try {
            const auto file = selectedMedia(selection);
            if (file.empty()) return E_INVALIDARG;
            const auto exe = vieweryPath();
            if (exe.empty()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

            // File system names cannot contain quotes, so both arguments have unambiguous boundaries.
            auto cmd = L"\"" + exe.native() + L"\" --open \"" + file.native() + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                exe.parent_path().c_str(), &startup, &process)) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override
    {
        if (!flags) return E_POINTER;
        *flags = ECF_DEFAULT;
        return S_OK;
    }

    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override
    {
        if (!commands) return E_POINTER;
        *commands = nullptr;
        return E_NOTIMPL;
    }

private:
    ServerObject object_;
};

class CommandFactory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    IFACEMETHODIMP CreateInstance(IUnknown* aggregate, REFIID iid, void** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (aggregate) return CLASS_E_NOAGGREGATION;
        const auto command = Make<OpenCommand>();
        return command ? command->QueryInterface(iid, result) : E_OUTOFMEMORY;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) ++serverLocks;
        else --serverLocks;
        return S_OK;
    }

private:
    ServerObject object_;
};

}

extern "C" BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) instance = dll;
    return TRUE;
}

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    if (clsid != commandClass) return CLASS_E_CLASSNOTAVAILABLE;
    const auto factory = Make<CommandFactory>();
    return factory ? factory->QueryInterface(iid, result) : E_OUTOFMEMORY;
}

STDAPI DllCanUnloadNow()
{
    return objects.load() == 0 && serverLocks.load() == 0 ? S_OK : S_FALSE;
}
