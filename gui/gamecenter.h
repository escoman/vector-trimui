#pragma once

#include <atomic>
#include <inttypes.h>
#include <vector>
#include "popup.h"
#include "netman.h"

/*
 * GAME CENTER: online ROM catalog browser opened from the MAIN MENU.
 *
 * Flow:
 *   1. open() starts the catalog download in the background and shows
 *      the window with a progress line; when the fetch lands, the INI
 *      is parsed and the list appears.
 *   2. Navigation: UP/DOWN scroll the list; after the selection has
 *      been idle for a moment the preview image of the selected entry
 *      is downloaded (cached per entry).
 *   3. A requests a load: the caller confirms with the MessageDialog,
 *      then perform_load_request() downloads the ROM (and its preview)
 *      into the ROM directory and sets has_rom_ready().
 *   4. close() cancels a running download and frees the buffers.
 *
 * Threading on TrimUI: open()/close()/update() run on the UI thread
 * inside Emulator::gui_tick() (~50 Hz) and must never block, so every
 * download lives in NetMan::AsyncFetch's own thread; paint() runs on
 * the render thread as for the other popups.
 */

/* Normalized pad state passed to GameCenter::update(). */
enum {
    GC_PAD_UP    = 0x01,
    GC_PAD_DOWN  = 0x02,
    GC_PAD_PRESS = 0x04,  /* A button - load ROM */
    GC_PAD_BACK  = 0x08,  /* B/START - back */
    GC_PAD_LEFT  = 0x10,
    GC_PAD_RIGHT = 0x20,
};

class GameCenter : public Popup
{
public:
    /* Window size, same as RomBrowser. */
    static const int PANEL_W = 440;
    static const int PANEL_H = 220;

    /* Layout constants. */
    static const int PAD_X = 8;
    static const int PAD_Y = 8;
    static const int TITLE_H = 16;
    static const int HDR_GAP = 4;
    static const int ROW_H = 20;
    static const int VISIBLE_ROWS = 8;
    static const int FOOTER_H = 16;

    /* Two panes: list left, preview right. */
    static const int LIST_W = 208;
    static const int DIV_GAP = 8;
    static const int PREVIEW_X = PAD_X + LIST_W + DIV_GAP;
    static const int PREVIEW_W = PANEL_W - PAD_X - PREVIEW_X;
    static const int PREVIEW_Y = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    static const int PREVIEW_H = PANEL_H - PAD_Y - FOOTER_H - PREVIEW_Y;

    /* Preview texture: power-of-two dimensions. */
    static const int PREVIEW_TEX_W = 256;
    static const int PREVIEW_TEX_H = 256;

    /* Catalog limits. */
    static const int MAX_GAMES = 128;
    static const int TITLE_LEN = 64;
    static const int PATH_LEN = 256;

    GameCenter();
    ~GameCenter();

    bool is_open() const override { return this->open_flag.load(std::memory_order_acquire); }

    /* UI thread: start the catalog download and show the window. */
    void open();
    /* UI thread: cancel a running download, free the buffers. */
    void close();

    /* One input step; called by the UI thread (~50 Hz). Also polls the
     * background fetch, which is how the catalog/preview/ROM results
     * reach the window. */
    void update(unsigned pad);

    bool has_items() const { return this->count > 0; }
    const char * selected_title() const;

    /* Footer status line. */
    void set_status(const char * msg);

    /* Preview access for draw_preview(). */
    bool has_preview() const { return this->preview_w > 0; }
    const uint32_t * preview_tex_data() const { return preview_tex; }
    int get_preview_w() const { return this->preview_w; }
    int get_preview_h() const { return this->preview_h; }
    void get_preview_rect(int * x, int * y, int * w, int * h) const
    {
        *x = fit_x; *y = fit_y; *w = fit_w; *h = fit_h;
    }
    bool consume_preview_upload()
    {
        bool v = preview_upload;
        preview_upload = false;
        return v;
    }

    /* ROM download access. */
    bool has_rom_ready() const { return rom_ready; }
    const char * get_rom_path() const { return rom_path; }
    void clear_rom_ready() { rom_ready = false; rom_path[0] = '\0'; }

    /* Confirmation dialog: when the user presses A on a ROM, this
     * flag is set. The caller shows the MessageDialog and, on YES,
     * calls perform_load_request() to start the download. */
    bool consume_load_request()
    {
        if (!load_requested) return false;
        load_requested = false;
        return true;
    }
    void perform_load_request() { load_selected_rom(); }

    /* Rasterize the window. Render thread only. */
    void paint() override;

    /* Render thread: draw the preview quad over the right pane. */
    void draw() override;

private:
    /* What the single background fetch slot is currently carrying. */
    enum FetchKind {
        FETCH_NONE = 0,
        FETCH_CATALOG,
        FETCH_PREVIEW,
        FETCH_ROM,
        FETCH_ROM_PREVIEW,
    };

    struct GameEntry {
        char key[TITLE_LEN];
        char title[TITLE_LEN];
        char description[256];
        char author[64];
        char genre[32];
        char year[16];
        char rom_file[PATH_LEN];
        char preview[PATH_LEN];  /* first preview path only */
        int  size_bytes;
    };

    void draw_preview();

    void poll_fetch();
    const char * fetch_label() const;
    void start_preview();
    void decode_preview(const std::vector<uint8_t> & img);
    void load_selected_rom();
    void start_rom_preview();

    void parse_catalog(const char * data, int len);
    /* download_url + the percent-encoded server-relative file name. */
    std::string build_url(const char * file) const;

    std::atomic<bool> open_flag;

    int count;
    int selected;
    int top;            /* first visible row (scrolling) */
    char message[64];
    bool catalog_ok;

    /* Allocated in open(), freed in close(). */
    GameEntry *entries;

    /* Preview state (same scheme as RomBrowser). */
    int preview_for_index;  /* entry the cache belongs to; -1 = none */
    int preview_w, preview_h;
    int fit_x, fit_y, fit_w, fit_h;
    bool preview_upload;
    int idle_frames;        /* frames since last selection change */

    /* Load request: set by update() when the user presses A, consumed
     * by the caller (which shows the MessageDialog first). */
    bool load_requested;

    /* ROM download state. */
    char rom_path[PATH_LEN];          /* downloaded ROM, empty if none */
    char rom_preview_path[PATH_LEN];  /* <rom base>.png next to it */
    bool rom_ready;                   /* ROM is on disk, ready to load */

    /* The one background download slot plus what it is fetching. */
    NetMan::AsyncFetch fetch;
    FetchKind fetch_kind;

    /* Allocated in open(), freed in close(). */
    uint32_t *preview_tex;
};
