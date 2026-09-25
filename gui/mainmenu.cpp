#include "mainmenu.h"
#include "font.h"

#include <cstdio>
#include <cstring>

/*
 * MAIN MENU input and rasterization, see mainmenu.h. The texture,
 * palette and repaint machinery come from the Popup base class; the
 * panel is repainted only when the selection changes, the recurring
 * per-frame cost is one quad.
 */

const char * const MainMenu::items[MainMenu::ITEM_COUNT] = {
    "Load ROM",
    "Save Preview",
    "Save State",
    "Load State",
    "Map Keys",
    "Game Center",
    "About",
    "Exit"
};

MainMenu::MainMenu() :
    open_flag(false), selected(0)
{
    reset_input_state();

    message[0] = '\0';

    panel_w = PAD_X * 2 + ITEM_W;
    panel_h = PAD_Y * 2 + TITLE_H + TITLE_GAP
        + ITEM_COUNT * ITEM_H + (ITEM_COUNT - 1) * ITEM_GAP;
}

void MainMenu::open(int focus_item)
{
    if (open_flag.load(std::memory_order_relaxed))
        return;
    /* Default opens land on the first item; sub-windows returning
     * here pass the item they were opened from. */
    selected = (focus_item >= 0 && focus_item < ITEM_COUNT)
        ? focus_item : 0;
    message[0] = '\0';
    reset_input_state();
    mark_dirty();
    open_flag.store(true, std::memory_order_release);
}

void MainMenu::close()
{
    open_flag.store(false, std::memory_order_release);
}

const char * MainMenu::item_label(int i)
{
    return (i >= 0 && i < ITEM_COUNT) ? items[i] : "";
}

void MainMenu::set_status(const char * msg)
{
    snprintf(message, sizeof(message), "%s", msg);
    mark_dirty();
}

void MainMenu::update(unsigned pad)
{
    if (!open_flag.load(std::memory_order_relaxed))
        return;

    /* MENU_PAD_PRESS (X) is intentionally handled by the caller.
     * Navigation fires on the keyup edge: one press = exactly one
     * step, holding the button never repeats. */

    if (keyup_edge(pad, MENU_PAD_DOWN)) {
        selected = (selected + 1) % ITEM_COUNT;
        mark_dirty();
    }
    if (keyup_edge(pad, MENU_PAD_UP)) {
        selected = (selected + ITEM_COUNT - 1) % ITEM_COUNT;
        mark_dirty();
    }

    prev_pad = pad;
}

void MainMenu::paint()
{
    /* Snapshot the sequence first: a selection change arriving from
     * the worker while we rasterize must force one more pass. */
    const unsigned seq = snapshot_seq();

    /* Panel background only; the dim backdrop underneath already
     * separates the panel from the game picture, no frame. */
    fill_rect(0, 0, panel_w, panel_h, C_PANEL_BG);

    /* Title, centered; a pending status message ("Saving..." while
     * the worker writes the preview) takes its place. */
    {
        const char * title =
            (message[0] != '\0') ? message : "MAIN MENU";
        const int title_w = (int)strlen(title) * OVERLAY_FONT_W * 2;
        print_text2x((panel_w - title_w) / 2, PAD_Y, title, C_TEXT_WHITE);
    }

    /* Item buttons: dark gray / white, selected one light gray /
     * black. */
    const int items_y = PAD_Y + TITLE_H + TITLE_GAP;
    for (int i = 0; i < ITEM_COUNT; ++i) {
        const int y = items_y + i * (ITEM_H + ITEM_GAP);
        const bool sel = (i == selected);
        fill_rect(PAD_X, y, ITEM_W, ITEM_H,
                  sel ? C_ITEM_BG_SEL : C_ITEM_BG);

        print_text2x(PAD_X + 8, y + (ITEM_H - OVERLAY_FONT_H * 2) / 2,
                     items[i], sel ? C_TEXT_BLACK : C_TEXT_WHITE);
    }

    finish_paint(seq);
}
