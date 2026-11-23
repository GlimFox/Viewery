// Keep the release parser and command-line boundary private to the production translation unit.
// Including it here exercises those exact routines without exposing internal APIs to the app.
#include "../src/updater.cpp"

#include <iostream>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    try {
        using namespace viewery;
        const auto beta1 = version("v0.2.0-beta.1");
        const auto beta2 = version("0.2.0-beta.2");
        const auto beta10 = version("0.2.0-beta.10");
        const auto stable = version("v0.2.0");
        const auto newer = version("v0.2.1");
        require(beta1 && beta2 && beta10 && stable && newer, "valid release versions rejected");
        require(compare(*beta1,*beta2) < 0 && compare(*beta2,*beta10) < 0, "numeric beta precedence");
        require(compare(*beta10,*stable) < 0 && compare(*stable,*newer) < 0, "stable/new base precedence");
        require(compare(*stable,*version("0.2.0+build123")) == 0, "build metadata precedence");
        const auto unicode = Json::parse(R"({"body":"Кириллица \uD83D\uDE00","tag_name":"v0.2.0-beta.2"})");
        require(std::wstring_view(winrt::to_hstring(jsonText(unicode,"body"))) == L"Кириллица \U0001F600", "UTF-8 release notes conversion");
        require(jsonText(Json::parse(R"({"body":null})"),"body").empty(), "nullable release body rejected");
        require(jsonText(unicode,"missing").empty(), "missing optional release field rejected");
        for (const auto* invalid : {"", "updates-beta", "v0.2", "v0.2.0.4", "01.2.3", "0.2.0-", "0.2.0-beta.01", "0.2.0-beta.", "0.2.0-../evil", "0.2.0+", "0.2.0+x+y", "0.2.0+../evil"}) {
            require(!version(invalid), "invalid release accepted");
        }
        require(allowedInstallerUri(L"https://github.com/GlimFox/Viewery/releases/download/updates-beta/Viewery.appinstaller"), "beta feed rejected");
        require(allowedInstallerUri(L"https://github.com/GlimFox/Viewery/releases/download/updates-stable/Viewery.appinstaller"), "stable feed rejected");
        for (const auto* untrusted : {L"http://github.com/GlimFox/Viewery/releases/download/updates-beta/Viewery.appinstaller",
                                      L"https://github.com/other/Viewery/releases/download/updates-beta/Viewery.appinstaller",
                                      L"file:///C:/unsafe.appinstaller",
                                      L"https://github.com/GlimFox/Viewery/releases/download/updates-beta/Viewery.appinstaller?redirect=evil"}) {
            require(!allowedInstallerUri(untrusted), "untrusted installer source accepted");
        }
        require(quoteArgument(L"C:\\Folder Name\\") == L"\"C:\\Folder Name\\\\\"", "trailing backslash command quoting");
        require(quoteArgument(L"a\"b") == L"\"a\\\"b\"", "embedded quote command quoting");
        require(quoteArgument(L"$(Get-Content secrets)") == L"\"$(Get-Content secrets)\"", "literal command argument changed");
        require(runtime::mediaToolPath(L"../arbitrary.exe").empty(), "media path traversal accepted");
        require(runtime::mediaToolPath(L"powershell.exe").empty(), "non-media executable accepted");
        require(!runtime::userDataDirectory().empty(), "LocalAppData lookup failed");
        std::stop_source canceled;
        canceled.request_stop();
        bool cancellationObserved = false;
        try { (void) releaseFeed(canceled.get_token()); }
        catch (const std::exception&) { cancellationObserved = true; }
        require(cancellationObserved, "canceled network check continued");
        UpdateService service(nullptr,WM_APP + 90);
        require(!service.install(), "update installed without successful authenticated availability check");
        require(!service.busy(), "rejected update left service busy");
        std::cout << "Update version, trusted-source, command quoting and runtime path checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
