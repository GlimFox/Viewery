#pragma once

#include "library.hpp"
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>

namespace viewery {

class RenameStudio {
public:
    enum class CollectionAction { backup, exportMetadata, importMetadata };
    static constexpr UINT closeCommandId = 1051;
    using BeforeFilesChange = std::function<void()>;
    RenameStudio(HWND owner, UINT changedMessage, BeforeFilesChange beforeChange,
                 std::filesystem::path database = {});
    ~RenameStudio();
    RenameStudio(const RenameStudio&) = delete;
    RenameStudio& operator=(const RenameStudio&) = delete;
    HWND window() const;
    void show(RECT bounds, UINT dpi, HFONT font);
    void hide();
    void openTemplates();
    HWND templatesWindow() const;
    bool processMessage(MSG& message);
    void collectionAction(CollectionAction action);
    std::size_t queuedFileCount() const;
    bool busy() const;
    void cancel();
    void setFiles(std::vector<std::filesystem::path> directory, std::vector<std::filesystem::path> selected);
    // Reload registry changes without reindexing files or rebuilding approved batches.
    // Calls made during a job are coalesced and run when that job finishes.
    void refreshCollection();
    const CollectionSnapshot& snapshot() const;
    std::optional<RenameResult> takeFileChanges();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
