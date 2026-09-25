#pragma once

#include <atomic>
#include <inttypes.h>
#include "popup.h"

/*
 * ROM Browser: the UI window opened from the MAIN MENU's
 * "Load ROM" item. UI state machine of this port:
 *
 *     enum class UIState { GAME, MAIN_MENU, ROM_BROWSER };
 *
 *   ROM_BROWSER - the machine stays frozen via the Emulator pause
 *                 flag (paused == true for the whole lifetime of the
 *                 window: no CPU/Memory/IO/sound work, no frames
 *                 published), the last Vector frame is the backdrop,
 *                 the window is drawn above the dim overlay in the
 *                 480x272 UI coordinate space.
 *
 * Transitions (all on the UI thread, see Emulator::gui_tick):
 *
 *   MAIN_MENU --A on Load ROM--> ROM_BROWSER   (menu closes, still paused)
 *   ROM_BROWSER --B/START------> MAIN_MENU     (focus back on Load ROM)
 *   ROM_BROWSER --A on a ROM---> GAME          (ROM loaded, resumed)
 *
 * The texture / repaint machinery is shared through the Popup base
 * class:
 *
 *   UI thread: open()/close()/update() mutate the state; open()
 *   rescans the ROM folder every time so the list is always fresh.
 *
 *   Same thread renders: needs_repaint()/paint() rasterize the
 *   window; TV::render() presents it as one overlay quad.
 *
 * The scanning itself reuses FileList::listRoms() (same path
 * handling, extension filter and sort order as the PSP port); the
 * actual ROM loading goes through Emulator::load_rom(). This class
 * never touches CPU/Memory directly.
 *
 * Preview: next to the ROM list the window shows a static picture
 * of the selected ROM: <base>.png in the same directory
 * (FileList::findPreview), decoded by img_load into a private RGBA
 * texture and presented as a second quad over the right pane.
 * The preview updates together with the selection and is cached:
 * the file is decoded once per ROM (a missing/broken file is cached
 * as "no preview" too, never retried every frame), released on
 * close(). Decode errors never surface to the user.
 */

/* Normalized pad state passed to RomBrowser::update(): which
 * buttons are currently held. Keeps rombrowser free of SDL. */
enum {
    RB_PAD_UP   = 0x01,
    RB_PAD_DOWN = 0x02,
};

class RomBrowser : public Popup
{
public:
    /* Window size, UI coordinate space (480x272); the renderer
     * centers it, leaving 20/26 px margins around. */
    static const int PANEL_W = 440;
    static const int PANEL_H = 220;

    /* Layout constants. */
    static const int PAD_X = 8;       /* window left/right padding */
    static const int PAD_Y = 8;       /* window top/bottom padding */
    static const int TITLE_H = 16;    /* header row, 8x8 font at 2x */
    static const int HDR_GAP = 4;     /* gap around the header divider */
    static const int ROW_H = 20;      /* one ROM row */
    static const int VISIBLE_ROWS = 8;/* rows fitting into the list area */
    static const int FOOTER_H = 16;   /* bottom hint/status row */

    /* Left pane: the ROM list. Right pane: the preview of the
     * selected ROM, its bounds derived from the panel. */
    static const int LIST_W = 208;    /* row width of the ROM list */
    static const int DIV_GAP = 8;     /* list -> divider -> preview */
    static const int PREVIEW_X = PAD_X + LIST_W + DIV_GAP;
    static const int PREVIEW_W = PANEL_W - PAD_X - PREVIEW_X;
    static const int PREVIEW_Y = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    static const int PREVIEW_H = PANEL_H - PAD_Y - FOOTER_H - PREVIEW_Y;

    /* Preview texture: power-of-two dimensions; the decoded image
     * occupies its top-left preview_w x preview_h window. */
    static const int PREVIEW_TEX_W = 256;
    static const int PREVIEW_TEX_H = 256;

    /* Fixed name storage instead of a shared std::vector: filled in
     * open(), read while painting. */
    static const int MAX_ROMS = 256;
    static const int NAME_LEN = 64;

    RomBrowser();

    /* Atomic: written and read on the UI thread; the handshake is
     * inherited from the PSP port and harmless here. */
    bool is_open() const override { return this->open_flag.load(std::memory_order_acquire); }

    /* Rescan the ROM folder (fresh list on every open), reset the
     * selection to the first ROM. */
    void open(const char * rom_dir);
    /* ROM_BROWSER -> MAIN_MENU or GAME. */
    void close();

    /* One input step; called on the UI thread (~50 Hz) while
     * open. UP/DOWN navigate cyclically, firing on the keyup edge
     * (one press = one row), and scroll
     * the window; with an empty list every key is a no-op. A/B/
     * START edges are handled by the caller (load / back). */
    void update(unsigned pad);

    bool has_items() const { return this->count > 0; }
    /* File name of the selected ROM. */
    const char * selected_name() const;

    /* Status line shown in the footer (load error), cleared on the
     * next open(). */
    void set_error(const char * msg);

    /* Preview access for draw_preview(). preview_w == 0 means "no
     * preview". */
    bool has_preview() const { return this->preview_w > 0; }
    const uint32_t * preview_tex_data() const { return preview_tex; }
    int get_preview_w() const { return this->preview_w; }
    int get_preview_h() const { return this->preview_h; }
    /* Panel-local destination rectangle of the preview quad: the
     * picture is stretched over the whole right pane (full pane
     * height). */
    void get_preview_rect(int * x, int * y, int * w, int * h) const
    {
        *x = fit_x; *y = fit_y; *w = fit_w; *h = fit_h;
    }
    /* True once after a new image was decoded: the renderer must
     * re-upload the texture before sampling. Consumed by draw(). */
    bool consume_preview_upload()
    {
        bool v = preview_upload;
        preview_upload = false;
        return v;
    }

    /* Rasterize the window into the popup texture; a state change
     * arriving mid-paint forces one more pass. */
    void paint() override;

    /* Draw the preview quad over the right pane. */
    void draw() override;

private:
    void draw_preview();
    /* Decode the preview of the selected ROM unless its result
     * (image or "no preview") is already cached. */
    void update_preview();

    std::atomic<bool> open_flag;

    /* Written in open()/update(), read while painting. */
    int count;
    int selected;
    int top;            /* first visible row (scrolling) */
    bool dir_ok;        /* ROMS folder opened successfully */
    char names[MAX_ROMS][NAME_LEN];
    char message[NAME_LEN]; /* footer status line, empty = none */
    char rom_dir[128];      /* stored by open() for the preview search */

    /* Preview state: written in open()/update(), the texture and
     * the fit rect are read while drawing. */
    char preview_for[NAME_LEN]; /* ROM name the cache entry belongs to;
                                 * covers negative results too, so a
                                 * missing image is never re-probed */
    int preview_w, preview_h;   /* decoded image size, 0 = none */
    int fit_x, fit_y, fit_w, fit_h; /* panel-local quad rectangle */
    bool preview_upload;        /* new image: cache writeback needed */

    /* Allocated in open(), freed in close(); 256 KB saved at idle. */
    uint32_t *preview_tex;
};
