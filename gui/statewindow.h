#pragma once

#include <atomic>
#include <inttypes.h>
#include "popup.h"

/*
 * State Browser: one window behind both MAIN MENU items "Save
 * State" and "Load State" (Stage 5), opened in the corresponding
 * mode. UI state machine:
 *
 *     enum class UIState { ..., STATE_BROWSER };
 *
 *   STATE_BROWSER - the machine stays frozen via the Emulator pause
 *                   flag for the whole lifetime of the window; the
 *                   last Vector frame is the backdrop, the window is
 *                   drawn above the dim overlay like the other
 *                   popups.
 *
 * Transitions (all in the worker thread, see main.cpp handle_input):
 *
 *   MAIN_MENU --X on Save/Load State--> STATE_BROWSER (menu closes)
 *   STATE_BROWSER --O/START-----------> MAIN_MENU (focus back on the
 *                                       item it was opened from)
 *   STATE_BROWSER --X (SAVE mode)-----> serialize into the slot,
 *                                       stay in the browser
 *   STATE_BROWSER --X (LOAD mode)-----> restore the slot, close,
 *                                       straight to GAME (resumed)
 *
 * The slots form a STATE_GRID_COLS x STATE_TOTAL_ROWS grid of which
 * only STATE_GRID_COLS x STATE_GRID_ROWS rows are visible at once:
 * the cursor drags a scroll window over the full slot list. The
 * whole UI iterates over STATE_SLOTS entries only, so a different
 * slot count needs no layout rewrite. Storage lives in plain files
 * (SAVES/<ROM base name>/stateN.bin + stateN.png, see statefile.h);
 * the directory is rescanned on every open.
 *
 * Slot thumbnails: the Vector screenshots (stateN.png) are decoded
 * into one shared RGBA atlas (STATE_GRID_COLS x STATE_TOTAL_ROWS
 * tiles of THUMB_W x THUMB_H, every slot gets a tile) and presented
 * by draw() as one quad per visible occupied slot stretched over the
 * whole cell, drawn UNDER the
 * panel quad: the panel texture keeps
 * transparent windows (Popup::C_HOLE) at the occupied cells, so the
 * picture shows through while the slot number and save date
 * rasterized into the panel land on top of the picture. The atlas
 * is rebuilt on open() and after every successful save.
 *
 * Thumbnails are loaded synchronously in open(): PNG files are
 * small enough that the whole atlas is ready before the window is
 * even drawn (no progressive loading needed).
 *
 * Threading split is the usual popup one: the worker mutates the
 * state (open/close/update/after_save), the display thread polls
 * needs_repaint() and paints; the thumbnail atlas follows the
 * RomBrowser preview handshake (upload flag consumed by TV).
 */

/* Grid dimensions (§1): the only constants the slot count and the
 * cell layout derive from. STATE_GRID_ROWS is the number of VISIBLE
 * rows; STATE_TOTAL_ROWS the whole slot list, scrolled under the
 * window. Keep STATE_TOTAL_ROWS * THUMB_H <= ATLAS_H. */
static const int STATE_GRID_COLS = 3;
static const int STATE_GRID_ROWS = 3;
static const int STATE_TOTAL_ROWS = 12;
static const int STATE_SLOTS = STATE_GRID_COLS * STATE_TOTAL_ROWS;

/* Normalized pad state passed to StateWindow::update(): which
 * buttons are currently held. Keeps statewindow free of pspctrl.h. */
enum {
    SB_PAD_UP    = 0x01,
    SB_PAD_DOWN  = 0x02,
    SB_PAD_LEFT  = 0x04,
    SB_PAD_RIGHT = 0x08,
};

class StateWindow : public Popup
{
public:
    /* Window size, UI coordinate space (480x272); the renderer
     * centers it like the other popups. */
    static const int PANEL_W = 440;
    static const int PANEL_H = 220;

    /* Layout constants (same scheme as the ROM Browser). */
    static const int PAD_X = 8;       /* window left/right padding */
    static const int PAD_Y = 8;       /* window top/bottom padding */
    static const int TITLE_H = 16;    /* header row, 8x8 font at 2x */
    static const int HDR_GAP = 4;     /* gap around the header divider */
    static const int FOOTER_H = 16;   /* bottom hint/status row */
    static const int GRID_GAP = 4;    /* gap between the grid cells */

    static const int GRID_Y0 = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    static const int GRID_W = PANEL_W - PAD_X * 2;
    static const int GRID_H = PANEL_H - PAD_Y - FOOTER_H - GRID_Y0;
    static const int CELL_W =
        (GRID_W - (STATE_GRID_COLS - 1) * GRID_GAP) / STATE_GRID_COLS;
    static const int CELL_H =
        (GRID_H - (STATE_GRID_ROWS - 1) * GRID_GAP) / STATE_GRID_ROWS;

    /* One slot thumbnail: the Vector frame (576x288) box-shrunk.
     * 80x40 keeps 2:1 aspect ratio and fits 12 rows in 512px atlas. */
    static const int THUMB_W = 80;
    static const int THUMB_H = 40;

    /* Atlas holding every slot tile; power-of-two texture dimensions.
     * 3 cols x 80 = 240 <= 256; 12 rows x 40 = 480 <= 512. */
    static const int ATLAS_W = 256;
    static const int ATLAS_H = 512;

    enum Mode { MODE_SAVE, MODE_LOAD };

    StateWindow();

    /* Atomic: written by the worker, read by the display thread. */
    bool is_open() const override { return this->open_flag.load(std::memory_order_acquire); }
    Mode mode() const { return this->open_mode; }

    /* Worker thread: rescan SAVES/<rom>/ (fresh slot info on every
     * open, §27); SAVE mode starts on slot 1, LOAD mode on the
     * first occupied slot (slot 1 when all are empty). */
    void open(Mode m, const char * rom_dir);
    /* Worker thread: STATE_BROWSER -> MAIN_MENU or GAME. */
    void close();

    /* One input step; called by the worker thread (~50 Hz) while
     * open. The D-pad moves the selection through the full
     * STATE_SLOTS grid, firing on the keyup edge and CLAMPED at the
     * grid edges (§13: no row wrap, the last slot + RIGHT stays
     * there); the scroll window follows the cursor. X/O/START edges
     * are handled by the caller. */
    void update(unsigned pad);

    /* 1-based slot number under the cursor. */
    int selected_slot() const { return this->selected + 1; }
    bool is_selected_occupied() const { return this->occupied[this->selected]; }

    /* 0-based index of the first slot of the visible window
     * (a multiple of STATE_GRID_COLS). Display thread reads only. */
    int first_visible() const { return this->top; }

    /* Worker thread: status line shown in the footer (in-progress
     * message or save/load error), cleared on the next open(). */
    void set_status(const char * msg);
    void set_error(const char * msg) { this->set_status(msg); }

    /* Worker thread: refresh a slot right after a successful save
     * (new timestamp, new thumbnail box-shrunk from the just-shown
     * frame fw x fh) without rescanning the directory. */
    void after_save(int slot, uint64_t ts,
                    const uint32_t * frame, int fw, int fh);

    /* Thumbnail access for draw_thumbs(). UI thread reads only
     * while is_open(). */
    const uint32_t * thumb_tex_data() const { return thumb_tex; }
    bool slot_has_thumb(int idx) const
    {
        return idx >= 0 && idx < STATE_SLOTS && this->thumb_w[idx] > 0;
    }
    /* Source tile of a slot inside the atlas (pixels). */
    void thumb_tile(int idx, int * u, int * v, int * w, int * h) const
    {
        *u = (idx % STATE_GRID_COLS) * THUMB_W;
        *v = (idx / STATE_GRID_COLS) * THUMB_H;
        *w = this->thumb_w[idx];
        *h = this->thumb_h[idx];
    }
    /* Panel-local destination rectangle of the slot quad: the
     * picture covers the whole visible cell (stretched by the GE),
     * the slot number and the save date are overlaid on top of it.
     * first is first_visible(): the position comes from the slot's
     * place in the scroll window, not in the full grid. */
    static void thumb_rect(int idx, int first, int * x, int * y,
                           int * w, int * h)
    {
        const int pos = idx - first;
        *x = PAD_X + (pos % STATE_GRID_COLS) * (CELL_W + GRID_GAP);
        *y = GRID_Y0 + (pos / STATE_GRID_COLS) * (CELL_H + GRID_GAP);
        *w = CELL_W;
        *h = CELL_H;
    }
    /* True once after the atlas was rebuilt: the SDL backend must
     * refresh its cached texture. Consumed by draw_thumbs(). */
    bool consume_thumb_upload()
    {
        bool v = thumb_upload;
        thumb_upload = false;
        return v;
    }

    /* Rasterize the window into the popup texture. Main thread
     * only; a state change arriving from the worker while painting
     * forces one more pass. */
    void paint() override;

    /* Display thread: draw thumbs under the panel quad. */
    void draw() override;

private:
    void draw_thumbs();
    /* Worker thread: probe stateN.bin headers and decode every
     * slot's PNG thumbnail into the atlas in one pass. */
    void scan_headers();
    /* Worker thread: blit a packed tw x th image into the slot's
     * atlas tile (img_load output is packed, the atlas has pitch). */
    void blit_tile(int idx, const uint32_t * src, int tw, int th);

    std::atomic<bool> open_flag;
    Mode open_mode;
    int selected;               /* 0-based grid index, worker only */
    int top;                    /* 0-based first visible slot, worker */
    char rom_dir[128];          /* SAVES/<rom> stored by open() */
    char message[64];           /* footer status line, empty = none */

    /* Slot info: worker writes in open()/after_save(), the display
     * thread reads it while is_open(). */
    bool occupied[STATE_SLOTS];
    uint64_t slot_ts[STATE_SLOTS];
    int thumb_w[STATE_SLOTS], thumb_h[STATE_SLOTS]; /* 0 = no picture */
    bool thumb_upload;          /* atlas rebuilt: cache writeback */

    /* Allocated in open(), freed in close(); 512 KB saved at idle. */
    uint32_t *thumb_tex;
};
