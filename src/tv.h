#pragma once

#include <string>
#include <functional>
#include <inttypes.h>
#include "globaldefs.h"

#if defined(__ANDROID_NDK__) || defined(__GODOT__)
#include "event.h"
#else
#include "SDL.h"
#if HAVE_OPENGL
#include "SDL_opengl.h"
#endif
#endif

#include "options.h"

#if HAS_IMAGE
extern "C" DECLSPEC int SDLCALL IMG_SavePNG(SDL_Surface *surface, const char *file);
#endif

/* GUI overlay layers (see gui/layer.h). TV only stores the pointers and
 * calls the virtual interface, so a forward declaration is enough. */
class UILayer;

class TV
{
private:
    static constexpr int NTEXTURES = 2;
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    SDL_Window * window;
    SDL_Renderer * renderer;
    SDL_Texture * texture[NTEXTURES];
#endif
    uint32_t * bmp;
    int tex_width;
    int tex_height;
    int refresh_rate;
    int texture_n;

    /* GUI overlay: an ordered (z-order) array of UILayer pointers drawn
     * above the machine picture inside render(), plus the frozen-frame
     * bookkeeping used while the machine is paused (menu open). When
     * frozen, render() re-presents last_presented instead of advancing
     * the texture ping-pong, so the paused picture does not flicker. */
    UILayer ** ui_layers = nullptr;
    int ui_layer_count = 0;
    bool frozen = false;
    int last_presented = 0;

    /* Screenshot source for save-state previews and slot thumbnails:
     * a persistent copy of the machine frame, refreshed on every
     * regular render before the streaming texture is unlocked (the
     * GUI overlay never reaches it). copy_latest_rgb() converts it
     * into 0xAABBGGRR with opaque alpha on demand. */
    uint32_t * frame_copy = nullptr;

    uint32_t pixelformat;

#if !defined(__ANDROID_NDK__) && !defined(__GODOT__) && HAVE_OPENGL
    SDL_GLContext gl_context;
    GLuint gl_textures[NTEXTURES];
    GLuint gl_program_id;
#endif
    int gl_window_width, gl_window_height;

private:
    void render_with_blend(int src_alpha);
    void render_single();
    void render_single_regular();
    void render_single_opengl();
    void render_frozen();
    void draw_ui_overlay();
    void init_regular();
    void init_opengl();
    void init_gl_textures();
    void window_resized(SDL_Event & event);

public:
    TV();
    ~TV();
    int probe();
    void init();
    void toggle_fullscreen();
    void save_frame(std::string path);
    uint32_t* pixels() const;
    std::function<uint32_t(uint8_t,uint8_t,uint8_t)> get_rgb2pixelformat() const;
    /* executed: 1 if the frame was real, 0 if the frame is a skip frame */
    void render(int executed);
    /* Install the GUI overlay layers (z-order) drawn above the picture.
     * The array is owned by the caller and must outlive TV. */
    void set_ui_layers(UILayer ** layers, int count);
    /* Freeze the machine picture: while true, render() re-presents the
     * last drawn frame (no ping-pong advance) and keeps drawing the GUI
     * overlay, so the paused screen stays stable under the menu. */
    void set_frozen(bool f);
#ifdef VECTOR06_GUI
    /* Copy the latest machine frame as 0xAABBGGRR pixels (w x h =
     * Options.screen_width x screen_height, alpha forced opaque). */
    void copy_latest_rgb(uint32_t * dst);
#endif
    int get_refresh_rate() const;
    void handle_window_event(SDL_Event & event);
};
