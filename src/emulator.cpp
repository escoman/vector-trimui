#include "emulator.h"

#include <cstdio>

#ifdef VECTOR06_GUI
#include "mainmenu.h"
#include "about.h"
#include "vkbd.h"
#include "statewindow.h"
#include "rombrowser.h"
#include "mapwindow.h"
#include "gamecenter.h"
#include "message.h"
#include "statefile.h"
#include "keymap.h"
#include "filelist.h"
#include "imgload.h"
#include "util.h"
#include "options.h"

#include <cctype>
#include <ctime>
#endif

/* Translate a gamepad button into the equivalent Vector-06C keyboard
 * scancode. The mapping is intentionally simple and can be adjusted after
 * testing on real hardware (see port spec, section 11). The Vector-06C
 * keyboard matrix itself is not modified. */
static SDL_Scancode controller_button_scancode(SDL_GameControllerButton button)
{
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    return SDL_SCANCODE_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return SDL_SCANCODE_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return SDL_SCANCODE_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return SDL_SCANCODE_RIGHT;
        case SDL_CONTROLLER_BUTTON_A:          return SDL_SCANCODE_RETURN; /* ВК */
        case SDL_CONTROLLER_BUTTON_B:          return SDL_SCANCODE_SPACE;
        case SDL_CONTROLLER_BUTTON_X:          return SDL_SCANCODE_ESCAPE; /* АП2 */
        case SDL_CONTROLLER_BUTTON_Y:          return SDL_SCANCODE_TAB;
        case SDL_CONTROLLER_BUTTON_BACK:       return SDL_SCANCODE_F6;     /* RUS/LAT */
        default:                               return SDL_SCANCODE_UNKNOWN;
    }
}

/* Build a synthetic keyboard event so gamepad input reuses the existing
 * KEYDOWN/KEYUP path (board.handle_keydown/keyup). */
static SDL_KeyboardEvent make_key_event(SDL_Scancode scancode)
{
    SDL_KeyboardEvent e;
    SDL_memset(&e, 0, sizeof(e));
    e.keysym.scancode = scancode;
    e.keysym.sym = SDL_GetKeyFromScancode(scancode);
    return e;
}

static void kick_timer()
{
    extern uint32_t timer_callback(uint32_t interval, void * param);
    timer_callback(0, 0);
    DBG_QUEUE(putchar('K'); fflush(stdout););
}

#ifdef VECTOR06_GUI
/* One bit per SDL controller button, so the held/pressed/released edge
 * math in gui_tick() works on a single word. */
static inline unsigned btn_bit(SDL_GameControllerButton b)
{
    return 1u << (unsigned)b;
}

/* The Brick Pro face buttons are labelled Nintendo-style (X on top, Y on
 * the left, A on the right, B on the bottom) while SDL reports them by
 * position (Y top, X left, B right, A bottom). Both pairs therefore arrive
 * swapped: the button printed "A" comes in as BUTTON_B, the one printed
 * "X" as BUTTON_Y. Normalize them once, here at the input edge, so the
 * rest of the code (the GUI windows and the KeyMap sources with their
 * labels) speaks in terms of the physical labels: A confirms/selects,
 * B returns back, X and Y are the two game buttons. */
static inline SDL_GameControllerButton physical_button(SDL_GameControllerButton b)
{
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return SDL_CONTROLLER_BUTTON_B;
        case SDL_CONTROLLER_BUTTON_B: return SDL_CONTROLLER_BUTTON_A;
        case SDL_CONTROLLER_BUTTON_X: return SDL_CONTROLLER_BUTTON_Y;
        case SDL_CONTROLLER_BUTTON_Y: return SDL_CONTROLLER_BUTTON_X;
        default:                      return b;
    }
}

/* Normalize the held controller buttons into the VKBD pad bitmask
 * (D-pad navigates, A presses the selected key). */
static unsigned vkbd_padmask(unsigned buttons)
{
    unsigned pad = 0;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  pad |= VKBD_PAD_LEFT;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) pad |= VKBD_PAD_RIGHT;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_UP))    pad |= VKBD_PAD_UP;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  pad |= VKBD_PAD_DOWN;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_A))          pad |= VKBD_PAD_PRESS;
    return pad;
}

/* Normalize the held controller buttons into the Main Menu pad bitmask
 * (D-pad up/down navigates, A activates the item). */
static unsigned menu_padmask(unsigned buttons)
{
    unsigned pad = 0;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_UP))   pad |= MENU_PAD_UP;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) pad |= MENU_PAD_DOWN;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_A))         pad |= MENU_PAD_PRESS;
    return pad;
}
/* Normalize the held controller buttons into the State Browser pad
 * bitmask (the D-pad moves the slot cursor around the grid). */
static unsigned sb_padmask(unsigned buttons)
{
    unsigned pad = 0;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_UP))    pad |= SB_PAD_UP;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  pad |= SB_PAD_DOWN;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  pad |= SB_PAD_LEFT;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) pad |= SB_PAD_RIGHT;
    return pad;
}

/* Normalize the held controller buttons into the ROM Browser pad
 * bitmask (UP/DOWN scroll the list). */
static unsigned rb_padmask(unsigned buttons)
{
    unsigned pad = 0;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_UP))   pad |= RB_PAD_UP;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) pad |= RB_PAD_DOWN;
    return pad;
}

/* Normalize the held controller buttons into the Map Keys pad
 * bitmask (UP/DOWN move the source selection). */
static unsigned mk_padmask(unsigned buttons)
{
    unsigned pad = 0;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_UP))   pad |= MK_PAD_UP;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) pad |= MK_PAD_DOWN;
    return pad;
}

/* Normalize the held controller buttons into the Game Center pad
 * bitmask. The MessageDialog shares these bit values, so the same mask
 * drives the confirmation dialog on top of the window. */
static unsigned gc_padmask(unsigned buttons)
{
    unsigned pad = 0;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_UP))    pad |= GC_PAD_UP;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  pad |= GC_PAD_DOWN;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  pad |= GC_PAD_LEFT;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) pad |= GC_PAD_RIGHT;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_A))          pad |= GC_PAD_PRESS;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_B))          pad |= GC_PAD_BACK;
    return pad;
}

/* The current controller state expressed as KeyMap source bits: the
 * D-pad, the four face buttons and the two shoulder buttons. START
 * and BACK (SELECT) are system buttons and never mapping sources. */
static unsigned trimui_source_mask(unsigned buttons)
{
    unsigned m = 0;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_UP))         m |= 1u << MAP_SRC_UP;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN))       m |= 1u << MAP_SRC_DOWN;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_LEFT))       m |= 1u << MAP_SRC_LEFT;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))      m |= 1u << MAP_SRC_RIGHT;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_A))               m |= 1u << MAP_SRC_A;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_B))               m |= 1u << MAP_SRC_B;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_X))               m |= 1u << MAP_SRC_X;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_Y))               m |= 1u << MAP_SRC_Y;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))    m |= 1u << MAP_SRC_L1;
    if (buttons & btn_bit(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))   m |= 1u << MAP_SRC_R1;
    return m;
}

/* Drop the disabled sources: keep only what the effective mapping
 * table actually turns into a Vector key. */
static unsigned mapped_source_mask(unsigned src_mask)
{
    unsigned m = 0;
    for (int src = 0; src < MAP_SRC_COUNT; ++src) {
        if ((src_mask & (1u << src)) && KeyMap::effective_key(src) >= 0)
            m |= 1u << src;
    }
    return m;
}
#endif

Emulator::Emulator(Board & borat) : board(borat)
{
    board.onframetimer = [=]() {
        ui_to_engine_queue.push(threadevent(EXECUTE_FRAME, 0));
    };
}

bool Emulator::handle_keyboard_event(SDL_KeyboardEvent & event)
{
    switch (event.keysym.scancode) 
    {
        case SDL_SCANCODE_RETURN:
#if __WIN32__
            if (event.keysym.mod & KMOD_ALT) {	
#else 
                if (event.keysym.mod & KMOD_GUI) {
#endif
                    board.toggle_fullscreen();
                    return true;
                }
            break;
        default:
            break;
    }
    return false;
}

/* This part is copied from SDL_events.c SDL_WaitEventTimeout().
 * It is not acceptable to wait 10ms as written in the original code.
 * Because this loop is also used to receive render requests from the engine,
 * 10ms may create frame dropouts and it looks awful.
 *
 * The place of SDL_Delay(10) is now taken by sync_priority_queue<>::pull_for() 
 * with a timeout. I'm not sure why boost only has timed-out pull for priority
 * queues and not for regular ones.
 *
 * When the engine is done executing a frame, it posts a threadevent(RENDER)
 * to engine_to_ui_queue and it's supposed to instantly wake up the main
 * thread. 
 */
int Emulator::wait_event(SDL_Event * event, threadevent & ev, int timeout)
{
    Uint32 expiration = 0;

    if (timeout > 0)
        expiration = SDL_GetTicks() + timeout;

    for (;;) {
        SDL_PumpEvents();
        switch (SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, 
                    SDL_LASTEVENT)) 
        {
        case -1:
            return 0;
        case 0:
            if (timeout == 0) {
                /* Polling and no events, just return */
                return 0;
            }
            if (timeout > 0 && SDL_TICKS_PASSED(SDL_GetTicks(), expiration)) {
                /* Timeout expired and no events */
                return 0;
            }
            //SDL_Delay(10); pizdec
            {
                auto timeout = boost::chrono::milliseconds(10);
                static auto constexpr ok = boost::queue_op_status::success;
                if (engine_to_ui_queue.pull_for(timeout, ev) == ok) {
                    /* render request: fill in a dummy SDL event */
                    event->type = SDL_USEREVENT;
                    event->user.code = 0x80 | ev.data;

                    /* purge extra requests if any, they can accumulate if
                     * window is being dragged by its titlebar on windows */
                    int purge = 0;
                    while(engine_to_ui_queue.nonblocking_pull(ev) == ok) 
                        ++purge;
                    purge && Options.log.video && 
                        fprintf(stderr, "purged render(%d)\n", purge);
                    return 2;
                }
            }
            break;
        default:
            /* Has events */
            return 1;
        }
    }
}

void Emulator::handle_render(threadevent & event, bool & stopping)
{
    if (event.data) {
        int frame_no = event.frame_no;
        bool executed = event.data;
        DBG_QUEUE(putchar('r'); putchar('0' + executed););
#ifdef VECTOR06_GUI
        /* Run the UI-state machine and feed the active window before the
         * frame is drawn, so TV::render presents the updated overlay. */
        gui_tick();
#endif
        board.render_frame(frame_no, executed);
        if ((Options.nosound && Options.novideo) || 
                (Options.vsync && Options.vsync_enable)) {
            /* tests: kick-spin the event loop */
            kick_timer();
        }
        DBG_QUEUE(putchar('R'); fflush(stdout););
        if (Options.max_frame >= 0 && frame_no >= Options.max_frame) {
            ui_to_engine_queue.push(threadevent(QUIT, 0));
            stopping = true;
        }
    }
}

/* handle sdl events in the main thread */
void Emulator::run_event_loop()
{
    SDL_Event event;
    threadevent threadev;
    bool end = false;
    bool kickstart = true;
    /* Open the first available game controller, if any. On TrimUI/CrossMix
     * the built-in gamepad is exposed through the SDL GameController API. */
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (SDL_NumJoysticks() > 0) {
        SDL_GameControllerOpen(0);
    }
    /* Prime the engine once. On framebuffer/KMSDRM targets the first
     * SDL_WINDOWEVENT_EXPOSED may be delivered seconds late, and the engine
     * thread blocks in wait_pull() until the first EXECUTE_FRAME is posted.
     * Without this kick the screen stays black until that expose event
     * finally arrives (observed as a ~20s startup delay on TrimUI). */
    kick_timer();
    /* tests: kick-spin the event loop */
    if (Options.nosound && Options.novideo) {
        kick_timer();
    }
    while(!end) {
        if (this->wait_event(&event, threadev, -1)) {
            switch(event.type) {
                case SDL_USEREVENT:
                    handle_render(threadev, end);
                    break;
                case SDL_KEYDOWN:
                    if (!this->handle_keyboard_event(event.key)) {
                        ui_to_engine_queue.push(threadevent(KEYDOWN, event.key));
                    }
                    break;
                case SDL_KEYUP:
                    ui_to_engine_queue.push(threadevent(KEYUP, event.key));
                    break;
                // this is to be handled in the ui thread
                case SDL_WINDOWEVENT:
                    //printf("windowevent: %x\n", event.window.event);
                    if (event.window.event == SDL_WINDOWEVENT_EXPOSED) {
                        if (kickstart) {
                            kick_timer();
                            kickstart = false;
                        }
                    } 
                    board.handle_window_event(event);
                    break;
                case SDL_QUIT:
                    ui_to_engine_queue.push(threadevent(QUIT, 0));
                    end = true;
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP:
                    {
                        bool down = (event.type == SDL_CONTROLLERBUTTONDOWN);
                        SDL_GameControllerButton b =
                            (SDL_GameControllerButton)event.cbutton.button;
#ifdef VECTOR06_GUI
                        /* The GUI owns all controller input: just track the
                         * held bitmask; gui_tick() (50 Hz) derives the edges
                         * and decides between menu, VKBD and game keys. */
                        gui_handle_button(b, down);
#else
                        /* START exits back to the CrossMix launcher */
                        if (b == SDL_CONTROLLER_BUTTON_START && down) {
                            ui_to_engine_queue.push(threadevent(QUIT, 0));
                            end = true;
                            break;
                        }
                        SDL_Scancode sc = controller_button_scancode(b);
                        if (sc != SDL_SCANCODE_UNKNOWN) {
                            SDL_KeyboardEvent ke = make_key_event(sc);
                            ui_to_engine_queue.push(
                                    threadevent(down ? KEYDOWN : KEYUP, ke));
                        }
#endif
                    }
                    break;
                default:
                    break;
            }
        }
#ifdef VECTOR06_GUI
        if (gui_quit) {
            end = true;
        }
#endif
    }
    /* Stop the audio callback before joining: it posts EXECUTE_FRAME into
     * ui_to_engine_queue, and that queue (with its mutexes) is destroyed
     * right after main() returns. A callback firing during/after teardown
     * would lock a destroyed mutex and abort with boost::lock_error. */
    board.pause_sound(1);
    join_emulator_thread();
}


void Emulator::handle_threadevent(threadevent & event)
{
    //printf("handle_event: event.type=%d\n", event.type);
    switch(event.type) {
        case KEYDOWN:
            board.handle_keydown(event.key);
            break;
        case KEYUP:
            board.handle_keyup(event.key);
            break;
        case EXECUTE_FRAME:
            {
                int executed;
                if (Options.vsync && Options.vsync_enable) {
                    DBG_QUEUE(putchar('E'); fflush(stdout););
                    executed = board.execute_frame_with_cadence(true, true);
                } 
                else {
                    DBG_QUEUE(putchar('e'); fflush(stdout););
                    executed = board.execute_frame_with_cadence(true, false);
                }
                engine_to_ui_queue.push(threadevent(RENDER, executed, 
                            board.get_frame_no()));
            }
            break;
        case QUIT:
            board.handle_quit();
            break;
        default:
            break;
    }
}


/* emulator thread body */
void Emulator::threadfunc()
{
    for(int i = 0; !this->board.terminating(); ++i) {
        threadevent ev;
        if (ui_to_engine_queue.wait_pull(ev) == boost::queue_op_status::closed)
            break;
        handle_threadevent(ev);
        if (i == 0) {
            board.pause_sound(0);
        }
    }
}

void Emulator::start_emulator_thread()
{
    thread = boost::thread(&Emulator::threadfunc, this);
}

void Emulator::join_emulator_thread()
{
    thread.join();
}

void Emulator::save_state(vector<uint8_t> & to)
{
    this->board.serialize(to);
}

/* The state file carries only memory, CPU and the visible IO part;
 * the sound chips (8253, AY) and the Soundnik mirrors/event queue
 * stay out of it. Reset the whole machine through the same path a
 * ROM load uses before applying the snapshot, so no pre-load
 * register contents survive — without this, a note playing at the
 * load moment drones on forever after the restore. */
static bool state_chunks_valid(std::vector<uint8_t> & data)
{
    auto it = data.begin();
    while (it != data.end()) {
        if ((size_t)std::distance(it, data.end()) < 8)
            return false;
        SerializeChunk::id signature;
        uint32_t size;
        auto begin = SerializeChunk::take_chunk(it, signature, size);
        switch (signature) {
            case SerializeChunk::MEMORY:
            case SerializeChunk::IO:
            case SerializeChunk::CPU:
            case SerializeChunk::BOARD:
                break;
            default:
                return false;
        }
        if ((size_t)std::distance(begin, data.end()) < size)
            return false;
        it = begin + size;
    }
    return true;
}

bool Emulator::restore_state(vector<uint8_t> & from)
{
    /* A refused state leaves the running machine untouched, so
     * validate the stream before the destructive reset. */
    if (!state_chunks_valid(from))
        return false;
    this->board.reset(Board::ResetMode::LOADROM);
    return this->board.deserialize(from);
}

#ifdef VECTOR06_GUI
/* Load address comes from the file name, same rule as the desktop
 * port: NAME0.ROM loads at 0x0000, NAME1.ROM at 0x0100, ... NAME9.ROM
 * at 0x0900; anything else (plain .ROM/.BIN) defaults to 0x0100. */
static uint16_t get_rom_org(const std::string & path)
{
    if (path.size() < 2)
        return 0x0100;

    char c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(path[path.size() - 2]))
    );

    if (c == 'o')
        return 0x0100;

    if (c >= '0' && c <= '9')
        return static_cast<uint16_t>((c - '0') * 0x0100);

    return 0x0100;
}

/* ROM base name: the file name without its extension, exactly as it
 * sits in the ROM directory (no path part, case untouched). */
static std::string derive_rom_base(const std::string & path)
{
    const size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos)
        ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        base = base.substr(0, dot);
    return base;
}

/* "<dir>/<base>.key": next to the ROM file, extension dropped - the
 * same derivation rule as the preview (save_preview_action). */
static std::string map_path_for_rom(const std::string & rom_path)
{
    const size_t slash = rom_path.find_last_of('/');
    const std::string dir = (slash == std::string::npos)
        ? std::string(".") : rom_path.substr(0, slash);
    return dir + "/" + derive_rom_base(rom_path) + ".key";
}

/* ROM isolation: every ROM load starts from the Default Mapping and
 * then applies the ROM's own .key on top; a ROM without a file
 * simply runs the defaults. Malformed files never fail the load,
 * unknown lines are ignored. */
static void apply_rom_mapping(const std::string & rom_path)
{
    KeyMap::reset_to_default();
    if (rom_path.empty())
        return;
    const std::string path = map_path_for_rom(rom_path);
    if (KeyMap::load_file(path))
        printf("UI: key mapping applied from %s\n", path.c_str());
}

bool Emulator::load_rom(const std::string & path)
{
    /* Read the file first: the old ROM's working state must survive
     * until the new one passes this basic check. */
    std::vector<uint8_t> data = util::load_binfile(path);
    if (data.empty()) {
        printf("Failed to load ROM: %s\n", path.c_str());
        return false;
    }

    const uint16_t org = get_rom_org(path);

    /* Install the ROM, then reset the board into the new program.
     * Paging goes back to the power-on state FIRST: init_from_vector()
     * places the bytes through the current mapping, and the desktop
     * port loads its single ROM with the power-on mapping too. */
    this->board.get_memory().reset_paging();
    this->board.get_memory().init_from_vector(data, org);
    Options.pc = org;
    this->board.reset(Board::ResetMode::LOADROM);

    /* The save-state directory is tied to the ROM name; the full file
     * path backs the Save Preview menu item and the ROM's own .key
     * mapping. */
    const std::string base = derive_rom_base(path);
    if (!base.empty())
        this->rom_base = base;
    this->rom_path = path;
    apply_rom_mapping(path);

    printf("ROM loaded: %s size=%lu org=%04X pc=%04X\n",
           path.c_str(), (unsigned long)data.size(),
           org, Options.pc);
    return true;
}

void Emulator::set_rom_path(const std::string & path)
{
    this->rom_path = path;
    const std::string base = derive_rom_base(path);
    if (!base.empty())
        this->rom_base = base;
    apply_rom_mapping(path);
}

/* SAVE flow: serialize the paused machine into the selected slot
 * (stateN.bin, safe tmp+rename overwrite) and write the screenshot of
 * the frame currently on screen next to it (stateN.png). Runs on the
 * UI thread while the machine is paused, so the Board cannot change
 * mid-serialize. The window stays open; the slot is refreshed in
 * place. */
void Emulator::state_save_action()
{
    const int fw = Options.screen_width;
    const int fh = Options.screen_height;
    /* The Vector frame as 0xAABBGGRR pixels: the pure machine picture,
     * no UI layer ever reaches these buffers. Heap-allocated: 648 KB
     * is too much for a comfortable stack frame. */
    uint32_t * shot = new uint32_t[(size_t)fw * fh];

    const std::string dir = StateFile::rom_dir(this->rom_base);
    const int slot = gui_sb->selected_slot();

    if (!StateFile::ensure_dir(dir)) {
        gui_sb->set_error("Cannot create saves dir");
        printf("UI: save failed, cannot create %s\n", dir.c_str());
        delete[] shot;
        return;
    }

    /* The file IO below is visibly slow; show the in-progress status
     * while it runs. Every exit path replaces the message. */
    gui_sb->set_status("Saving...");

    std::vector<uint8_t> payload;
    this->save_state(payload);

    const uint64_t now = (uint64_t)time(nullptr);
    if (!StateFile::save(dir, slot, payload, now)) {
        gui_sb->set_error("Save failed");
        delete[] shot;
        return;
    }

    board.get_tv().copy_latest_rgb(shot);
    if (!img_save(StateFile::shot_path(dir, slot).c_str(), shot, fw, fh))
        printf("UI: screenshot write failed (slot %d)\n", slot);

    /* The state itself is already saved; a missing screenshot is not
     * fatal (the slot simply shows no picture next time). */
    gui_sb->after_save(slot, now, shot, fw, fh);
    printf("UI: state saved into slot %d\n", slot);
    delete[] shot;
}

/* LOAD flow: restore the selected slot into the paused machine.
 * Every refusal (empty slot, missing/corrupt file, unknown version,
 * Board rejecting the payload) leaves the machine untouched and the
 * window open with a footer message. True on success; the caller then
 * closes everything and resumes. */
bool Emulator::state_load_action()
{
    if (!gui_sb->is_selected_occupied()) {
        gui_sb->set_error("Empty slot");
        return false;
    }

    const std::string dir = StateFile::rom_dir(this->rom_base);
    std::vector<uint8_t> payload;
    uint64_t ts = 0;
    if (!StateFile::load(dir, gui_sb->selected_slot(), payload, ts)) {
        gui_sb->set_error("Invalid state");
        return false;
    }

    if (!this->restore_state(payload)) {
        gui_sb->set_error("Invalid state");
        printf("UI: restore refused (slot %d)\n", gui_sb->selected_slot());
        return false;
    }
    return true;
}

/* SAVE PREVIEW flow: write the frame currently on screen next to the
 * loaded ROM file ("<rom base>.png", the exact name the ROM Browser
 * preview lookup uses). The boot loader has no ROM file behind it
 * (rom_path empty), so for it the item is a no-op. */
void Emulator::save_preview_action()
{
    if (this->rom_path.empty()) {
        printf("UI: Save Preview skipped, no ROM loaded\n");
        return;
    }

    /* "<dir>/<base>.png": the directory and the base name of the ROM
     * file, extension dropped. */
    const size_t slash = this->rom_path.find_last_of('/');
    const std::string dir = (slash == std::string::npos)
        ? std::string(".") : this->rom_path.substr(0, slash);
    const std::string preview = dir + "/" + derive_rom_base(this->rom_path) + ".png";

    const int fw = Options.screen_width;
    const int fh = Options.screen_height;
    uint32_t * shot = new uint32_t[(size_t)fw * fh];
    board.get_tv().copy_latest_rgb(shot);

    /* The PNG write is visibly slow; show the in-progress status in
     * the menu title while it runs. */
    gui_menu->set_status("Saving...");

    if (img_save(preview.c_str(), shot, fw, fh))
        printf("UI: preview saved: %s\n", preview.c_str());
    else
        printf("UI: preview write failed: %s\n", preview.c_str());

    gui_menu->set_status("");
    delete[] shot;
}

void Emulator::set_gui(MainMenu * menu, AboutWindow * about, VirtualKeyboard * vkbd,
                       StateWindow * sb, RomBrowser * browser, MapWindow * mapk,
                       GameCenter * gc, MessageDialog * msg)
{
    gui_menu = menu;
    gui_about = about;
    gui_vkbd = vkbd;
    gui_sb = sb;
    gui_browser = browser;
    gui_mapk = mapk;
    gui_gc = gc;
    gui_msg = msg;
    if (vkbd) {
        /* Virtual key presses travel the same KEYDOWN/KEYUP queue as the
         * physical gamepad, so the engine applies them on the next frame
         * - except while the Map Keys window waits for a key: then the
         * first VKBD press becomes the assignment instead of a machine
         * input. */
        vkbd->on_keydown = [this](int sc) {
            if (gui_mapk && gui_mapk->is_open() && gui_mapk->is_waiting()) {
                gui_mapk->assign_selected(sc);
                /* The picker key itself must not stay latched inside
                 * the VKBD (sticky keys); the emitted keyups are
                 * harmless no-ops for the machine, which never saw
                 * the matching keydowns. */
                gui_vkbd->release_all();
                return;
            }
            inject_key((SDL_Scancode)sc, true);
        };
        vkbd->on_keyup   = [this](int sc) { inject_key((SDL_Scancode)sc, false); };
    }
}

void Emulator::pause()
{
    board.set_paused(true);
    board.get_tv().set_frozen(true);
    gui_paused = true;
}

void Emulator::resume()
{
    board.set_paused(false);
    board.get_tv().set_frozen(false);
    gui_paused = false;
}

void Emulator::inject_key(SDL_Scancode sc, bool down)
{
    SDL_KeyboardEvent ke = make_key_event(sc);
    ui_to_engine_queue.push(threadevent(down ? KEYDOWN : KEYUP, ke));
}

void Emulator::gui_handle_button(SDL_GameControllerButton b, bool down)
{
    const SDL_GameControllerButton pb = physical_button(b);
    if (down) gui_held |= btn_bit(pb);
    else      gui_held &= ~btn_bit(pb);
}

/* Release every Vector key currently held through the effective key
 * mapping, so opening the menu or the VKBD never leaves a key stuck. */
void Emulator::release_held_game_keys(unsigned buttons)
{
    const unsigned fed = mapped_source_mask(trimui_source_mask(buttons));
    for (int src = 0; src < MAP_SRC_COUNT; ++src) {
        if (fed & (1u << src))
            inject_key((SDL_Scancode)KeyMap::effective_key(src), false);
    }
}

/* MAP_KEYS -> MAIN_MENU: save only the differences next to the ROM
 * (fully default -> the .key is deleted), return the focus to the
 * Map Keys item. The VKBD is only visible during the assignment
 * mode, so normally it is already hidden here; guard anyway. The
 * boot loader has no ROM file behind it (rom_path empty): its edits
 * stay session-local and never reach the disk. */
void Emulator::mapkey_close_action()
{
    if (!this->rom_path.empty())
        KeyMap::save_or_cleanup(map_path_for_rom(this->rom_path));
    if (gui_vkbd->is_visible())
        gui_vkbd->hide(vkbd_padmask(gui_held));
    gui_mapk->close();
    gui_menu->open(MainMenu::ITEM_MAP_KEYS);
}

/* UI-state machine, ported from the PSP handle_input() dispatcher
 * (without the Config window). Runs once per rendered frame (~50 Hz)
 * on the UI thread. Branch order: STATE_BROWSER, MAP_KEYS,
 * GAME_CENTER, ABOUT, ROM_BROWSER, MAIN_MENU, GAME - the popups are
 * mutually exclusive, so only the open one ever consumes the pad. */
void Emulator::gui_tick()
{
    if (!gui_menu) return;

    const unsigned buttons = gui_held;
    const unsigned pressed = buttons & ~gui_prev_held;
    /* START or B returns from a window / closes the menu. */
    const unsigned BACK =
        btn_bit(SDL_CONTROLLER_BUTTON_START) | btn_bit(SDL_CONTROLLER_BUTTON_B);

    /* STATE_BROWSER state: the D-pad moves the slot selection around
     * the grid (clamped at the edges, never wraps), A saves into /
     * restores the selected slot per the window mode, START/B return
     * to the Main Menu with the focus back on the item the window was
     * opened from. The machine stays paused the whole time. */
    if (gui_sb && gui_sb->is_open()) {
        gui_sb->update(sb_padmask(buttons));

        if (pressed & BACK) {
            const int focus = (gui_sb->mode() == StateWindow::MODE_SAVE)
                ? MainMenu::ITEM_SAVE_STATE : MainMenu::ITEM_LOAD_STATE;
            gui_sb->close();
            gui_menu->open(focus);
        } else if (pressed & btn_bit(SDL_CONTROLLER_BUTTON_A)) {
            if (gui_sb->mode() == StateWindow::MODE_SAVE) {
                state_save_action();
            } else if (state_load_action()) {
                /* Restored successfully: straight to GAME (resumed),
                 * never back through the Main Menu. */
                gui_sb->close();
                gui_menu->close();
                resume();
            }
        }
        gui_input_muted = true;
        gui_prev_held = buttons;
        return;
    }

    /* MAP_KEYS state: the machine stays paused the whole time.
     * Normal mode: UP/DOWN move the source selection, A starts the
     * assignment, X writes an explicit NONE, START/B save the .key
     * and return to the Main Menu. Assignment mode: the pad drives
     * the VKBD (the key picker); the sink interception in set_gui()
     * lands the picked key, START/B cancel. SELECT is a system
     * button and is ignored here. */
    if (gui_mapk && gui_mapk->is_open()) {
        if (gui_mapk->is_waiting()) {
            /* Assignment mode: the VKBD is visible exactly for its
             * lifetime and serves as the key picker. */
            gui_vkbd->update(vkbd_padmask(buttons));
            if (pressed & BACK) {
                gui_mapk->cancel_assign();
                gui_vkbd->hide(vkbd_padmask(buttons));
            } else if (!gui_mapk->is_waiting()) {
                /* The VKBD sink landed the picked key this very
                 * tick; the picker is no longer needed. */
                gui_vkbd->hide(vkbd_padmask(buttons));
            }
        } else {
            gui_mapk->update(mk_padmask(buttons));

            if (pressed & BACK) {
                mapkey_close_action();
            } else if (pressed & btn_bit(SDL_CONTROLLER_BUTTON_A)) {
                gui_mapk->start_assign();
                /* The VKBD appears only now, as the key picker; the
                 * rest of the time it stays hidden and never covers
                 * the source list. */
                gui_vkbd->show(vkbd_padmask(buttons));
            } else if (pressed & btn_bit(SDL_CONTROLLER_BUTTON_X)) {
                gui_mapk->disable_selected();
            }
        }
        gui_input_muted = true;
        gui_prev_held = buttons;
        return;
    }

    /* GAME_CENTER state: the online ROM catalog. UP/DOWN scroll the
     * list, A asks for a load (the modal MessageDialog confirms),
     * START/B return to the Main Menu. The machine stays paused; every
     * download runs in NetMan's background thread, so this tick never
     * blocks and the progress line keeps updating. */
    if (gui_gc && gui_msg && gui_gc->is_open()) {
        /* While the modal dialog is up it owns the pad. */
        if (gui_msg->is_active()) {
            gui_msg->update(gc_padmask(buttons));
            if (!gui_msg->is_active()
                    && gui_msg->result() == MessageDialog::RESULT_YES) {
                gui_gc->perform_load_request();
            }
            gui_input_muted = true;
            gui_prev_held = buttons;
            return;
        }

        gui_gc->update(gc_padmask(buttons));

        /* A on an entry: confirm before downloading a ROM and replacing
         * the running machine state with it. */
        if (gui_gc->consume_load_request())
            gui_msg->show("LOAD ROM?", MessageDialog::YES_NO);

        /* The ROM (plus its preview) has landed in the ROM directory:
         * load it and go straight to GAME, as the ROM Browser does. */
        if (gui_gc->has_rom_ready()) {
            const std::string path = gui_gc->get_rom_path();
            gui_gc->clear_rom_ready();
            if (load_rom(path)) {
                gui_gc->close();
                gui_menu->close();
                resume();
            } else {
                /* Keep the catalog open with the error in the footer. */
                gui_gc->set_status("Failed to load ROM");
            }
        }

        if (pressed & BACK) {
            gui_gc->close();
            gui_menu->open(MainMenu::ITEM_GAME_CENTER);
        }
        gui_input_muted = true;
        gui_prev_held = buttons;
        return;
    }

    /* ABOUT state: CIRCLE/START closes and returns to the Main Menu. */
    if (gui_about->is_open()) {
        gui_about->update(0);
        if (pressed & BACK) {
            gui_about->close();
            gui_menu->open(MainMenu::ITEM_ABOUT);
        }
        gui_input_muted = true;
        gui_prev_held = buttons;
        return;
    }

    /* ROM_BROWSER state: UP/DOWN navigate the list (cyclic,
     * scrolled), A loads the selected ROM through load_rom() and
     * goes straight to GAME, START/B go back to the Main Menu. The
     * machine stays paused the whole time; nothing reaches the
     * Vector, SELECT does not open the VKBD. */
    if (gui_browser && gui_browser->is_open()) {
        gui_browser->update(rb_padmask(buttons));

        if (pressed & BACK) {
            gui_browser->close();
            /* The menu selection resets to the first item (Load
             * ROM) on every open. */
            gui_menu->open();
        } else if ((pressed & btn_bit(SDL_CONTROLLER_BUTTON_A))
                && gui_browser->has_items()) {
            const std::string path = FileList::default_rom_dir()
                                   + "/" + gui_browser->selected_name();
            if (load_rom(path)) {
                /* ROM Browser -> GAME with the new ROM; the old
                 * board state was replaced by the LOADROM reset.
                 * The mapping starts from the defaults plus this
                 * ROM's own .key (applied inside load_rom). */
                gui_browser->close();
                gui_menu->close();
                resume();
            } else {
                /* Keep the browser open with the error in the
                 * footer; the old ROM state is untouched. */
                gui_browser->set_error("Failed to load ROM");
            }
        }
        gui_input_muted = true;
        gui_prev_held = buttons;
        return;
    }

    /* MAIN MENU state: D-pad navigates, START/B close and resume, A
     * activates the selected item. The machine stays paused. */
    if (gui_menu->is_open()) {
        gui_menu->update(menu_padmask(buttons));
        if (pressed & BACK) {
            gui_menu->close();
            resume();
        } else if (pressed & btn_bit(SDL_CONTROLLER_BUTTON_A)) {
            switch (gui_menu->selected_item()) {
                case MainMenu::ITEM_LOAD_ROM:
                    /* Main Menu -> ROM Browser: the browser replaces
                     * the menu (never stacked under it), the machine
                     * stays paused, the VKBD stays hidden. */
                    gui_menu->close();
                    gui_browser->open(FileList::default_rom_dir().c_str());
                    break;
                case MainMenu::ITEM_SAVE_PREVIEW:
                    /* Save the current screen as the ROM Browser preview
                     * of the running ROM; the menu stays open, the
                     * machine stays paused. No-op for the boot loader. */
                    save_preview_action();
                    break;
                case MainMenu::ITEM_SAVE_STATE:
                    gui_menu->close();
                    gui_sb->open(StateWindow::MODE_SAVE,
                                 StateFile::rom_dir(rom_base).c_str());
                    break;
                case MainMenu::ITEM_LOAD_STATE:
                    gui_menu->close();
                    gui_sb->open(StateWindow::MODE_LOAD,
                                 StateFile::rom_dir(rom_base).c_str());
                    break;
                case MainMenu::ITEM_MAP_KEYS:
                    /* Main Menu -> Map Keys: the machine stays
                     * paused; the VKBD stays hidden until the
                     * assignment mode starts. */
                    gui_menu->close();
                    {
                        std::string label = "BOOT LOADER";
                        if (!rom_path.empty()) {
                            const size_t slash = rom_path.find_last_of('/');
                            label = (slash == std::string::npos)
                                ? rom_path : rom_path.substr(slash + 1);
                        }
                        gui_mapk->open(label.c_str());
                    }
                    break;
                case MainMenu::ITEM_GAME_CENTER:
                    /* Main Menu -> Game Center: the window opens at once
                     * and downloads the catalog in the background; the
                     * machine stays paused, the VKBD stays hidden. */
                    gui_menu->close();
                    gui_gc->open();
                    break;
                case MainMenu::ITEM_ABOUT:
                    gui_menu->close();
                    gui_about->open();
                    break;
                case MainMenu::ITEM_EXIT:
                    ui_to_engine_queue.push(threadevent(QUIT, 0));
                    gui_quit = true;
                    break;
                default:
                    break;
            }
        }
        gui_input_muted = true;
        gui_prev_held = buttons;
        return;
    }

    /* GAME state. START opens the menu (pausing the machine); SELECT
     * toggles the on-screen keyboard. */
    if (pressed & btn_bit(SDL_CONTROLLER_BUTTON_START)) {
        if (gui_vkbd->is_visible()) {
            gui_vkbd->hide(vkbd_padmask(buttons));
        } else {
            release_held_game_keys(buttons);
        }
        gui_menu->open();
        pause();
        gui_prev_held = buttons;
        return;
    }

    if (pressed & btn_bit(SDL_CONTROLLER_BUTTON_BACK)) {
        if (gui_vkbd->is_visible()) {
            gui_vkbd->hide(vkbd_padmask(buttons));
        } else {
            release_held_game_keys(buttons);
            gui_vkbd->show(vkbd_padmask(buttons));
        }
    }

    if (gui_vkbd->is_visible()) {
        /* Nothing reaches the Vector while the VKBD is open: the D-pad
         * moves the finger, A presses the key, B flips top/bottom. */
        gui_vkbd->update(vkbd_padmask(buttons));
        if (pressed & btn_bit(SDL_CONTROLLER_BUTTON_B)) {
            gui_vkbd->move();
        }
        gui_input_muted = true;
    } else {
        /* Mapping-driven feed: every held source goes through the
         * effective table (defaults plus the ROM's .key); disabled
         * sources do nothing. Edges are detected per source, so a
         * changed mapping applies from the next tick with no ROM
         * reload, and several buttons may feed one Vector key.
         * START and BACK (SELECT) are system buttons (menu / VKBD),
         * never game keys. */
        const unsigned mapped_now =
            mapped_source_mask(trimui_source_mask(buttons));
        if (gui_input_muted) {
            /* First GAME tick after a window closed: whatever is still
             * held from the menu interaction is not a game keypress,
             * otherwise closing a window with A or B would fire their
             * Vector key. A re-press is required. */
            gui_old_mapped = mapped_now;
            gui_input_muted = false;
        }
        const unsigned down_edges = mapped_now & ~gui_old_mapped;
        const unsigned up_edges = gui_old_mapped & ~mapped_now;
        for (int src = 0; src < MAP_SRC_COUNT; ++src) {
            if (down_edges & (1u << src))
                inject_key((SDL_Scancode)KeyMap::effective_key(src), true);
            if (up_edges & (1u << src))
                inject_key((SDL_Scancode)KeyMap::effective_key(src), false);
        }
        gui_old_mapped = mapped_now;
    }

    gui_prev_held = buttons;
}
#endif

