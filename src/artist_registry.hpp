#pragma once

#include "library.hpp"
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>

namespace viewery {

class ArtistRegistry {
public:
    using BeforeFilesChange = std::function<void()>;
    ArtistRegistry(HWND owner, UINT changedMessage, BeforeFilesChange beforeChange,
                   std::filesystem::path database = {});
    ~ArtistRegistry();
    ArtistRegistry(const ArtistRegistry&) = delete;
    ArtistRegistry& operator=(const ArtistRegistry&) = delete;
    void show(UINT dpi = 0);
    void hide();
    HWND window() const;
    bool processMessage(MSG& message);
    bool busy() const;
    void cancel();
    void refresh();
    std::optional<RenameResult> takeFileChanges();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace viewery
