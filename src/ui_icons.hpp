#pragma once
#include <windows.h>

namespace viewery::ui {

enum class Icon {
    none, folder, catalog, info, exportFile, more, first, chevronLeft,
    play, pause, chevronRight, last, fit, fullscreen, restore, pin,
    openWindow, close, catalogBottom, catalogLeft, volume, muted,
    camera, timestamp, rename, settings, checkbox, checkboxChecked,
    revealFile, artist
};

// Font-independent native outlines. Preserves the caller's GDI state and clip.
// false means no drawing (none/invalid icon, null DC or empty bounds).
bool drawIcon(HDC dc, RECT bounds, Icon icon, COLORREF color, UINT dpi=96);

} // namespace viewery::ui
