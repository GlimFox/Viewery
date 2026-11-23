#pragma once

#include <cstdint>

namespace viewery {

enum class QuickSpaceAction { none, togglePlayback, closePreview };

// Call once for each physical key-down edge. A keyboard hook or WM_KEYDOWN's
// previous-state bit identifies repeat events; a held key never counts twice.
class QuickSpaceGesture {
public:
    QuickSpaceAction press(std::uint64_t now, std::uint32_t interval, bool repeat = false) noexcept {
        if (repeat) return QuickSpaceAction::none;
        if (armed_ && now >= previous_ && now - previous_ <= interval) {
            reset();
            return QuickSpaceAction::closePreview;
        }
        armed_ = true;
        previous_ = now;
        return QuickSpaceAction::togglePlayback;
    }

    void reset() noexcept { armed_ = false; previous_ = 0; }

private:
    std::uint64_t previous_ = 0;
    bool armed_ = false;
};

enum class QuickSpaceRoute { passThrough, openPreview, controlPreview };

struct QuickSpaceContext {
    bool openingChord = false;
    bool plainSpace = false;
    bool previewActive = false;
    bool sameExplorer = false;
    bool fileListFocused = false;
};

// Opening shortcuts remain scoped to a verified Explorer file list. Once a
// preview is open, plain Space controls it even when the opening chord is Ctrl+Space.
constexpr QuickSpaceRoute routeQuickSpace(const QuickSpaceContext& context) noexcept {
    if (!context.fileListFocused) return QuickSpaceRoute::passThrough;
    if (context.previewActive && context.sameExplorer && (context.plainSpace || context.openingChord)) {
        return QuickSpaceRoute::controlPreview;
    }
    return context.openingChord ? QuickSpaceRoute::openPreview : QuickSpaceRoute::passThrough;
}

} // namespace viewery
