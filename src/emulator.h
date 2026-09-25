#if !defined(__ANDROID__) && !defined(__GODOT__)
#include "SDL.h"

#include <boost/thread.hpp>
#include <boost/thread/concurrent_queues/sync_queue.hpp>
#include <boost/thread/concurrent_queues/sync_priority_queue.hpp>
#else
#include <pthread.h>
#endif

#include "board.h"

#ifdef VECTOR06_GUI
/* Service GUI windows (gui/). Emulator only holds pointers and drives
 * them through the UI-state machine, so forward declarations suffice. */
class MainMenu;
class AboutWindow;
class VirtualKeyboard;
class StateWindow;
class RomBrowser;
class MapWindow;
class GameCenter;
class MessageDialog;
#endif

class Emulator {
    static const int N_SCANCODES = 4;

private:
    enum event_type {
        /* ui to emulator thread */
        EXECUTE_FRAME,
        KEYDOWN,
        KEYUP,
        QUIT,
        /* emulator to ui */
        RENDER,
        /* DUMMY */
        VACANT
    };

    struct threadevent {
        event_type type;
        int data;
        int frame_no;
        SDL_KeyboardEvent key;
        threadevent() {}
        threadevent(event_type t, int d) : type(t), data(d) {}
        threadevent(event_type t, int d, int frameno) : type(t), data(d),
            frame_no(frameno) {}
        threadevent(event_type t, SDL_KeyboardEvent k) : 
            type(t), data(0), key(k) {}

        bool operator <(const threadevent& other) const
        {
            return false;
        }
    };

    Board & board;

#if !defined(__ANDROID__) && !defined(__GODOT__)
    boost::thread thread;
    boost::sync_queue<threadevent> ui_to_engine_queue;
    boost::sync_priority_queue<threadevent> engine_to_ui_queue;
#else
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    threadevent ui_to_engine_event;
    threadevent engine_to_ui_event;
    int keydowns[N_SCANCODES];
    int keyups[N_SCANCODES];
#endif

private:
#if !defined(__ANDROID__) && !defined(__GODOT__)
    void threadfunc();
    void handle_threadevent(threadevent & ev);
    void handle_render(threadevent & event, bool & stopping);
    void join_emulator_thread();
    bool handle_keyboard_event(SDL_KeyboardEvent & event);
    int wait_event(SDL_Event * event, threadevent & ev, int timeout);
#else
public:
    void execute_frame(); // execute frame in current thread, no mt stuff
    void keydown(int scancode);
    void keyup(int scancode);
    void export_pixel_bytes(uint8_t * dst);
    void export_audio_frame(float * dst, size_t count);
    size_t pixel_bytes_size();
#endif

public:
    Emulator(Board & borat);
    void run_event_loop();
    void start_emulator_thread();

    void save_state(vector<uint8_t> & to);
    bool restore_state(vector<uint8_t> & to);

#ifdef VECTOR06_GUI
    /* Load a ROM file into the running machine (ROM Browser / Game
     * Center): reset paging, place the image at its file-name-derived
     * origin, LOADROM reset, and remember the path for the save-state
     * directory binding (rom_base) and the Save Preview item. */
    bool load_rom(const std::string & path);
    /* Record the ROM the startup path already loaded (Options.romfile),
     * so rom_base/rom_path are bound without a second read. */
    void set_rom_path(const std::string & path);
    /* File name without extension/path, as it sits in the ROM dir;
     * empty while the boot loader runs (no ROM file behind it). */
    const std::string & get_rom_base() const { return this->rom_base; }
    const std::string & get_rom_path() const { return this->rom_path; }

    /* Wire the service GUI windows. Also installs the VKBD scancode
     * sinks so virtual key presses reach the engine key queue (or the
     * Map Keys window while it waits for an assignment). */
    void set_gui(MainMenu * menu, AboutWindow * about, VirtualKeyboard * vkbd,
                 StateWindow * sb, RomBrowser * browser, MapWindow * mapk,
                 GameCenter * gc, MessageDialog * msg);
    /* Freeze / unfreeze the machine (Main Menu open). Also toggles the
     * TV frozen-frame re-present so the paused picture stays stable. */
    void pause();
    void resume();
    bool is_paused() const { return gui_paused; }

private:
    /* Per-rendered-frame (~50 Hz) UI dispatch: compute button edges from
     * the held bitmask, run the UI-state machine, feed the active window
     * and inject game keys. Called from handle_render on the UI thread. */
    void gui_tick();
    void gui_handle_button(SDL_GameControllerButton b, bool down);
    void inject_key(SDL_Scancode sc, bool down);
    void release_held_game_keys(unsigned buttons);
    /* State Browser / Save Preview actions (UI thread, machine paused). */
    void state_save_action();
    bool state_load_action();
    void save_preview_action();
    /* MAP_KEYS -> MAIN_MENU: persist the ROM's .key next to the ROM
     * file (fully default -> deleted), hide the VKBD key picker if it
     * is still up, reopen the menu on the Map Keys item. */
    void mapkey_close_action();

    MainMenu * gui_menu = nullptr;
    AboutWindow * gui_about = nullptr;
    VirtualKeyboard * gui_vkbd = nullptr;
    StateWindow * gui_sb = nullptr;
    RomBrowser * gui_browser = nullptr;
    MapWindow * gui_mapk = nullptr;
    GameCenter * gui_gc = nullptr;
    MessageDialog * gui_msg = nullptr;
    unsigned gui_held = 0;        /* currently held controller buttons */
    unsigned gui_prev_held = 0;   /* held bitmask of the previous tick */
    unsigned gui_old_mapped = 0;  /* mapping sources fed in the previous
                                   * GAME tick (edge detection) */
    bool gui_input_muted = false; /* the previous tick did not feed the
                                   * game: swallow the held buttons on the
                                   * first GAME tick so closing a window
                                   * never fires a Vector key */
    bool gui_paused = false;
    bool gui_quit = false;        /* Exit item selected */

    /* ROM file binding for the save-state directory and Save Preview. */
    std::string rom_base;
    std::string rom_path;
#endif
};
