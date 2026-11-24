/*
 * BruceLink on Bruce's main menu (next to Wi-Fi, BLE, RF...).
 *
 * Installed by tools/install_into_bruce.py, which adds this item to
 * src/core/main_menu.{h,cpp}. Selecting it opens BruceLink's own submenu
 * (turn on/off, pair with a Flipper, forget the pairing; see
 * BruceLinkModule.h). Kept free of NimBLE and bridge headers: main_menu.h
 * includes it from many translation units.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <MenuItemInterface.h>

class BruceLinkMenu : public MenuItemInterface {
public:
    BruceLinkMenu() : MenuItemInterface("BruceLink") {}

    void optionsMenu(void) override;
    void drawIcon(float scale) override;
    bool hasTheme() override { return false; } // no theme image: always draw the icon
    const String &themePath() override { return _noTheme; }

private:
    String _noTheme;
};
