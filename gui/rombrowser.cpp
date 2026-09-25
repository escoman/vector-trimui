#include "rombrowser.h"
#include "filelist.h"
#include "imgload.h"
#include "font.h"
#include "layer_draw.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/*
 * ROM Browser input and rasterization, see rombrowser.h. The
 * texture, palette and repaint machinery come from the Popup base
 * class; the window is repainted only when its visible state
 * changes, the recurring per-frame cost is one quad (two while a
 * preview is visible).
 */

RomBrowser::RomBrowser() :
    open_flag(false),
    count(0), selected(0), top(0), dir_ok(false),
    preview_w(0), preview_h(0),
    fit_x(0), fit_y(0), fit_w(0), fit_h(0),
    preview_upload(false),
    preview_tex(nullptr)
{
    reset_input_state();

    panel_w = PANEL_W;
    panel_h = PANEL_H;

    memset(names, 0, sizeof(names));
    message[0] = '\0';
    rom_dir[0] = '\0';
    preview_for[0] = '\0';
}

void RomBrowser::open(const char * rom_dir_path)
{
    if (open_flag.load(std::memory_order_relaxed))
        return;

    /* Allocate the preview texture on demand (256 KB). */
    if (!preview_tex)
        preview_tex = new uint32_t[PREVIEW_TEX_W * PREVIEW_TEX_H]();

    /* Fresh scan on every open: ROMs added/removed outside the app
     * show up immediately. Reuses the existing FileList filter
     * and sort order. */
    std::vector<std::string> files;
    dir_ok = FileList::listRoms(rom_dir_path, files);

    snprintf(rom_dir, sizeof(rom_dir), "%s", rom_dir_path);

    count = 0;
    for (size_t i = 0; i < files.size() && count < MAX_ROMS; ++i) {
        snprintf(names[count], NAME_LEN, "%s", files[i].c_str());
        ++count;
    }

    selected = 0;         /* every open starts on the first ROM */
    top = 0;
    reset_input_state();
    message[0] = '\0';

    /* Drop the preview cache: the directory was rescanned, so the
     * file set may have changed. */
    preview_for[0] = '\0';
    preview_w = preview_h = 0;
    update_preview();

    mark_dirty();
    open_flag.store(true, std::memory_order_release);
}

void RomBrowser::close()
{
    preview_for[0] = '\0';
    preview_w = preview_h = 0;
    preview_upload = false;

    /* Release the preview texture back to the heap. */
    delete[] preview_tex;
    preview_tex = nullptr;

    open_flag.store(false, std::memory_order_release);
}

const char * RomBrowser::selected_name() const
{
    if (count <= 0 || selected < 0 || selected >= count)
        return "";
    return names[selected];
}

void RomBrowser::set_error(const char * msg)
{
    snprintf(message, sizeof(message), "%s", msg);
    mark_dirty();
}

void RomBrowser::update(unsigned pad)
{
    if (!open_flag.load(std::memory_order_relaxed))
        return;

    if (count <= 0) {
        /* Empty list: UP/DOWN do nothing (X is handled by the
         * caller and refuses without items). */
        prev_pad = pad;
        return;
    }

    /* Navigation fires on the keyup edge: one press = exactly one
     * row, holding the button never repeats. */
    bool moved = false;
    if (keyup_edge(pad, RB_PAD_DOWN)) {
        selected = (selected + 1) % count;
        moved = true;
    }
    if (keyup_edge(pad, RB_PAD_UP)) {
        selected = (selected + count - 1) % count;
        moved = true;
    }
    if (moved) {
        /* Keep the selection inside the visible window. */
        if (selected < top)
            top = selected;
        if (selected >= top + VISIBLE_ROWS)
            top = selected - VISIBLE_ROWS + 1;

        /* The preview follows the selection (cached: the same ROM
         * decodes its .png only once). */
        update_preview();

        mark_dirty();
    }

    prev_pad = pad;
}

/* Preview of the selected ROM: <base>.png next to the ROMs
 * (case-insensitive search). The result is cached under the ROM
 * name — including "no preview" — so navigation never re-probes the
 * directory for a ROM it already checked. Decode errors stay
 * invisible: the area is simply empty, the ROM stays loadable. */
void RomBrowser::update_preview()
{
    const char * name = selected_name();
    if (name[0] == '\0') {
        preview_for[0] = '\0';
        preview_w = preview_h = 0;
        return;
    }
    if (strcmp(name, preview_for) == 0)
        return;                 /* cache hit: image or known-absent */

    snprintf(preview_for, NAME_LEN, "%s", name);
    preview_w = preview_h = 0;  /* default: no preview */

    std::string path;
    int w = 0, h = 0;
    if (FileList::findPreview(rom_dir, name, path)
            && img_load(path.c_str(), preview_tex,
                        PREVIEW_TEX_W, PREVIEW_TEX_H, &w, &h)) {
        preview_w = w;
        preview_h = h;

        /* Stretched over the whole right pane (full pane height):
         * the renderer scales the decoded image to the quad, big
         * ones arrive pre-shrunk from the loader. */
        fit_x = PREVIEW_X;
        fit_y = PREVIEW_Y;
        fit_w = PREVIEW_W;
        fit_h = PREVIEW_H;

        preview_upload = true;  /* re-upload the texture before drawing */
    }
}

void RomBrowser::paint()
{
    /* Snapshot the sequence first: a state change arriving from the
     * worker while we rasterize must force one more pass. */
    const unsigned seq = snapshot_seq();

    const int header_w = PANEL_W - PAD_X * 2;
    const int list_y0 = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    const int footer_y = PANEL_H - PAD_Y - FOOTER_H;

    /* Window background only; the dim backdrop underneath already
     * separates the window from the game picture, no frame. */
    fill_rect(0, 0, PANEL_W, PANEL_H, C_PANEL_BG);

    /* Header: "Load ROM" left, "ROMs: N" right, divider below. */
    print_text2x(PAD_X, PAD_Y, "Load ROM", C_TEXT_WHITE);
    {
        char count_text[32];
        snprintf(count_text, sizeof(count_text), "ROMs: %d", count);
        const int cw = (int)strlen(count_text) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - cw, PAD_Y, count_text, C_TEXT_WHITE);
    }
    fill_rect(PAD_X, PAD_Y + TITLE_H + HDR_GAP, header_w, 1, C_PANEL_BORDER);

    /* Two panes: the ROM list on the left, the preview of the
     * selected ROM on the right (a separate RGBA quad drawn by
     * draw(); here the pane is just empty background). A thin
     * divider separates them. */
    fill_rect(PREVIEW_X - DIV_GAP / 2, list_y0, 1, footer_y - list_y0,
              C_PANEL_BORDER);

    if (!dir_ok) {
        const char * msg = "Unable to open ROMS folder";
        const int mw = (int)strlen(msg) * OVERLAY_FONT_W * 2;
        print_text2x(PAD_X + (LIST_W - mw) / 2,
                     list_y0 + (VISIBLE_ROWS * ROW_H) / 2 - OVERLAY_FONT_H,
                     msg, C_TEXT_WHITE);
    } else if (count == 0) {
        const char * msg = "No ROMs found";
        const int mw = (int)strlen(msg) * OVERLAY_FONT_W * 2;
        print_text2x(PAD_X + (LIST_W - mw) / 2,
                     list_y0 + (VISIBLE_ROWS * ROW_H) / 2 - OVERLAY_FONT_H,
                     msg, C_TEXT_WHITE);
    } else {
        /* Visible slice of the list; long names are cut at the row
         * width. Same highlight scheme as the MAIN MENU. */
        const int max_chars = LIST_W / (OVERLAY_FONT_W * 2);
        for (int r = 0; r < VISIBLE_ROWS; ++r) {
            const int idx = top + r;
            if (idx >= count)
                break;
            const int y = list_y0 + r * ROW_H;
            const bool sel = (idx == selected);
            fill_rect(PAD_X, y, LIST_W, ROW_H - 2,
                      sel ? C_ITEM_BG_SEL : C_ITEM_BG);

            char shown[NAME_LEN];
            snprintf(shown, sizeof(shown), "%s", names[idx]);
            shown[max_chars < NAME_LEN ? max_chars : NAME_LEN - 1] = '\0';
            print_text2x(PAD_X + 4, y + (ROW_H - 2 - OVERLAY_FONT_H * 2) / 2,
                         shown, sel ? C_TEXT_BLACK : C_TEXT_WHITE);
        }
    }

    /* Footer: a pending error message wins over the key hints. */
    if (message[0] != '\0') {
        const int mw = (int)strlen(message) * OVERLAY_FONT_W * 2;
        print_text2x((PANEL_W - mw) / 2, footer_y, message, C_TEXT_WHITE);
    } else {
        const char * hints = "A Select    B Back";
        const int hw = (int)strlen(hints) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - hw, footer_y, hints, C_TEXT_WHITE);
    }

    finish_paint(seq);
}

void RomBrowser::draw()
{
    /* Panel quad first. */
    UILayer::draw();
    /* Preview on top of the right pane. */
    draw_preview();
}

void RomBrowser::draw_preview()
{
    if (!has_preview())
        return;

    /* The upload flag is consumed only when a quad is really drawn:
     * the SDL texture cache in layer_draw.cpp refreshes the atlas
     * from the (possibly recycled) data pointer on this flag. */
    const bool upload = consume_preview_upload();

    /* Panel-local fit rectangle moved onto the display: the panel is
     * centered the same way UILayer::draw() centers it. */
    int fx, fy, fw, fh;
    get_preview_rect(&fx, &fy, &fw, &fh);
    const float x0 = ((float)LAYER_SCREEN_W - (float)get_width()) / 2.0f + (float)fx;
    const float y0 = ((float)LAYER_SCREEN_H - (float)get_height()) / 2.0f + (float)fy;
    const float uw = (float)get_preview_w();
    const float vh = (float)get_preview_h();

    /* Scaled pictures look better filtered. */
    layer_draw_rgba_quad(
        preview_tex_data(), PREVIEW_TEX_W, PREVIEW_TEX_H,
        x0, y0, (float)fw, (float)fh,
        0.0f, 0.0f, uw, vh,
        /* bilinear */ true, upload);
}
