#include "vkbd.h"
#include "vkbd_font.h"
#include "layer_draw.h"

#include <sstream>
#include <algorithm>
#include <cstring>

/*
 * UTF-8 -> CP866 for the key legends (adapted from the libretro core
 * conv.cpp). prepare() runs it once; the rasterizer works on CP866
 * bytes indexing vkbd_font[].
 */
static std::string utf8_to_cp866(const std::string & input)
{
    std::string output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        uint8_t c = static_cast<uint8_t>(input[i]);

        if (c == 0xD0 && (i + 1) < input.size()) {
            uint8_t next = static_cast<uint8_t>(input[++i]);
            if (next == 0x81) output += (char)0xF0;            /* Ё */
            else if (next >= 0x90 && next <= 0xBF)
                output += (char)(next - 0x90 + 0x80);          /* А-Я */
        }
        else if (c == 0xD1 && (i + 1) < input.size()) {
            uint8_t next = static_cast<uint8_t>(input[++i]);
            if (next == 0x91) output += (char)0xF1;            /* ё */
            else if (next >= 0x80 && next <= 0x8F)
                output += (char)(next - 0x80 + 0xE0);          /* р-я */
        }
        else if (c == 0xC2 && (i + 1) < input.size()) {
            uint8_t next = static_cast<uint8_t>(input[++i]);
            if (next == 0xa4) output += (char)0xfd;            /* ¤ */
            else output += '?';
        }
        else if (c == 0xe2 && (i + 2) < input.size()) {        /* arrows */
            uint8_t n1 = static_cast<uint8_t>(input[++i]);
            if (n1 == 0x86) {
                uint8_t n2 = static_cast<uint8_t>(input[++i]);
                switch (n2) {
                    case 0x96: output += (char)0x1c; break;    /* ↖ */
                    case 0x91: output += (char)0x18; break;    /* ↑ */
                    case 0x90: output += (char)0x1b; break;    /* ← */
                    case 0x93: output += (char)0x19; break;    /* ↓ */
                    case 0x92: output += (char)0x1a; break;    /* → */
                    default:   output += '?';
                }
            }
        }
        else {
            output += (char)c;
        }
    }
    return output;
}

static uint32_t rgb_to_psp(uint8_t r, uint8_t g, uint8_t b)
{
    /* Same layout as the GE CLUT built in TV::init(): memory order
     * A B G R. */
    return 0xff000000u |
           ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

/* mix(color, white, 0.3), like Graphics32::mix for pressed keys */
static uint32_t lighten_rgb(uint32_t rgb)
{
    const int r = (int)((rgb >> 16) & 0xff);
    const int g = (int)((rgb >> 8) & 0xff);
    const int b = (int)(rgb & 0xff);
    const int nr = (r * 7 + 255 * 3 + 5) / 10;
    const int ng = (g * 7 + 255 * 3 + 5) / 10;
    const int nb = (b * 7 + 255 * 3 + 5) / 10;
    return rgb_to_psp((uint8_t)nr, (uint8_t)ng, (uint8_t)nb);
}

VirtualKeyboard::VirtualKeyboard() :
    kb_width(0), kb_height(0),
    select_row(2), select_col(5),
    finger_row(2), finger_col(5),
    visible(false), top(false),
    prev_pad(0), autorepeat_count(-1),
    last_ruslat(false), ruslat_src(nullptr),
    paint_seq(1), painted_seq(0), tex_upload(false)
{
    /* Same proportions as the original 34x20 keys, scaled down so the
     * 17.5-unit wide layout fits into the 480-pixel PSP display. The
     * key height keeps the original 20 pixels: the two 8-pixel legend
     * rows do not fit into anything shorter. */
    unit_w = 27;
    unit_h = 20;

    xgap = 1;
    ygap = 1;

    keys_down.fill(0);
    sticky_down.fill(0);
    key_map.fill(key_info_t{});

    memset(tex, 0, sizeof(tex));

    /* Keyboard palette: the colormap of the original vkbd, converted
     * to the GE CLUT format; entries 11..14 are the pressed-key
     * (lightened) variants of ALPHA/BROWN/GREEN/FN. */
    memset(clut, 0, sizeof(clut));
    clut[C_BACKGROUND]      = rgb_to_psp(0xb0, 0xb0, 0xa0);
    clut[C_KEY_BORDER]      = rgb_to_psp(0x40, 0x40, 0x40);
    clut[C_KEY_ALPHA]       = rgb_to_psp(0xbb, 0xb5, 0xa5);
    clut[C_KEY_BROWN]       = rgb_to_psp(0x40, 0x25, 0x06);
    clut[C_KEY_GREEN]       = rgb_to_psp(0x7e, 0x7f, 0x65);
    clut[C_KEY_FN]          = rgb_to_psp(0x9e, 0x97, 0x76);
    clut[C_KEY_TEXT]        = rgb_to_psp(0x00, 0x00, 0x00);
    clut[C_KEY_TEXT_BROWN]  = rgb_to_psp(0x80, 0x80, 0x80);
    clut[C_KEY_BORDER_SELECT] = rgb_to_psp(0xff, 0xff, 0xff);
    clut[C_LED_ON]          = rgb_to_psp(0xff, 0x40, 0x40);
    clut[C_LED_OFF]         = rgb_to_psp(0x40, 0x10, 0x10);
    clut[C_PRESSED_BASE + 0] = lighten_rgb(0x00bbb5a5);
    clut[C_PRESSED_BASE + 1] = lighten_rgb(0x00402506);
    clut[C_PRESSED_BASE + 2] = lighten_rgb(0x007e7f65);
    clut[C_PRESSED_BASE + 3] = lighten_rgb(0x009e9776);

    /* The CLUT is expanded into an RGBA scratch buffer at upload time
     * on the SDL backend (layer_draw.cpp); repaints re-upload it via
     * consume_tex_upload(), so no cache writeback is needed here. */
}

void VirtualKeyboard::show(unsigned pad_snapshot)
{
    visible = true;
    prev_pad = pad_snapshot;
    autorepeat_count = -1;
}

void VirtualKeyboard::hide(unsigned pad_snapshot)
{
    release_all();
    visible = false;
    prev_pad = pad_snapshot;
}

void VirtualKeyboard::release_all()
{
    for (unsigned i = 0; i < keys_down.size(); ++i) {
        if (keys_down[i] != 0) {
            if (on_keyup) on_keyup(keys_down[i]);
            keys_down[i] = 0;
        }
    }
    for (unsigned i = 0; i < sticky_down.size(); ++i) {
        if (sticky_down[i] != 0) {
            if (on_keyup) on_keyup(sticky_down[i]);
            sticky_down[i] = 0;
        }
    }
    for (key_info_t & ki : key_map) {
        ki.pressed = false;
    }
    autorepeat_count = -1;
    paint_seq.fetch_add(1, std::memory_order_relaxed);
}

/* D-pad edge/autorepeat: the direction is held and either freshly
 * pressed or the repeat counter expired. */
static inline bool pad_edge_or_repeat(unsigned pad, unsigned prev_pad,
                                      unsigned mask, int autorepeat_count)
{
    return (pad & mask) != 0 &&
           ((prev_pad & mask) == 0 || autorepeat_count == 0);
}

void VirtualKeyboard::update(unsigned pad)
{
    if (!visible)
        return;

    if (autorepeat_count > 0)
        --autorepeat_count;

    if (pad == prev_pad && autorepeat_count != 0)
        return;

    const bool b_down = (pad & VKBD_PAD_PRESS) != 0;   /* X */
    const bool b_trig = b_down && (prev_pad & VKBD_PAD_PRESS) == 0;
    const bool b_release = !b_down && (prev_pad & VKBD_PAD_PRESS) != 0;

    if (pad_edge_or_repeat(pad, prev_pad, VKBD_PAD_RIGHT, autorepeat_count)) {
        key_up(selected(), true);
        move_finger(+1, 0);
        autorepeat_count = (autorepeat_count == -1) ? autorepeat_delay : autorepeat_rate;
        paint_seq.fetch_add(1, std::memory_order_relaxed);
    }
    if (pad_edge_or_repeat(pad, prev_pad, VKBD_PAD_LEFT, autorepeat_count)) {
        key_up(selected(), true);
        move_finger(-1, 0);
        autorepeat_count = (autorepeat_count == -1) ? autorepeat_delay : autorepeat_rate;
        paint_seq.fetch_add(1, std::memory_order_relaxed);
    }
    if (pad_edge_or_repeat(pad, prev_pad, VKBD_PAD_UP, autorepeat_count)) {
        key_up(selected(), true);
        move_finger(0, -1);
        autorepeat_count = (autorepeat_count == -1) ? autorepeat_delay : autorepeat_rate;
        paint_seq.fetch_add(1, std::memory_order_relaxed);
    }
    if (pad_edge_or_repeat(pad, prev_pad, VKBD_PAD_DOWN, autorepeat_count)) {
        key_up(selected(), true);
        move_finger(0, +1);
        autorepeat_count = (autorepeat_count == -1) ? autorepeat_delay : autorepeat_rate;
        paint_seq.fetch_add(1, std::memory_order_relaxed);
    }

    if (b_trig) {
        /* Modifiers (СС/УС) latch as sticky keys so a second X
         * press on another key forms a combination; normal keys are
         * momentary (keydown now, keyup when X is released). РУС/ЛАТ
         * is a mode key: a momentary press flips the mode latch. */
        key_down(selected(), is_sticky_scancode(selected().scancode));
        paint_seq.fetch_add(1, std::memory_order_relaxed);
    }
    if (b_release) {
        key_up(selected(), true);
        paint_seq.fetch_add(1, std::memory_order_relaxed);
    }

    prev_pad = pad;

    if (pad == 0) {
        autorepeat_count = -1;
    }
}

bool VirtualKeyboard::is_sticky_scancode(int scancode)
{
    /* Only the real hold-modifiers (СС/УС and both shifts). РУС/ЛАТ
     * (F6) is intentionally not here: it is a mode toggle key. */
    return scancode == SDL_SCANCODE_LSHIFT ||
           scancode == SDL_SCANCODE_RSHIFT ||
           scancode == SDL_SCANCODE_LCTRL ||
           scancode == SDL_SCANCODE_RCTRL;
}

VirtualKeyboard::key_info_t & VirtualKeyboard::selected()
{
    return key_map.at(select_row * NUM_COLS + select_col);
}

void VirtualKeyboard::key_down(key_info_t & ki, bool sticky)
{
    if (ki.scancode <= 0)
        return;

    unsigned first_empty = keys_down.size();
    for (unsigned i = 0; i < keys_down.size(); ++i) {
        if (keys_down[i] == ki.scancode) {
            return;
        }
        if (keys_down[i] == 0) {
            first_empty = i;
        }
    }

    if (sticky) {
        for (unsigned i = 0; i < sticky_down.size(); ++i) {
            if (sticky_down[i] == ki.scancode) {
                sticky_down[i] = 0;
                if (on_keyup) on_keyup(ki.scancode);
                ki.pressed = false;
                break;
            }
            else if (sticky_down[i] == 0) {
                sticky_down[i] = ki.scancode;
                if (on_keydown) on_keydown(ki.scancode);
                ki.pressed = true;
                break;
            }
        }
        return;
    }

    if (first_empty >= keys_down.size())
        return;
    keys_down[first_empty] = ki.scancode;
    if (on_keydown) on_keydown(ki.scancode);
    ki.pressed = true;
}

void VirtualKeyboard::key_up(key_info_t & ki, bool unstick)
{
    if (ki.scancode <= 0)
        return;

    for (unsigned i = 0; i < keys_down.size(); ++i) {
        if (keys_down[i] == ki.scancode) {
            keys_down[i] = 0;
            if (on_keyup) on_keyup(ki.scancode);
            ki.pressed = false;

            if (unstick)
                unstick_stickies();
            return;
        }
    }
}

void VirtualKeyboard::unstick_stickies()
{
    bool nothing = true;
    for (unsigned i = 0; i < sticky_down.size(); ++i) {
        nothing &= sticky_down[i] == 0;
    }
    if (nothing) return;

    for (key_info_t & sticky : key_map) {
        for (unsigned i = 0; i < sticky_down.size(); ++i) {
            if (sticky_down[i] == sticky.scancode) {
                if (on_keyup) on_keyup(sticky_down[i]);
                sticky.pressed = false;
                sticky_down[i] = 0;
            }
        }
    }
}

void VirtualKeyboard::move_finger(int dx, int dy)
{
    finger_row = std::clamp(finger_row + dy, 0, NUM_ROWS - 1);
    finger_col = std::clamp(finger_col + dx, 0, NUM_COLS);

    select_row = finger_row;

    int prev_col = select_col;

    int again_finger_col = -1;
    while (again_finger_col != finger_col) {
        again_finger_col = finger_col;
        /* Search the key under the finger by its pixel position, so
         * wide keys (space) are stepped over instead of stopped on.
         * The position is the center of the nominal finger column,
         * and the test is a half-open [x, x+width) interval, so the
         * movement is symmetric: one D-pad press moves one key. */
        int finger_x = finger_col * unit_w + unit_w / 2;
        for (int i = 0; i < NUM_COLS; ++i) {
            key_info_t & ki = key_map.at(select_row * NUM_COLS + i);
            if (ki.scancode == 0) continue;

            if (ki.x <= finger_x && ki.x + ki.width > finger_x) {
                select_col = i;
                if (select_col == prev_col) {
                    finger_col = std::clamp(finger_col + dx, 0, NUM_COLS);
                }
                break;
            }
        }
    }
}

/* --- layout ------------------------------------------------------ */

int VirtualKeyboard::pixel_width(unit_width_t uwidth) const
{
    switch (uwidth) {
        case unit_width_t::U0_5: return unit_w / 2;
        case unit_width_t::U1:   return unit_w;
        case unit_width_t::U1_5: return unit_w * 3 / 2;
        case unit_width_t::U7:   return unit_w * 7;
    }
    return unit_w;
}

void VirtualKeyboard::make_key_info(key_info_t & ki, int col, int row,
                                    const std::string & L1,
                                    const std::string & L2)
{
    /* x/y are filled by prepare(); colors and width by coord */
    ki.color = C_KEY_ALPHA;
    ki.text_color = C_KEY_TEXT;
    unit_width_t unit_width = unit_width_t::U1;
    ki.coord = row * 100 + col;

    for (unsigned i = 0; i < sizeof(longKeys)/sizeof(longKeys[0]); ++i) {
        if (longKeys[i] == ki.coord) {
            unit_width = unit_width_t::U1_5;
            ki.color = C_KEY_BROWN;
            ki.text_color = C_KEY_TEXT_BROWN;
        }
    }
    if (ki.coord == spaceKey) {
        unit_width = unit_width_t::U7;
        ki.color = C_KEY_GREEN;
    }
    for (unsigned i = 0; i < sizeof(greenishKeys)/sizeof(greenishKeys[0]); ++i) {
        if (greenishKeys[i] == ki.coord) ki.color = C_KEY_GREEN;
    }
    for (unsigned i = 0; i < sizeof(mustardKeys)/sizeof(mustardKeys[0]); ++i) {
        if (mustardKeys[i] == ki.coord) ki.color = C_KEY_FN;
    }
    ki.width = pixel_width(unit_width);

    ki.legend_1 = L1;
    ki.legend_2 = L2;
}

void VirtualKeyboard::prepare()
{
    int cur_x = 0;
    int cur_y = TOP_BORDER;
    kb_width = 0;

    for (unsigned row = 0; row < 5; ++row)
    {
        if (row == 1) {
            cur_x += pixel_width(unit_width_t::U0_5);
        }

        int col = 0;
        {
            std::string line = utf8_to_cp866(top_text[row]);
            std::stringstream ss(line);
            std::string word;

            std::string bline = utf8_to_cp866(bottom_text[row]);
            std::stringstream bs(bline);
            std::string bword;

            while (ss >> word && bs >> bword) {
                key_info_t ki{};

                make_key_info(ki, col, row, word, bword);
                ki.x = cur_x;
                ki.y = cur_y;
                ki.row = row;
                ki.col = col;
                ki.scancode = scancodes[row][col];
                key_map.at(row * NUM_COLS + col) = ki;

                col += 1;
                cur_x += ki.width;
            }
        }

        if (row == 1) {
            cur_x += pixel_width(unit_width_t::U0_5);
        }

        cur_x += pixel_width(unit_width_t::U1);
        col = 14;
        int fcol = 50; /* only used for coord to reference colours */

        {
            std::string line = utf8_to_cp866(num_text[row]);
            std::stringstream ss(line);
            std::string word;
            while (ss >> word) {
                key_info_t ki{};
                make_key_info(ki, fcol, row, word, "");
                ki.x = cur_x;
                ki.y = cur_y;
                ki.row = row;
                ki.col = col; /* linear col, not fcol */
                ki.scancode = scancodes_num[row][fcol - 50];
                key_map.at(row * NUM_COLS + col) = ki;

                col += 1;
                fcol += 1;
                cur_x += ki.width;
            }
        }

        if (cur_x > kb_width) kb_width = cur_x;
        cur_x = 0;
        cur_y += unit_h;
    }

    kb_height = cur_y + BOTTOM_BORDER;
    paint_seq.fetch_add(1, std::memory_order_relaxed);
}

/* --- rasterization ----------------------------------------------- */

bool VirtualKeyboard::needs_repaint() const
{
    if (ruslat_src != nullptr && last_ruslat != *ruslat_src) {
        last_ruslat = *ruslat_src;
        return true;
    }
    return paint_seq.load(std::memory_order_relaxed) != painted_seq;
}

void VirtualKeyboard::fill_rect(int x, int y, int w, int h, uint8_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VKBD_TEX_W) w = VKBD_TEX_W - x;
    if (y + h > VKBD_TEX_H) h = VKBD_TEX_H - y;

    for (int yy = y; yy < y + h; ++yy) {
        uint8_t * dst = tex + (size_t)yy * VKBD_TEX_W + x;
        memset(dst, color, (size_t)w);
    }
}

void VirtualKeyboard::fill_ellipse(int cx, int cy, int rx, int ry, uint8_t color)
{
    for (int dy = -ry; dy <= ry; ++dy) {
        for (int dx = -rx; dx <= rx; ++dx) {
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry) {
                int x = cx + dx, y = cy + dy;
                if ((unsigned)x < VKBD_TEX_W && (unsigned)y < VKBD_TEX_H)
                    tex[y * VKBD_TEX_W + x] = color;
            }
        }
    }
}

/* Key border: top/bottom/left/right edges, like the original
 * key_rect(). */
void VirtualKeyboard::key_rect(int x, int y, int w, int h, uint8_t color)
{
    fill_rect(x + 1, y, w - 2, 1, color);
    fill_rect(x + 1, y + h - 1, w - 2, 1, color);
    fill_rect(x, y + 1, 1, h - 2, color);
    fill_rect(x + w - 1, y + 1, 1, h - 2, color);
}

void VirtualKeyboard::print_text(int x, int y, const std::string & text,
                                 uint8_t color, int char_w)
{
    int cx = x;
    for (size_t i = 0; i < text.size(); ++i) {
        const uint8_t * glyph = vkbd_font[(uint8_t)text[i]];
        for (int gy = 0; gy < VKBD_FONT_H; ++gy) {
            int py = y + gy;
            if ((unsigned)py >= VKBD_TEX_H) break;
            uint8_t rowbits = glyph[gy];
            for (int gx = 0; gx < char_w; ++gx) {
                /* Narrow mode: sample every second source column */
                int src_col = (char_w < VKBD_FONT_W) ? gx * 2 : gx;
                if (rowbits & (0x80u >> src_col)) {
                    int px = cx + gx;
                    if ((unsigned)px < VKBD_TEX_W)
                        tex[py * VKBD_TEX_W + px] = color;
                }
            }
        }
        cx += char_w;
    }
}

void VirtualKeyboard::draw_key(const key_info_t & ki)
{
    const int border = 1;
    const int xmargin = 2;
    const int ymargin = 1;

    if (ki.scancode == 0) return;

    int w = ki.width - xgap;
    int h = unit_h - ygap;
    uint8_t bg_color = ki.color;
    if (ki.pressed && ki.color >= C_KEY_ALPHA && ki.color <= C_KEY_FN) {
        /* lightened variant */
        bg_color = (uint8_t)(C_PRESSED_BASE + (ki.color - C_KEY_ALPHA));
    }
    fill_rect(ki.x + border, ki.y + border,
              w - 2 * border, h - 2 * border, bg_color);

    uint8_t border_color = C_KEY_BORDER;
    if (ki.row == select_row && ki.col == select_col) {
        border_color = C_KEY_BORDER_SELECT;
    }

    int text1_x = ki.x + border + xmargin;
    int text1_y = ki.y + border + ymargin;

    int text2_x = ki.x + ki.width - xgap - xmargin
        - VKBD_FONT_W * (int)ki.legend_2.size();
    int text2_y = ki.y + unit_h - ygap - border - ymargin - VKBD_FONT_H;

    /* Long legends (ВВОД, БЛК, СБР, АР2, СТР) do not fit the narrow
     * numpad keys at full 8-pixel width; render them half-width. */
    int char_w = VKBD_FONT_W;
    const int avail_w = w - 2 * border - 2 * xmargin;
    if ((int)ki.legend_1.size() * char_w > avail_w) {
        char_w = VKBD_FONT_W / 2;
    }

    if ((ki.coord % 100) >= 50 || ki.legend_2 == "___") {
        text1_x = ki.x + (ki.width - (int)ki.legend_1.size() * char_w) / 2;
        text1_y = ki.y + (unit_h - ygap - VKBD_FONT_H) / 2;
    }
    else if (ki.legend_2 == "_") {
        /* backspace/underscore key: center horizontally only */
        text1_x = ki.x + (ki.width - (int)ki.legend_1.size() * VKBD_FONT_W) / 2;
        text2_x = ki.x + (ki.width - (int)ki.legend_2.size() * VKBD_FONT_W) / 2;
    }

    if (ki.legend_1 != "___") {
        print_text(text1_x, text1_y, ki.legend_1, ki.text_color, char_w);
    }
    if (ki.legend_2 != "___") {
        print_text(text2_x, text2_y, ki.legend_2, ki.text_color);
    }

    key_rect(ki.x, ki.y, w, h, border_color);
}

void VirtualKeyboard::draw_ruslat()
{
    const int x = unit_w / 3 + 1;
    const int y = unit_h * 4 + unit_h - LED_RADIUS - 3 - ygap + TOP_BORDER;

    const bool rus = (ruslat_src != nullptr) ? *ruslat_src : false;
    fill_ellipse(x, y, LED_RADIUS * 3 / 2, LED_RADIUS,
                 rus ? C_LED_ON : C_LED_OFF);
}

void VirtualKeyboard::paint()
{
    /* Snapshot the sequence first: a visual change arriving from the
     * worker thread while we rasterize must force one more pass. */
    const unsigned seq = paint_seq.load(std::memory_order_relaxed);

    fill_rect(0, 0, kb_width, kb_height, C_BACKGROUND);
    for (const key_info_t & ki : key_map) {
        draw_key(ki);
    }
    draw_ruslat();

    painted_seq = seq;
    tex_upload = true;
}

/* --- layout data --------------------------------------------------
 * The same legends, key classes and scancodes as the original
 * libretro vkbd.h; "___" means "no legend in this corner". */

const char * const VirtualKeyboard::top_text[] = {
    "; 1 2 3 4 5 6 7 8 9 0 - /",
    "Й Ц У К Е Н Г Ш Щ З Х :",
    "УС Ф Ы В А П Р О Л Д Ж Э .",
    "СС Я Ч С М И Т Ь Б Ю , ВК",
    "РУС ТАБ ___ ПС ЗБ"};

const char * const VirtualKeyboard::bottom_text[] = {
    "+ ! \" # ¤ % & ' ( ) ___ = ?",
    "J C U K E N G [ ] Z H *",
    "___ F Y W A P R O L D V \\ >",
    "___ Q ^ S M I T X B @ < ___",
    "LAT ___ ___ ___ _"};

const char * const VirtualKeyboard::num_text[] = {
    "ВВОД БЛК СБР",
    "F1 F2 F3",
    "F4 F5 АР2",
    "↖ ↑ СТР",
    "← ↓ →"};

const int VirtualKeyboard::longKeys[] =
    {300, 311, 400, 401, 403, 404}; /* also brown */
const int VirtualKeyboard::greenishKeys[] =
    {402, 50, 51, 52, 252, 200};
const int VirtualKeyboard::mustardKeys[] =
    {150, 151, 152, 250, 251, 352};
const int VirtualKeyboard::spaceKey = 402;

const int VirtualKeyboard::scancodes[5][14] = {
    {SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_1, SDL_SCANCODE_2,
        SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
        SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
        SDL_SCANCODE_EQUALS, SDL_SCANCODE_SLASH},

    {SDL_SCANCODE_J, SDL_SCANCODE_C, SDL_SCANCODE_U, SDL_SCANCODE_K,
        SDL_SCANCODE_E, SDL_SCANCODE_N, SDL_SCANCODE_G, SDL_SCANCODE_LEFTBRACKET,
        SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_Z, SDL_SCANCODE_H,
        SDL_SCANCODE_APOSTROPHE},

    {SDL_SCANCODE_LCTRL, SDL_SCANCODE_F, SDL_SCANCODE_Y, SDL_SCANCODE_W,
        SDL_SCANCODE_A, SDL_SCANCODE_P, SDL_SCANCODE_R, SDL_SCANCODE_O,
        SDL_SCANCODE_L, SDL_SCANCODE_D, SDL_SCANCODE_V, SDL_SCANCODE_BACKSLASH,
        SDL_SCANCODE_PERIOD},

    {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_Q, SDL_SCANCODE_GRAVE, SDL_SCANCODE_S,
        SDL_SCANCODE_M, SDL_SCANCODE_I, SDL_SCANCODE_T, SDL_SCANCODE_X,
        SDL_SCANCODE_B, SDL_SCANCODE_MINUS, SDL_SCANCODE_COMMA,
        SDL_SCANCODE_RETURN},

    {SDL_SCANCODE_F6, SDL_SCANCODE_TAB,
        SDL_SCANCODE_SPACE,
        SDL_SCANCODE_LALT, SDL_SCANCODE_BACKSPACE}};

const int VirtualKeyboard::scancodes_num[5][3] = {
    {SDL_SCANCODE_F11, -1, SDL_SCANCODE_F12},
    {SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3},
    {SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_ESCAPE},
    {SDL_SCANCODE_HOME, SDL_SCANCODE_UP, SDL_SCANCODE_END},
    {SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN, SDL_SCANCODE_RIGHT}};

void VirtualKeyboard::draw()
{
    const float w = (float)get_width();
    const float h = (float)get_height();
    const float x = ((float)LAYER_SCREEN_W - w) / 2.0f;
    const float y = is_top() ? 0.0f : (float)LAYER_SCREEN_H - h;

    layer_draw_quad(
        tex_data(), VKBD_TEX_W, VKBD_TEX_H,
        clut_data(),
        x, y, w, h,
        0.0f, 0.0f, w, h,
        false,
        consume_tex_upload());
}
