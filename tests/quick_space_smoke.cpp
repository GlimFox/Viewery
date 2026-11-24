#include "quick_space.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    std::cout << "OK " << message << '\n';
}
}

int main() {
    using namespace viewery;
    try {
        QuickSpaceGesture gesture;
        require(gesture.press(100, 500) == QuickSpaceAction::togglePlayback,
                "First Space controls playback immediately");
        require(gesture.press(200, 500) == QuickSpaceAction::closePreview,
                "A second physical Space within the interval closes the preview");
        require(gesture.press(250, 500) == QuickSpaceAction::togglePlayback,
                "Closing consumes the pair rather than arming a third press");

        gesture.reset();
        require(gesture.press(0, 500) == QuickSpaceAction::togglePlayback,
                "A zero timestamp is a valid first press");
        require(gesture.press(500, 500) == QuickSpaceAction::closePreview,
                "The exact Windows double-click boundary closes the preview");
        require(gesture.press(1000, 500) == QuickSpaceAction::togglePlayback,
                "The next sequence starts with playback control");
        require(gesture.press(1501, 500) == QuickSpaceAction::togglePlayback,
                "A press beyond the interval is a separate playback action");

        gesture.reset();
        require(gesture.press(10, 500, true) == QuickSpaceAction::none,
                "An isolated auto-repeat never arms the gesture");
        require(gesture.press(100, 500) == QuickSpaceAction::togglePlayback,
                "A physical press following a repeat still starts normally");
        require(gesture.press(200, 500, true) == QuickSpaceAction::none,
                "Holding Space does not close the preview");
        require(gesture.press(590, 500, true) == QuickSpaceAction::none,
                "Later repeats also leave the original timestamp intact");
        require(gesture.press(650, 500) == QuickSpaceAction::togglePlayback,
                "Release and press after a long hold does not become a double press");
        require(gesture.press(700, 500) == QuickSpaceAction::closePreview,
                "Release and another physical press can close normally");

        gesture.reset();
        require(gesture.press(100, 500) == QuickSpaceAction::togglePlayback,
                "A new preview starts with playback control");
        gesture.reset();
        require(gesture.press(150, 500) == QuickSpaceAction::togglePlayback,
                "A focus transfer or file change resets the earlier press");
        gesture.reset();
        require(gesture.press(175, 500) == QuickSpaceAction::togglePlayback,
                "Opening a preview never carries a press from an earlier preview");
        require(gesture.press(50, 500) == QuickSpaceAction::togglePlayback,
                "A backwards clock jump cannot accidentally close the preview");
        gesture.reset();
        require(gesture.press(std::numeric_limits<std::uint64_t>::max() - 10, 500) ==
                    QuickSpaceAction::togglePlayback,
                "Large monotonic timestamps are supported");
        require(gesture.press(3, 500) == QuickSpaceAction::togglePlayback,
                "Timestamp wrap is treated as a fresh sequence");

        require(routeQuickSpace({true, true, false, false, true}) == QuickSpaceRoute::openPreview,
                "The initial configured Space opens a preview only");
        require(routeQuickSpace({true, true, true, true, true}) == QuickSpaceRoute::controlPreview,
                "Space on the source Explorer controls an existing preview instead of reopening it");
        require(routeQuickSpace({false, true, true, true, true}) == QuickSpaceRoute::controlPreview,
                "Plain Space controls an open preview when its opening chord is Ctrl+Space");
        require(routeQuickSpace({true, false, true, true, true}) == QuickSpaceRoute::controlPreview,
                "The configured control chord also controls an existing preview once");
        require(routeQuickSpace({false, true, false, true, true}) == QuickSpaceRoute::passThrough,
                "Plain Space remains in Explorer when the preview is closed and another chord is configured");
        require(routeQuickSpace({false, true, true, false, true}) == QuickSpaceRoute::passThrough,
                "Plain Space in another Explorer window is not consumed");
        require(routeQuickSpace({true, false, true, false, true}) == QuickSpaceRoute::openPreview,
                "A configured shortcut in another Explorer window opens that window's file");
        require(routeQuickSpace({true, true, true, true, false}) == QuickSpaceRoute::passThrough,
                "Rename fields, search, menus and unverified focus keep their Space key");
        require(routeQuickSpace({false, false, true, true, true}) == QuickSpaceRoute::passThrough,
                "Other modifiers do not become playback shortcuts");

        std::cout << "Quick Space gesture and Explorer routing checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
