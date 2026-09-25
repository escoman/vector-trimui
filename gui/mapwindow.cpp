#include "mapwindow.h"
#include "font.h"
#include "keymap.h"

#include <cstring>
#include <cstdio>

/*
 * Map Keys input and rasterization, see mapwindow.h. The texture,
 * palette and repaint machinery come from the Popup base class; the
 * panel is repainted only on a visible change, the recurring
 * per-frame cost is one quad (plus the always-on VKBD quad).
 */

MapWindow::MapWindow() :
    open_flag(false), selected(0), top(0), wait_src(-1)
{
    reset_input_state();
    rom_label[0] = '\0';

    panel_w = PANEL_W;
    panel_h = PANEL_H;
}

void MapWindow::open(const char * label)
{
    if (open_flag.load(std::memory_order_relaxed))
        return;
    snprintf(rom_label, sizeof(rom_label), "%s",
             (label != nullptr) ? label : "");
    selected = 0;
    top = 0;
    wait_src = -1;
    reset_input_state();
    mark_dirty();
    open_flag.store(true, std::memory_order_release);
}

void MapWindow::close()
{
    wait_src = -1;
    open_flag.store(false, std::memory_order_release);
}

void MapWindow::update(unsigned pad)
{
    if (!open_flag.load(std::memory_order_relaxed))
        return;

    /* A / X / B are handled by the caller. Navigation fires
     * on the keyup edge: one press = exactly one row, holding the
     * button never repeats. */

    if (keyup_edge(pad, MK_PAD_DOWN)) {
        selected = (selected + 1) % MAP_SRC_COUNT;
        if (selected < top)
            top = 0;                        /* wrapped to the top */
        if (selected >= top + VISIBLE_ROWS)
            top = selected - VISIBLE_ROWS + 1;
        mark_dirty();
    }
    if (keyup_edge(pad, MK_PAD_UP)) {
        selected = (selected + MAP_SRC_COUNT - 1) % MAP_SRC_COUNT;
        if (selected >= top + VISIBLE_ROWS)
            top = MAP_SRC_COUNT - VISIBLE_ROWS;   /* wrapped down */
        if (selected < top)
            top = selected;
        mark_dirty();
    }

    prev_pad = pad;
}

void MapWindow::start_assign()
{
    wait_src = selected;
    mark_dirty();
}

void MapWindow::cancel_assign()
{
    if (wait_src < 0)
        return;
    wait_src = -1;
    mark_dirty();
}

void MapWindow::assign_selected(int scancode)
{
    if (wait_src < 0)
        return;
    KeyMap::assign(wait_src, scancode);
    wait_src = -1;
    mark_dirty();
}

void MapWindow::disable_selected()
{
    KeyMap::disable(selected);
    mark_dirty();
}

void MapWindow::paint()
{
    /* Snapshot the sequence first: a state change arriving from the
     * worker while we rasterize must force one more pass. */
    const unsigned seq = snapshot_seq();

    const int inner_w = PANEL_W - PAD_X * 2;
    /* Tight vertical packing: the source list sits directly under
     * the header divider and stays clear of the footer hints. */
    const int div_y = PAD_Y + TITLE_H + 2;
    const int list_y0 = div_y + 1 + 2;
    const int footer_y = PANEL_H - PAD_Y - FOOTER_H;

    fill_rect(0, 0, PANEL_W, PANEL_H, C_PANEL_BG);

    /* Header: "Map Keys" left, the ROM name right, divider below. */
    print_text2x(PAD_X, PAD_Y, "Map Keys", C_TEXT_WHITE);
    {
        char hdr[80];
        snprintf(hdr, sizeof(hdr), "ROM: %s",
                 rom_label[0] != '\0' ? rom_label : "-");
        /* Cut at the panel width: 16 px per glyph at 2x. */
        const int max_chars = inner_w / (OVERLAY_FONT_W * 2) - 9;
        if ((int)strlen(hdr) > max_chars)
            hdr[max_chars] = '\0';
        const int hw = (int)strlen(hdr) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - hw, PAD_Y, hdr, C_TEXT_WHITE);
    }
    fill_rect(PAD_X, div_y, inner_w, 1, C_PANEL_BORDER);

    /* The full effective mapping list: source -> Vector key plus
     * the origin tag (DEFAULT / ROM / DISABLED). */
    for (int r = 0; r < VISIBLE_ROWS; ++r) {
        const int idx = top + r;
        if (idx >= MAP_SRC_COUNT)
            break;
        const int y = list_y0 + r * ROW_H;
        const bool sel = (idx == selected);
        fill_rect(PAD_X, y, inner_w, ROW_H - 2,
                  sel ? C_ITEM_BG_SEL : C_ITEM_BG);

        const uint8_t fg = sel ? C_TEXT_BLACK : C_TEXT_WHITE;
        const int ty = y + (ROW_H - 2 - OVERLAY_FONT_H * 2) / 2;

        if (idx == wait_src) {
            /* Assignment mode: the row is the selected source, it
             * now waits for a VKBD key. */
            print_text2x(PAD_X + 4, ty, "PRESS VECTOR KEY", fg);
            continue;
        }

        print_text2x(PAD_X + 4, ty, KeyMap::source_label(idx), fg);

        /* Origin tag, right-aligned, at 1x so the longest row
         * ("BUTTON A > BACKSPACE ... DEFAULT") fits; the key name
         * sits between the source column and the tag. */
        const KeyMap::EntryState st = KeyMap::entry_state(idx);
        const char * tag = (st == KeyMap::STATE_OVERRIDE) ? "ROM"
                         : (st == KeyMap::STATE_DISABLED) ? "DISABLED"
                         : "DEFAULT";
        const int tag_w = (int)strlen(tag) * OVERLAY_FONT_W;
        const int ty1 = y + (ROW_H - 2 - OVERLAY_FONT_H) / 2;
        print_text(PAD_X + inner_w - 4 - tag_w, ty1, tag, fg);

        const int eff = KeyMap::effective_key(idx);
        const char * key = (st == KeyMap::STATE_DISABLED || eff < 0)
            ? "NONE" : KeyMap::key_label(eff);
        char mid[48];
        snprintf(mid, sizeof(mid), "> %s", key);
        print_text2x(PAD_X + 4 + 13 * OVERLAY_FONT_W * 2, ty, mid, fg);
    }

    /* Footer key hints. */
    {
        const char * hints = "A:Assign X:None B:Back";
        const int hw = (int)strlen(hints) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - hw, footer_y, hints, C_TEXT_WHITE);
    }

    finish_paint(seq);
}
