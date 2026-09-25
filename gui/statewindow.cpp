#include "statewindow.h"
#include "statefile.h"
#include "imgload.h"
#include "font.h"
#include "layer_draw.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

/*
 * State Browser input and rasterization, see statewindow.h. The
 * texture, palette and repaint machinery come from the Popup base
 * class; the slot thumbnails are separate RGBA quads drawn under the
 * panel quad from the shared atlas (SDL_Renderer backend).
 */

/* Save timestamp -> "DD.MM.YYYY HH:MM" shown in the slot header. */
static void format_ts(uint64_t ts, char * buf, size_t len)
{
    time_t t = (time_t)ts;
    struct tm * tmv = localtime(&t);
    if (tmv == nullptr) {
        snprintf(buf, len, "-");
        return;
    }
    snprintf(buf, len, "%02d.%02d.%04d %02d:%02d",
             tmv->tm_mday, tmv->tm_mon + 1, tmv->tm_year + 1900,
             tmv->tm_hour, tmv->tm_min);
}

/* Box-filter shrink into the 0xAABBGGRR layout (alpha forced opaque);
 * the save-time frame (576x288) arrives exactly 4x larger than a
 * tile, but the code stays generic for other sizes. */
static void box_shrink(const uint32_t * src, int sw, int sh,
                       uint32_t * dst, int dw, int dh)
{
    for (int y = 0; y < dh; ++y) {
        const int sy0 = y * sh / dh;
        const int sy1 = (y + 1) * sh / dh;
        for (int x = 0; x < dw; ++x) {
            const int sx0 = x * sw / dw;
            const int sx1 = (x + 1) * sw / dw;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint32_t * row = src + (size_t)sy * sw;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint32_t c = row[sx];
                    r += c & 0xff;
                    g += (c >> 8) & 0xff;
                    b += (c >> 16) & 0xff;
                    ++n;
                }
            }
            if (n == 0)
                n = 1;
            dst[(size_t)y * dw + x] = 0xff000000u
                | ((b / n) << 16) | ((g / n) << 8) | (r / n);
        }
    }
}

StateWindow::StateWindow() :
    open_flag(false),
    open_mode(MODE_SAVE),
    selected(0),
    top(0),
    thumb_upload(false),
    thumb_tex(nullptr)
{
    reset_input_state();

    panel_w = PANEL_W;
    panel_h = PANEL_H;

    rom_dir[0] = '\0';
    message[0] = '\0';
    for (int i = 0; i < STATE_SLOTS; ++i) {
        occupied[i] = false;
        slot_ts[i] = 0;
        thumb_w[i] = thumb_h[i] = 0;
    }
}

void StateWindow::open(Mode m, const char * dir_path)
{
    if (open_flag.load(std::memory_order_relaxed))
        return;

    /* Allocate the thumbnail atlas on demand (512 KB). */
    if (!thumb_tex)
        thumb_tex = new uint32_t[ATLAS_W * ATLAS_H]();

    open_mode = m;
    snprintf(rom_dir, sizeof(rom_dir), "%s", dir_path);
    message[0] = '\0';

    /* Scan headers and load all thumbnails synchronously. PNG
     * files are small enough that the whole atlas is ready before
     * the window is even drawn. */
    scan_headers();

    /* SAVE starts on slot 1; LOAD on the first occupied slot. */
    selected = 0;
    if (m == MODE_LOAD) {
        for (int i = 0; i < STATE_SLOTS; ++i) {
            if (occupied[i]) {
                selected = i;
                break;
            }
        }
    }

    int top_row = selected / STATE_GRID_COLS;
    if (top_row > STATE_TOTAL_ROWS - STATE_GRID_ROWS)
        top_row = STATE_TOTAL_ROWS - STATE_GRID_ROWS;
    top = top_row * STATE_GRID_COLS;

    reset_input_state();

    thumb_upload = true;
    mark_dirty();
    open_flag.store(true, std::memory_order_release);
}

void StateWindow::close()
{
    thumb_upload = false;
    open_flag.store(false, std::memory_order_release);

    /* Release the atlas back to the heap. */
    delete[] thumb_tex;
    thumb_tex = nullptr;
}

void StateWindow::set_status(const char * msg)
{
    snprintf(message, sizeof(message), "%s", msg);
    mark_dirty();
}

void StateWindow::update(unsigned pad)
{
    if (!open_flag.load(std::memory_order_relaxed))
        return;

    /* Keyup-edge steps. */
    int col = selected % STATE_GRID_COLS;
    int row = selected / STATE_GRID_COLS;
    bool moved = false;

    if (keyup_edge(pad, SB_PAD_RIGHT) && col < STATE_GRID_COLS - 1) {
        ++col;
        moved = true;
    }
    if (keyup_edge(pad, SB_PAD_LEFT) && col > 0) {
        --col;
        moved = true;
    }
    if (keyup_edge(pad, SB_PAD_DOWN) && row < STATE_TOTAL_ROWS - 1) {
        ++row;
        moved = true;
    }
    if (keyup_edge(pad, SB_PAD_UP) && row > 0) {
        --row;
        moved = true;
    }

    if (moved) {
        selected = row * STATE_GRID_COLS + col;

        int top_row = top / STATE_GRID_COLS;
        if (row < top_row)
            top_row = row;
        else if (row > top_row + STATE_GRID_ROWS - 1)
            top_row = row - STATE_GRID_ROWS + 1;
        top = top_row * STATE_GRID_COLS;

        mark_dirty();
    }

    prev_pad = pad;
}

/* Probe stateN.bin headers and decode every occupied slot's PNG
 * thumbnail into the atlas in one pass. */
void StateWindow::scan_headers()
{
    memset(thumb_tex, 0, (size_t)ATLAS_W * ATLAS_H * sizeof(uint32_t));
    static uint32_t scratch[THUMB_W * THUMB_H];

    for (int i = 0; i < STATE_SLOTS; ++i) {
        occupied[i] = false;
        slot_ts[i] = 0;
        thumb_w[i] = thumb_h[i] = 0;

        const int slot = i + 1;
        uint64_t ts = 0;
        if (StateFile::read_header(StateFile::bin_path(rom_dir, slot), ts)) {
            occupied[i] = true;
            slot_ts[i] = ts;

            /* Decode the slot's PNG thumbnail straight into the
             * atlas tile. */
            int w = 0, h = 0;
            const std::string shot = StateFile::shot_path(rom_dir, slot);
            if (img_load(shot.c_str(), scratch, THUMB_W, THUMB_H, &w, &h))
                blit_tile(i, scratch, w, h);
        }
    }
}

void StateWindow::blit_tile(int idx, const uint32_t * src, int tw, int th)
{
    const int tx = (idx % STATE_GRID_COLS) * THUMB_W;
    const int ty = (idx / STATE_GRID_COLS) * THUMB_H;
    for (int y = 0; y < th; ++y) {
        memcpy(thumb_tex + (size_t)(ty + y) * ATLAS_W + tx,
               src + (size_t)y * tw,
               (size_t)tw * sizeof(uint32_t));
    }
    thumb_w[idx] = tw;
    thumb_h[idx] = th;
}

/* The slot was just written: update its visible info from the data
 * the caller already holds (no rescan, the frame is the picture the
 * user saw at save time, §9). */
void StateWindow::after_save(int slot, uint64_t ts,
                             const uint32_t * frame, int fw, int fh)
{
    const int idx = slot - 1;
    if (idx < 0 || idx >= STATE_SLOTS)
        return;

    static uint32_t scratch[THUMB_W * THUMB_H];

    occupied[idx] = true;
    slot_ts[idx] = ts;
    thumb_w[idx] = thumb_h[idx] = 0;

    box_shrink(frame, fw, fh, scratch, THUMB_W, THUMB_H);
    blit_tile(idx, scratch, THUMB_W, THUMB_H);

    snprintf(message, sizeof(message), "Saved slot %d", slot);

    thumb_upload = true;
    mark_dirty();
}

void StateWindow::paint()
{
    /* Snapshot the sequence first: a state change arriving from the
     * worker while we rasterize must force one more pass. */
    const unsigned seq = snapshot_seq();

    const int header_w = PANEL_W - PAD_X * 2;
    const int footer_y = PANEL_H - PAD_Y - FOOTER_H;
    const bool saving = (open_mode == MODE_SAVE);

    /* Window background only; the dim backdrop underneath already
     * separates the window from the game picture, no frame. */
    fill_rect(0, 0, PANEL_W, PANEL_H, C_PANEL_BG);

    /* Header: mode title left, occupied-slot count right. */
    print_text2x(PAD_X, PAD_Y, saving ? "Save State" : "Load State",
                 C_TEXT_WHITE);
    {
        int used = 0;
        for (int i = 0; i < STATE_SLOTS; ++i)
            if (occupied[i])
                ++used;
        char count_text[32];
        snprintf(count_text, sizeof(count_text), "States: %d/%d",
                 used, STATE_SLOTS);
        const int cw = (int)strlen(count_text) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - cw, PAD_Y, count_text, C_TEXT_WHITE);
    }
    fill_rect(PAD_X, PAD_Y + TITLE_H + HDR_GAP, header_w, 1, C_PANEL_BORDER);

    /* The visible window of the slot grid. Occupied cells cut a
     * transparent window (C_HOLE) into the panel: the slot
     * screenshot is drawn by TV under the panel quad and shows
     * through it, while the labels rasterized here land on top of
     * the picture. Empty cells keep the usual opaque background. */
    for (int vis = 0; vis < STATE_GRID_ROWS * STATE_GRID_COLS; ++vis) {
        const int i = top + vis;
        const int cx = PAD_X + (vis % STATE_GRID_COLS) * (CELL_W + GRID_GAP);
        const int cy = GRID_Y0 + (vis / STATE_GRID_COLS) * (CELL_H + GRID_GAP);
        const bool sel = (i == selected);

        if (occupied[i]) {
            fill_rect(cx, cy, CELL_W, CELL_H, C_HOLE);

            /* Selection: a light frame around the picture. */
            if (sel) {
                fill_rect(cx, cy, CELL_W, 2, C_ITEM_BG_SEL);
                fill_rect(cx, cy + CELL_H - 2, CELL_W, 2, C_ITEM_BG_SEL);
                fill_rect(cx, cy, 2, CELL_H, C_ITEM_BG_SEL);
                fill_rect(cx + CELL_W - 2, cy, 2, CELL_H, C_ITEM_BG_SEL);
            }

            /* Slot number top-left, save date top-right, both over
             * the picture: black drop shadow first, then white, so
             * the labels stay readable on any screenshot. */
            char num[8];
            snprintf(num, sizeof(num), "%d", i + 1);
            char date[64];
            format_ts(slot_ts[i], date, sizeof(date));
            /* Compact date: 7px advance instead of 8px to fit. */
            const int dw = (int)strlen(date) * 7 - 1;

            print_text(cx + 4, cy + 2, num, C_TEXT_BLACK);
            print_text(cx + CELL_W - 2 - dw, cy + 2, date, C_TEXT_BLACK, 7);
            print_text(cx + 3, cy + 1, num, C_TEXT_WHITE);
            print_text(cx + CELL_W - 3 - dw, cy + 1, date, C_TEXT_WHITE, 7);
        } else {
            const uint8_t fg = sel ? C_TEXT_BLACK : C_TEXT_WHITE;
            fill_rect(cx, cy, CELL_W, CELL_H,
                      sel ? C_ITEM_BG_SEL : C_ITEM_BG);

            char num[8];
            snprintf(num, sizeof(num), "%d", i + 1);
            print_text(cx + 3, cy + 1, num, fg);

            const char * msg = "EMPTY";
            const int mw = (int)strlen(msg) * OVERLAY_FONT_W * 2;
            print_text2x(cx + (CELL_W - mw) / 2,
                         cy + (CELL_H - OVERLAY_FONT_H * 2) / 2,
                         msg, fg);
        }
    }

    /* Footer: a pending message (save/load result) wins over the
     * key hints. */
    if (message[0] != '\0') {
        const int mw = (int)strlen(message) * OVERLAY_FONT_W * 2;
        print_text2x((PANEL_W - mw) / 2, footer_y, message, C_TEXT_WHITE);
    } else {
        const char * hints = saving ? "X Save    O Back"
                                    : "X Load    O Back";
        const int hw = (int)strlen(hints) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - hw, footer_y, hints, C_TEXT_WHITE);
    }

    finish_paint(seq);
}

void StateWindow::draw()
{
    /* Slot screenshots first (under the panel quad with C_HOLE windows). */
    draw_thumbs();
    /* Panel quad on top. */
    UILayer::draw();
}

void StateWindow::draw_thumbs()
{
    const int first = first_visible();
    const int last = first + STATE_GRID_ROWS * STATE_GRID_COLS;

    /* Nothing to draw: keep the upload flag pending so a rebuilt atlas
     * is still re-uploaded when a tile finally becomes visible. */
    int first_thumb = -1;
    for (int i = first; i < last; ++i) {
        if (slot_has_thumb(i)) {
            first_thumb = i;
            break;
        }
    }
    if (first_thumb < 0)
        return;

    /* Rebuilt atlas: the SDL backend refreshes the cached texture on
     * the quad carrying this flag (once per rebuild). */
    const bool upload = consume_thumb_upload();

    /* Panel-local rectangles moved onto the display: the panel is
     * centered the same way UILayer::draw() centers it. */
    const float px = ((float)LAYER_SCREEN_W - (float)get_width()) / 2.0f;
    const float py = ((float)LAYER_SCREEN_H - (float)get_height()) / 2.0f;

    for (int i = first_thumb; i < last; ++i) {
        if (!slot_has_thumb(i))
            continue;

        int tu, tv_, tw, th;
        thumb_tile(i, &tu, &tv_, &tw, &th);
        int rx, ry, rw, rh;
        thumb_rect(i, first, &rx, &ry, &rw, &rh);

        layer_draw_rgba_quad(
            thumb_tex_data(), ATLAS_W, ATLAS_H,
            px + (float)rx, py + (float)ry, (float)rw, (float)rh,
            (float)tu, (float)tv_, (float)(tu + tw), (float)(tv_ + th),
            /* bilinear */ true,
            /* upload */ i == first_thumb && upload);
    }
}
