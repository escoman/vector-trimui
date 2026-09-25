#include "gamecenter.h"
#include "filelist.h"
#include "imgload.h"
#include "options.h"
#include "font.h"
#include "layer_draw.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/*
 * Game Center input, catalog parsing and rasterization, see
 * gamecenter.h. The texture, palette and repaint machinery come from
 * the Popup base class; the window is repainted only when its visible
 * state changes.
 *
 * Unlike the PSP original nothing here blocks: the catalog, the
 * preview images and the ROM itself travel through the single
 * NetMan::AsyncFetch slot in its own thread, and poll_fetch() picks
 * the result up on a later UI tick. The window therefore keeps
 * repainting (with a progress line) while a download runs.
 */

/* Download caps: the catalog INI and one preview image. The ROM itself
 * is streamed straight to disk and is not limited here. */
static const size_t CATALOG_BUF_SIZE = 128 * 1024;
static const size_t PREVIEW_BUF_SIZE = 256 * 1024;

/* UI ticks (~50 Hz) the selection must stay put before its preview is
 * fetched: scrolling through the list must not start a download per row. */
static const int PREVIEW_IDLE_TICKS = 12;

/* ---- INI value helper -------------------------------------------- */

/* Strip leading/trailing whitespace and a pair of surrounding quotes. */
static void trim_value(char * dst, const char * src, int dst_len)
{
    while (*src == ' ' || *src == '\t')
        ++src;

    int len = (int)strlen(src);
    if (len >= 2 && src[0] == '"' && src[len - 1] == '"') {
        ++src;
        len -= 2;
    }

    while (len > 0 && (src[len - 1] == ' '  || src[len - 1] == '\t'
                       || src[len - 1] == '\r' || src[len - 1] == '\n'))
        --len;

    if (len >= dst_len)
        len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Percent-encode everything outside the unreserved set (plus '/'): the
 * catalog hands the file names over exactly as they are on the server,
 * and a space or a non-ASCII byte would break the request line. */
static std::string url_encode(const char * s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char * p = (const unsigned char *)s; *p; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' ||
            *p == '-' || *p == '~' || *p == '/') {
            out += (char)*p;
        } else {
            out += '%';
            out += hex[*p >> 4];
            out += hex[*p & 0x0f];
        }
    }
    return out;
}

/* ---- Constructor ---- */

GameCenter::GameCenter() :
    open_flag(false),
    count(0), selected(0), top(0),
    catalog_ok(false),
    entries(nullptr),
    preview_for_index(-1),
    preview_w(0), preview_h(0),
    fit_x(0), fit_y(0), fit_w(0), fit_h(0),
    preview_upload(false),
    idle_frames(0),
    load_requested(false),
    rom_ready(false),
    fetch_kind(FETCH_NONE),
    preview_tex(nullptr)
{
    reset_input_state();
    panel_w = PANEL_W;
    panel_h = PANEL_H;
    message[0] = '\0';
    rom_path[0] = '\0';
    rom_preview_path[0] = '\0';
}

GameCenter::~GameCenter()
{
    delete[] entries;
    delete[] preview_tex;
}

/* ---- Open / Close ---- */

void GameCenter::open()
{
    if (open_flag.load(std::memory_order_relaxed))
        return;

    /* Catalog and preview buffers live only while the window is open. */
    if (!entries)
        entries = new GameEntry[MAX_GAMES]();
    if (!preview_tex)
        preview_tex = new uint32_t[PREVIEW_TEX_W * PREVIEW_TEX_H]();

    selected = 0;
    top = 0;
    count = 0;
    catalog_ok = false;
    message[0] = '\0';
    preview_for_index = -1;
    preview_w = preview_h = 0;
    preview_upload = false;
    idle_frames = 0;
    load_requested = false;
    rom_ready = false;
    rom_path[0] = '\0';
    rom_preview_path[0] = '\0';
    fetch_kind = FETCH_NONE;
    reset_input_state();

    /* The WiFi link belongs to CrossMix/the OS: no init, no AP dialog,
     * just the catalog request. The window opens before it completes
     * and shows the progress. */
    open_flag.store(true, std::memory_order_release);
    fetch.start_memory(Options.catalog_url, CATALOG_BUF_SIZE);
    fetch_kind = FETCH_CATALOG;
    mark_dirty();
}

void GameCenter::close()
{
    /* Cancel first: the download thread writes into the buffers that
     * are released below. The wait is bounded by the socket timeout. */
    fetch.cancel();
    fetch_kind = FETCH_NONE;

    preview_for_index = -1;
    preview_w = preview_h = 0;
    preview_upload = false;

    delete[] entries;
    entries = nullptr;
    delete[] preview_tex;
    preview_tex = nullptr;

    open_flag.store(false, std::memory_order_release);
}

/* ---- Status ---- */

void GameCenter::set_status(const char * msg)
{
    if (strncmp(message, msg, sizeof(message)) == 0)
        return;                     /* same text: no repaint needed */
    snprintf(message, sizeof(message), "%s", msg);
    mark_dirty();
}

const char * GameCenter::selected_title() const
{
    if (count <= 0 || selected < 0 || selected >= count)
        return "";
    return entries[selected].title;
}

const char * GameCenter::fetch_label() const
{
    switch (fetch_kind) {
        case FETCH_CATALOG:     return "Catalog";
        case FETCH_PREVIEW:     return "Preview";
        case FETCH_ROM:         return "ROM";
        case FETCH_ROM_PREVIEW: return "Saving PNG";
        default:                return "";
    }
}

/* ---- INI parser ---- */

void GameCenter::parse_catalog(const char * data, int len)
{
    count = 0;
    GameEntry * cur = nullptr;

    const char * p = data;
    const char * end = data + len;

    while (p < end) {
        /* Find end of line. */
        const char * eol = p;
        while (eol < end && *eol != '\n')
            ++eol;

        /* Copy line into a temp buffer. */
        int line_len = (int)(eol - p);
        char line[512];
        if (line_len >= (int)sizeof(line))
            line_len = sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = '\0';
        p = eol + 1;

        /* Strip trailing CR. */
        if (line_len > 0 && line[line_len - 1] == '\r')
            line[--line_len] = '\0';

        /* Skip empty lines and comments. */
        const char * s = line;
        while (*s == ' ' || *s == '\t')
            ++s;
        if (*s == '\0' || *s == ';')
            continue;

        /* Section header: [name] */
        if (*s == '[') {
            const char * close = strchr(s, ']');
            if (close) {
                char section[128];
                int slen = (int)(close - s - 1);
                if (slen >= (int)sizeof(section))
                    slen = sizeof(section) - 1;
                memcpy(section, s + 1, slen);
                section[slen] = '\0';

                if (strcmp(section, "catalog") == 0) {
                    cur = nullptr;  /* metadata section, skip */
                } else if (count < MAX_GAMES) {
                    cur = &entries[count];
                    memset(cur, 0, sizeof(GameEntry));
                    snprintf(cur->key, TITLE_LEN, "%.*s",
                             TITLE_LEN - 1, section);
                    ++count;
                } else {
                    cur = nullptr;
                }
            }
            continue;
        }

        /* Key = value pair. */
        if (cur != nullptr) {
            const char * eq = strchr(s, '=');
            if (eq) {
                char key[64];
                int klen = (int)(eq - s);
                while (klen > 0 && (s[klen - 1] == ' ' || s[klen - 1] == '\t'))
                    --klen;
                if (klen >= (int)sizeof(key))
                    klen = sizeof(key) - 1;
                memcpy(key, s, klen);
                key[klen] = '\0';

                const char * val = eq + 1;

                if (strcmp(key, "title") == 0) {
                    trim_value(cur->title, val, TITLE_LEN);
                } else if (strcmp(key, "description") == 0) {
                    trim_value(cur->description, val, sizeof(cur->description));
                } else if (strcmp(key, "author") == 0) {
                    trim_value(cur->author, val, sizeof(cur->author));
                } else if (strcmp(key, "genre") == 0) {
                    trim_value(cur->genre, val, sizeof(cur->genre));
                } else if (strcmp(key, "year") == 0) {
                    trim_value(cur->year, val, sizeof(cur->year));
                } else if (strcmp(key, "rom_file") == 0) {
                    trim_value(cur->rom_file, val, PATH_LEN);
                } else if (strcmp(key, "preview") == 0) {
                    char raw[PATH_LEN];
                    trim_value(raw, val, PATH_LEN);
                    /* Take only the first path (before ';'). */
                    char * semi = strchr(raw, ';');
                    if (semi)
                        *semi = '\0';
                    snprintf(cur->preview, PATH_LEN, "%s", raw);
                } else if (strcmp(key, "size_bytes") == 0) {
                    cur->size_bytes = atoi(val);
                }
            }
        }
    }
}

/* ---- URLs ---- */

std::string GameCenter::build_url(const char * file) const
{
    return Options.download_url + url_encode(file);
}

/* ---- Background fetch ---- */

void GameCenter::poll_fetch()
{
    if (fetch_kind == FETCH_NONE)
        return;
    if (!fetch.poll())
        return;                         /* still running */

    const FetchKind kind = fetch_kind;
    const bool ok = fetch.ok();
    fetch_kind = FETCH_NONE;

    /* Take the payload out before the slot is reused by the next
     * request; poll() has already reaped the download thread. */
    std::vector<uint8_t> payload = fetch.take_data();

    switch (kind) {
        case FETCH_CATALOG:
            if (ok) {
                parse_catalog((const char *)payload.data(),
                              (int)payload.size());
                if (count > 0) {
                    catalog_ok = true;
                    message[0] = '\0';
                } else {
                    snprintf(message, sizeof(message), "Catalog is empty");
                }
            } else {
                snprintf(message, sizeof(message), "Catalog download failed");
            }
            break;

        case FETCH_PREVIEW:
            if (ok)
                decode_preview(payload);
            break;

        case FETCH_ROM:
            /* The ROM is on disk already; its preview is an optional
             * second step, and rom_ready is set when that finishes (or
             * is skipped), so the caller loads a complete pair. */
            if (ok)
                start_rom_preview();
            else
                snprintf(message, sizeof(message), "ROM download failed");
            break;

        case FETCH_ROM_PREVIEW:
            rom_ready = true;           /* a failed preview is not fatal */
            break;

        default:
            break;
    }

    mark_dirty();
}

/* ---- Preview ---- */

void GameCenter::start_preview()
{
    if (count <= 0 || selected < 0 || selected >= count)
        return;

    /* The cache slot is taken even when the fetch later fails, so a
     * missing preview is not retried on every idle window. */
    preview_for_index = selected;
    preview_w = preview_h = 0;

    const char * prev = entries[selected].preview;
    if (prev[0] == '\0')
        return;                         /* no preview for this entry */

    fetch.start_memory(build_url(prev), PREVIEW_BUF_SIZE);
    fetch_kind = FETCH_PREVIEW;
}

void GameCenter::decode_preview(const std::vector<uint8_t> & img)
{
    if (preview_tex == nullptr || img.empty())
        return;

    int w = 0, h = 0;
    if (!img_load_from_memory(img.data(), img.size(), preview_tex,
                              PREVIEW_TEX_W, PREVIEW_TEX_H, &w, &h))
        return;

    preview_w = w;
    preview_h = h;

    /* Stretched over the whole right pane. */
    fit_x = PREVIEW_X;
    fit_y = PREVIEW_Y;
    fit_w = PREVIEW_W;
    fit_h = PREVIEW_H;
    preview_upload = true;
}

/* ---- ROM download ---- */

void GameCenter::load_selected_rom()
{
    if (count <= 0 || selected < 0 || selected >= count)
        return;

    const GameEntry & entry = entries[selected];
    if (entry.rom_file[0] == '\0') {
        set_status("No ROM file");
        return;
    }

    const char * slash = strrchr(entry.rom_file, '/');
    const std::string name = (slash != nullptr) ? slash + 1 : entry.rom_file;
    if (name.empty()) {
        set_status("Bad ROM name");
        return;
    }

    /* The ROM lands in the directory the ROM Browser lists, so a
     * downloaded game is available offline from then on. */
    const std::string dir = FileList::default_rom_dir();
    snprintf(rom_path, sizeof(rom_path), "%s/%s", dir.c_str(), name.c_str());

    /* "<base>.png" next to it: the ROM Browser preview slot. */
    const size_t dot = name.find_last_of('.');
    const std::string base =
        (dot == std::string::npos) ? name : name.substr(0, dot);
    snprintf(rom_preview_path, sizeof(rom_preview_path), "%s/%s.png",
             dir.c_str(), base.c_str());

    rom_ready = false;
    fetch.start_file(build_url(entry.rom_file), rom_path);
    fetch_kind = FETCH_ROM;
    set_status("ROM ...");
}

void GameCenter::start_rom_preview()
{
    if (selected < 0 || selected >= count ||
        entries[selected].preview[0] == '\0' ||
        rom_preview_path[0] == '\0') {
        rom_ready = true;               /* nothing more to fetch */
        return;
    }

    fetch.start_file(build_url(entries[selected].preview), rom_preview_path);
    fetch_kind = FETCH_ROM_PREVIEW;
}

/* ---- Input ---- */

void GameCenter::update(unsigned pad)
{
    if (!open_flag.load(std::memory_order_relaxed))
        return;

    poll_fetch();

    if (fetch_kind != FETCH_NONE) {
        /* Downloading: show the progress, the list is not interactive
         * (the PSP flow blocked here outright). */
        char st[48];
        const long long got = fetch.received();
        const long long total = fetch.total();
        if (total > 0)
            snprintf(st, sizeof(st), "%s %d%%", fetch_label(),
                     (int)(got * 100 / total));
        else
            snprintf(st, sizeof(st), "%s %lldK", fetch_label(), got / 1024);
        set_status(st);

        prev_pad = pad;
        return;
    }

    if (count <= 0) {
        prev_pad = pad;
        return;
    }

    bool moved = false;
    if (keyup_edge(pad, GC_PAD_DOWN)) {
        selected = (selected + 1) % count;
        moved = true;
    }
    if (keyup_edge(pad, GC_PAD_UP)) {
        selected = (selected + count - 1) % count;
        moved = true;
    }
    if (moved) {
        if (selected < top)
            top = selected;
        if (selected >= top + VISIBLE_ROWS)
            top = selected - VISIBLE_ROWS + 1;

        idle_frames = 0;            /* restart the idle counter */
        mark_dirty();
    }

    /* Lazy preview: fetch only once the selection has settled. */
    ++idle_frames;
    if (idle_frames == PREVIEW_IDLE_TICKS && preview_for_index != selected) {
        start_preview();
        mark_dirty();
    }

    /* A requests the load; the caller shows the confirmation dialog. */
    if (keyup_edge(pad, GC_PAD_PRESS)) {
        load_requested = true;
        idle_frames = 0;            /* no preview reload in between */
    }

    prev_pad = pad;
}

/* ---- Paint ---- */

void GameCenter::paint()
{
    const unsigned seq = snapshot_seq();

    const int header_w = PANEL_W - PAD_X * 2;
    const int list_y0 = PAD_Y + TITLE_H + HDR_GAP + 1 + HDR_GAP;
    const int footer_y = PANEL_H - PAD_Y - FOOTER_H;

    fill_rect(0, 0, PANEL_W, PANEL_H, C_PANEL_BG);

    /* Header: until the catalog is in, the status message replaces the
     * title, same pattern as the MainMenu "Saving..." status. */
    if (message[0] != '\0' && !catalog_ok) {
        const int tw = (int)strlen(message) * OVERLAY_FONT_W * 2;
        print_text2x((PANEL_W - tw) / 2, PAD_Y, message, C_TEXT_WHITE);
    } else {
        print_text2x(PAD_X, PAD_Y, "Game Center", C_TEXT_WHITE);
        char count_text[32];
        snprintf(count_text, sizeof(count_text), "Games: %d", count);
        const int cw = (int)strlen(count_text) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - cw, PAD_Y, count_text, C_TEXT_WHITE);
    }
    fill_rect(PAD_X, PAD_Y + TITLE_H + HDR_GAP, header_w, 1, C_PANEL_BORDER);

    /* Divider between the list and the preview pane. */
    fill_rect(PREVIEW_X - DIV_GAP / 2, list_y0, 1, footer_y - list_y0,
              C_PANEL_BORDER);

    if (!catalog_ok) {
        /* The error/status message is already in the header; leave the
         * list area empty. */
    } else if (count == 0) {
        const char * msg = "Catalog is empty";
        const int mw = (int)strlen(msg) * OVERLAY_FONT_W * 2;
        print_text2x((PANEL_W - mw) / 2,
                     list_y0 + (VISIBLE_ROWS * ROW_H) / 2 - OVERLAY_FONT_H,
                     msg, C_TEXT_WHITE);
    } else {
        const int max_chars = LIST_W / (OVERLAY_FONT_W * 2);
        for (int r = 0; r < VISIBLE_ROWS; ++r) {
            const int idx = top + r;
            if (idx >= count)
                break;
            const int y = list_y0 + r * ROW_H;
            const bool sel = (idx == selected);
            fill_rect(PAD_X, y, LIST_W, ROW_H - 2,
                      sel ? C_ITEM_BG_SEL : C_ITEM_BG);

            char shown[TITLE_LEN];
            snprintf(shown, sizeof(shown), "%s", entries[idx].title);
            shown[max_chars < TITLE_LEN ? max_chars : TITLE_LEN - 1] = '\0';
            print_text2x(PAD_X + 4, y + (ROW_H - 2 - OVERLAY_FONT_H * 2) / 2,
                         shown, sel ? C_TEXT_BLACK : C_TEXT_WHITE);
        }
    }

    /* Footer: download progress / last error on the left (1x, it can be
     * longer than the 2x hints), key hints on the right. */
    if (catalog_ok && message[0] != '\0')
        print_text(PAD_X, footer_y + (FOOTER_H - OVERLAY_FONT_H) / 2,
                   message, C_TEXT_WHITE);
    {
        const char * hints = catalog_ok ? "A:Load B:Back" : "B:Back";
        const int hw = (int)strlen(hints) * OVERLAY_FONT_W * 2;
        print_text2x(PANEL_W - PAD_X - hw, footer_y, hints, C_TEXT_WHITE);
    }

    finish_paint(seq);
}

void GameCenter::draw()
{
    /* Panel quad first. */
    UILayer::draw();
    /* Preview on top of the right pane. */
    draw_preview();
}

void GameCenter::draw_preview()
{
    if (!has_preview())
        return;

    /* The upload flag is consumed only when a quad is really drawn:
     * the SDL texture cache in layer_draw.cpp refreshes from the
     * (possibly recycled) data pointer on this flag. */
    const bool upload = consume_preview_upload();

    int fx, fy, fw, fh;
    get_preview_rect(&fx, &fy, &fw, &fh);
    const float x0 = ((float)LAYER_SCREEN_W - (float)get_width()) / 2.0f + (float)fx;
    const float y0 = ((float)LAYER_SCREEN_H - (float)get_height()) / 2.0f + (float)fy;
    const float uw = (float)get_preview_w();
    const float vh = (float)get_preview_h();

    layer_draw_rgba_quad(
        preview_tex_data(), PREVIEW_TEX_W, PREVIEW_TEX_H,
        x0, y0, (float)fw, (float)fh,
        0.0f, 0.0f, uw, vh,
        /* bilinear */ true, upload);
}
