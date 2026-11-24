/*
 * BruceLink on Bruce's main menu (see BruceLinkMenu.h).
 * Builds only inside the Bruce tree.
 * SPDX-License-Identifier: MIT
 */
#if !defined(LITE_VERSION)
#include "BruceLinkMenu.h"

#include "BruceLinkModule.h"

void BruceLinkMenu::optionsMenu() { bruceLinkMenu(); }

/* Two devices joined by a dotted line: a wide one (the remote) and a tall,
 * narrow one (the stick). The dots are filled while BruceLink is on and
 * hollow while it is off. Sizes are for scale 1: 80 px wide, the most a
 * main-menu icon may span (ICON_SCALE_REFERENCE); the grid layout draws it
 * smaller. */
void BruceLinkMenu::drawIcon(float scale) {
    clearIconArea();
    const uint16_t fg = bruceConfig.priColor;
    const uint16_t bg = bruceConfig.bgColor;
    const int cx = iconCenterX;
    const int cy = iconCenterY;
    const int t = max(1, (int)(scale * 3)); // outline thickness

    auto device = [&](int x, int y, int w, int h, int r) {
        tft.fillRoundRect(x, y, w, h, r, fg);
        tft.fillRoundRect(x + t, y + t, w - 2 * t, h - 2 * t, max(0, r - t), bg);
    };

    // Remote: 32 x 24, screen on its left half.
    const int rw = scale * 32, rh = scale * 24;
    const int rx = cx - scale * 40, ry = cy - rh / 2;
    device(rx, ry, rw, rh, max(2, (int)(scale * 5)));
    tft.fillRect(rx + scale * 6, ry + scale * 6, scale * 12, rh - scale * 12, fg);

    // Stick: 16 x 46, screen near the top.
    const int sw = scale * 16, sh = scale * 46;
    const int sx = cx + scale * 24, sy = cy - sh / 2;
    device(sx, sy, sw, sh, max(2, (int)(scale * 4)));
    tft.fillRect(sx + scale * 4, sy + scale * 5, sw - scale * 8, scale * 12, fg);

    // The link.
    const bool on = brucelink::moduleRunning();
    const int dr = max(1, (int)(scale * 3));
    for (int i = 0; i < 3; i++) {
        const int dx = cx + scale * (i * 8);
        if (on) tft.fillCircle(dx, cy, dr, fg);
        else tft.drawCircle(dx, cy, dr, fg);
    }
}
#endif // !LITE_VERSION
