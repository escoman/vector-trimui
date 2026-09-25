#include <string>
#include <inttypes.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "globaldefs.h"
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
#include "SDL.h"
#if HAVE_OPENGL
#include "SDL_opengl.h"
#endif
#include "icon.h"
#else
#include "event.h"
#endif

#include "options.h"
#include "tv.h"

#ifdef VECTOR06_GUI
#include "layer.h"
#include "layer_draw.h"
#endif


#if HAVE_OPENGL

#ifdef __APPLE__
#include <OpenGL/GL.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "glextns.h"
#include "shaders.h"

#endif

#if HAS_IMAGE
extern "C" DECLSPEC int SDLCALL IMG_SavePNG(SDL_Surface *surface, const char *file);
#endif

TV::TV() : pixelformat(TV_PIXELFORMAT)
{
}

TV::~TV()
{
    if (Options.novideo) {
        delete[] bmp;
    }
    delete[] frame_copy;
}

int TV::probe()
{
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    static int display_in_use = 0; /* Only using first display */

    int i, display_mode_count;
    SDL_DisplayMode mode;
    Uint32 f;

    SDL_Log("SDL_GetNumVideoDisplays(): %i", SDL_GetNumVideoDisplays());

    display_mode_count = SDL_GetNumDisplayModes(display_in_use);
    if (display_mode_count < 1) {
        SDL_Log("SDL_GetNumDisplayModes failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Log("SDL_GetNumDisplayModes: %i", display_mode_count);

    for (i = 0; i < display_mode_count; ++i) {
        if (SDL_GetDisplayMode(display_in_use, i, &mode) != 0) {
            SDL_Log("SDL_GetDisplayMode failed: %s", SDL_GetError());
            return 1;
        }
        f = mode.format;

        SDL_Log("Mode %i\tbpp %i\t%s\t%i x %i %dHz", i,
                SDL_BITSPERPIXEL(f), SDL_GetPixelFormatName(f), mode.w, mode.h,
                mode.refresh_rate);
    }
#endif
    return 0;
}

void TV::init()
{
    if (Options.novideo || Options.opengl) {
        /* if novideo, we need a texture to draw to */
        /* if OpenGl, allocate pixels */
        this->bmp = new uint32_t[Options.screen_width * Options.screen_height];
    }
    if (Options.novideo) {
        return;
    }
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)

    SDL_Init(SDL_INIT_VIDEO);
#if 0
    probe();
#endif
    SDL_DisplayMode display_mode;
    SDL_GetCurrentDisplayMode(0, &display_mode);

    printf("Current display mode: %dx%d %dHz\n", display_mode.w, display_mode.h,
            display_mode.refresh_rate);
    this->refresh_rate = display_mode.refresh_rate;

    if (Options.opengl) {
#if HAVE_OPENGL
        this->init_opengl();
#else
        this->init_regular();
#endif
    }
    else {
        this->init_regular();
    }
#endif
}

void TV::init_regular()
{
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    int window_options = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    /* Do not force SDL_RENDERER_ACCELERATED: on framebuffer/KMSDRM targets
     * only the software renderer may exist, and a NULL renderer here would
     * later cause wild writes through NULL textures. flags=0 lets SDL pick
     * the best available renderer (still accelerated on desktop). */
    int renderer_options = 0;
    if (Options.vsync) {
        renderer_options |= SDL_RENDERER_PRESENTVSYNC;
    }

    int window_height = Options.screen_height * 2;
    int window_width = window_height * 5 / 4;

    window_width += 2 * (Options.border_width - 32);

    this->window = SDL_CreateWindow("Вектор-06ц", 0, 0, 
            window_width, window_height, window_options);
    this->renderer = SDL_CreateRenderer(this->window, -1, 
            renderer_options);
    if (this->window == NULL || this->renderer == NULL) {
        fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
        exit(1);
    }

    SDL_RendererInfo rinfo;
    SDL_GetRendererInfo(this->renderer, &rinfo);
    printf("SDL_RendererInfo: %s SDL_RENDERER_ACCELERATED\n", 
            (rinfo.flags & SDL_RENDERER_ACCELERATED) ? "yes" : "no");
    printf("SDL_RendererInfo: %s SDL_RENDERER_TARGETTEXTURE\n", 
            (rinfo.flags & SDL_RENDERER_TARGETTEXTURE) ? "yes" : "no");

    uint32_t window_pixelformat = SDL_GetWindowPixelFormat(this->window);
    switch (window_pixelformat) {
        /* in theory we should follow native format, but on linux vm
         * I'm getting native format RGB888, which gets internally
         * converted into ARGB8888 anyway */
        case SDL_PIXELFORMAT_ARGB8888:
            printf("Native pixelformat: ARGB8888\n");
            this->pixelformat = SDL_PIXELFORMAT_ARGB8888;
            break;
        case SDL_PIXELFORMAT_ABGR8888:
            printf("Native pixelformat: ABGR8888\n");
            this->pixelformat = SDL_PIXELFORMAT_ABGR8888;
            break;
        case SDL_PIXELFORMAT_RGB888:
            printf("Native pixelformat: RGB888\n");
            this->pixelformat = SDL_PIXELFORMAT_RGB888;
            break;
        case SDL_PIXELFORMAT_BGR888:
            printf("Native pixelformat: BGR888\n");
            this->pixelformat = SDL_PIXELFORMAT_BGR888;
            break;
        default:
            printf("Unknown native pixelformat: %08x, falling back to ARGB8888\n",
                    window_pixelformat);
            this->pixelformat = SDL_PIXELFORMAT_ARGB8888;
            break;
    }

    if (!Options.window) {
        SDL_SetWindowFullscreen(this->window, 
#if __MACOSX__
                SDL_WINDOW_FULLSCREEN
#else
                SDL_WINDOW_FULLSCREEN_DESKTOP
#endif
                );
    }
    else {
        SDL_SetWindowPosition(this->window, 100, 100);
    }

    this->tex_width = Options.screen_width;
    this->tex_height = Options.screen_height;
    for (int i = 0; i < TV::NTEXTURES; ++i) {
        this->texture[i] = SDL_CreateTexture(this->renderer, this->pixelformat,
                SDL_TEXTUREACCESS_STREAMING, this->tex_width, this->tex_height);
        if (this->texture[i] == NULL) {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            exit(1);
        }
    }
    this->texture_n = 0;

#ifdef VECTOR06_GUI
    /* Persistent machine-frame copy for the save-state screenshots
     * (see copy_latest_rgb); refreshed in the regular render paths. */
    this->frame_copy = new uint32_t[this->tex_width * this->tex_height];
#endif

    SDL_RenderSetLogicalSize(this->renderer, window_width, window_height);

    icon_set(this->window);
#endif
}

void TV::init_opengl()
{
#if HAVE_OPENGL
    int window_height = Options.screen_height * 2;
    int window_width = window_height * 5 / 4;

    window_width += 2 * (Options.border_width - 32);

    int window_options = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | 
        SDL_WINDOW_OPENGL;
    this->window = SDL_CreateWindow("Вектор-06ц (gl)", 0, 0, 
            window_width, window_height, window_options);
    gl_context = SDL_GL_CreateContext(window);

    const GLubyte* openGLVersion = glGetString(GL_VERSION);
    printf("GL_VERSION: %s\n", openGLVersion);

    this->gl_program_id = 0;
    if (Options.gl.use_shader) {
        if (initGLExtensions()) {
            init_shaders(this->gl_program_id);
        } else {
            fprintf(stderr, 
                    "Could not init gl extensions, shaders will not be used\n"); 
        }
    }

    SDL_GL_SetSwapInterval(Options.vsync ? 1 : 0);

    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);

    init_gl_textures();

    if (!Options.window) {
        SDL_SetWindowFullscreen(this->window, 
#if __MACOSX__
                SDL_WINDOW_FULLSCREEN
#else
                SDL_WINDOW_FULLSCREEN_DESKTOP
#endif
                );
    }
    else {
        SDL_SetWindowPosition(this->window, 100, 100);
    }
    icon_set(this->window);
#endif
}


void TV::init_gl_textures()
{
#if HAVE_OPENGL
    printf("Texture size: %dx%d\n", Options.screen_width, Options.screen_height);
    this->pixelformat = SDL_PIXELFORMAT_ARGB8888;
    glGenTextures(1, &this->gl_textures[0]);
    glBindTexture(GL_TEXTURE_2D, gl_textures[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    if (Options.gl.filtering) {
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,
                GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    } 
    else {
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    }

    glTexImage2D(GL_TEXTURE_2D, 0, 
            /* internalformat */ GL_RGBA8,
            Options.screen_width, Options.screen_height, /* border */ 0, 
            /* format */ GL_RGBA, 
            /* type */   GL_UNSIGNED_BYTE, 
            (uint8_t*)this->bmp);
#endif
}

void TV::window_resized(SDL_Event & event)
{
#if HAVE_OPENGL
    if (Options.opengl) {
        int window_width, window_height;
        SDL_GetWindowSize(this->window, &window_width, &window_height);

    	int ww = window_height * 5 / 4;
        int wh = window_height;
        if (ww > window_width) {
            ww = window_width;
            wh = window_width * 4 / 5;
        }
        this->gl_window_width = ww;
        this->gl_window_height = wh;

        int ox = window_width/2 - ww/2;
        int oy = window_height/2 - wh/2;
        glViewport(ox, oy, ww, wh);
    }
#endif
}

void TV::handle_window_event(SDL_Event & event)
{
    switch (event.window.event) {
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_EXPOSED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            window_resized(event);
            break;
        }    
}

void TV::toggle_fullscreen()
{
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    int windowflags = SDL_GetWindowFlags(this->window);
#if __MACOSX__
    /* on mac only this works */
    int fs = SDL_WINDOW_FULLSCREEN;
#else
    /* on windows regular FULLSCREEN distorts aspect ratio disregarding
     * logical size */
    int fs = SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    int set = (windowflags ^ fs) & fs;
    SDL_SetWindowFullscreen(this->window, set);
    SDL_RenderSetLogicalSize(this->renderer, 4, 3);
#endif
}

void TV::save_frame(std::string path)
{
#if HAS_IMAGE
    SDL_Surface * s = SDL_CreateRGBSurfaceWithFormatFrom(this->bmp, 
            Options.screen_width, Options.screen_height, 32, 
            4*Options.screen_width, this->pixelformat);

    if (IMG_SavePNG(s, path.c_str()) < 0) {
        const char * msg = SDL_GetError();
        fprintf(stderr, "Error saving %s: %s\n", path.c_str(), msg);
    };

    SDL_FreeSurface(s);
#endif
}

uint32_t* TV::pixels() const {
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    if (!this->bmp) {
        int pitch;
        SDL_LockTexture(this->texture[this->texture_n], NULL, 
                (void**)&this->bmp, &pitch);
    }
#endif
    return this->bmp;
}

void TV::render_with_blend(int src_alpha)
{
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    if (Options.opengl) {
        printf("render_with_blend not supported on opengl");
        return;
    }
    int A = this->texture_n;
    int B = (this->texture_n + 1) & 1;

#ifdef VECTOR06_GUI
    if (this->bmp && this->frame_copy) {
        memcpy(this->frame_copy, this->bmp,
               (size_t)Options.screen_width * Options.screen_height * 4);
    }
#endif

    /* render single frame */
    SDL_UnlockTexture(this->texture[A]);
    this->bmp = NULL;
    SDL_SetTextureBlendMode(this->texture[A], SDL_BLENDMODE_NONE);
    SDL_SetTextureAlphaMod(this->texture[A], 255);
    SDL_RenderClear(this->renderer);
    SDL_RenderCopy(this->renderer, this->texture[A], NULL, NULL);

    SDL_SetTextureBlendMode(this->texture[B], SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(this->texture[B], src_alpha);
    SDL_RenderCopy(this->renderer, this->texture[B], NULL, NULL);

    this->last_presented = A;
    this->texture_n = (this->texture_n + 1) & 1;
#endif
}

void TV::render_single()
{
    if (Options.opengl) {
#if HAVE_OPENGL
        this->render_single_opengl();
#else
        this->render_single_regular();
#endif
    }
    else {
        this->render_single_regular();
    }
}

void TV::render_single_regular()
{
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    /* render single frame */
    int t = this->texture_n;
#ifdef VECTOR06_GUI
    if (this->bmp && this->frame_copy) {
        memcpy(this->frame_copy, this->bmp,
               (size_t)Options.screen_width * Options.screen_height * 4);
    }
#endif
    SDL_UnlockTexture(this->texture[t]);
    this->bmp = NULL;
    SDL_SetTextureBlendMode(this->texture[t], SDL_BLENDMODE_NONE);
    SDL_SetTextureAlphaMod(this->texture[t], 255);
    SDL_RenderClear(this->renderer);
    SDL_RenderCopy(this->renderer, this->texture[t], NULL, NULL);
    this->last_presented = t;
    this->texture_n = this->texture_n + 1;
    if (this->texture_n >= TV::NTEXTURES) this->texture_n = 0;
#endif
}

/* Re-present the frame drawn before the machine was frozen. The paused
 * picture lives in texture[last_presented]; the ping-pong cursor is not
 * advanced, so consecutive frozen frames show the same stable image
 * (the GUI overlay on top is what actually changes). */
void TV::render_frozen()
{
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    int t = this->last_presented;
    this->bmp = NULL;
    SDL_SetTextureBlendMode(this->texture[t], SDL_BLENDMODE_NONE);
    SDL_SetTextureAlphaMod(this->texture[t], 255);
    SDL_RenderClear(this->renderer);
    SDL_RenderCopy(this->renderer, this->texture[t], NULL, NULL);
#endif
}

/* Draw the GUI overlay (dim backdrop + active UILayers in z-order) above
 * the machine picture, in the 480x272 UI space mapped onto the renderer
 * logical size by layer_draw_begin(). */
void TV::draw_ui_overlay()
{
#ifdef VECTOR06_GUI
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    layer_draw_begin(this->renderer);

    /* Repaint any layer whose visual state changed since the last frame.
     * paint() and draw() both run on the UI thread here, so the raster
     * is always current before the quad is blitted. */
    for (int i = 0; i < this->ui_layer_count; ++i) {
        if (this->ui_layers[i]->is_active() && this->ui_layers[i]->needs_repaint()) {
            this->ui_layers[i]->paint();
        }
    }

    bool any_dim = false;
    for (int i = 0; i < this->ui_layer_count; ++i) {
        if (this->ui_layers[i]->is_active() && this->ui_layers[i]->wants_dim()) {
            any_dim = true;
            break;
        }
    }
    if (any_dim) {
        layer_draw_dim_overlay();
    }

    for (int i = 0; i < this->ui_layer_count; ++i) {
        if (this->ui_layers[i]->is_active()) {
            this->ui_layers[i]->draw();
        }
    }
#endif
#endif
}

void TV::set_ui_layers(UILayer ** layers, int count)
{
    this->ui_layers = layers;
    this->ui_layer_count = count;
}

void TV::set_frozen(bool f)
{
    this->frozen = f;
}

#ifdef VECTOR06_GUI
/* Screenshot source: the persistent frame copy (pure machine picture,
 * the GUI overlay never reaches it) converted from the streaming-texture
 * pixel format into 0xAABBGGRR with opaque alpha, exactly what img_save
 * and the StateWindow thumbnail shrinker consume. */
void TV::copy_latest_rgb(uint32_t * dst)
{
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
    const int w = Options.screen_width;
    const int h = Options.screen_height;
    if (!this->frame_copy) {
        memset(dst, 0, (size_t)w * h * sizeof(uint32_t));
        return;
    }

    int bpp = 0;
    Uint32 rm = 0, gm = 0, bm = 0, am = 0;
    SDL_PixelFormatEnumToMasks(this->pixelformat, &bpp, &rm, &gm, &bm, &am);
    auto shift_of = [](Uint32 m) {
        int s = 0;
        while (m && !(m & 1)) { m >>= 1; ++s; }
        return s;
    };
    const int rs = shift_of(rm), gs = shift_of(gm), bs = shift_of(bm);

    const uint32_t * src = this->frame_copy;
    for (int i = 0; i < w * h; ++i) {
        const uint32_t p = src[i];
        const uint32_t r = (p & rm) >> rs;
        const uint32_t g = (p & gm) >> gs;
        const uint32_t b = (p & bm) >> bs;
        dst[i] = 0xff000000u | (b << 16) | (g << 8) | r;
    }
#endif
}
#endif

void TV::render_single_opengl()
{
#if HAVE_OPENGL
    const int w = Options.screen_width, h = Options.screen_height;   // 576x288
    int old_program_id;

    /* This should be done but there's an linkage error with mingw.
     * We only have one texture anyway so whatevs..*/
#ifndef __MINGW32__
     glActiveTexture(GL_TEXTURE0); 
#endif
    glBindTexture(GL_TEXTURE_2D, this->gl_textures[0]);
    if (this->gl_program_id) {
        GLuint pid = this->gl_program_id;
        glGetIntegerv(GL_CURRENT_PROGRAM, &old_program_id);
        glUseProgram(pid);

        GLint tex0 = glGetUniformLocation(pid, "Texture0");
        glUniform1i(tex0, 0);

        GLint tex0sz = glGetUniformLocation(pid, "color_texture_sz");
        glUniform2f(tex0sz, (float)w, (float)h);

        GLint screensz = glGetUniformLocation(pid, "screen_texture_sz");
        glUniform2f(screensz, (float)this->gl_window_width, 
                (float)this->gl_window_height);

        GLint filter_gain = glGetUniformLocation(pid, "filter_gain");
        glUniform1f(filter_gain, 1.6f);

        GLint filter_invgain = glGetUniformLocation(pid, "filter_invgain");
        glUniform1f(filter_invgain, 1.6f);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, 
            GL_BGRA, GL_UNSIGNED_BYTE, (GLvoid*)this->bmp);

    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION); 
    glLoadIdentity(); 
    glOrtho(0.0f, w, h, 0.0f, 0.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW); 
    glLoadIdentity();
 
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0,0);
    glTexCoord2f(1, 0); glVertex2f(w,0);
    glTexCoord2f(1, 1); glVertex2f(w,h);
    glTexCoord2f(0, 1); glVertex2f(0,h);
    glEnd();
    
    if (this->gl_program_id) {
        glUseProgram(old_program_id);
    }
#endif
}

/* executed: 1 if the frame was real, 0 if the frame is a skip frame */
void TV::render(int executed)
{
    static int prev_executed;
    if (!Options.novideo) {
        if (frozen) {
            render_frozen();
        }
        else if (Options.blendmode == 0) {
            if (executed) render_single();
        } 
        else if (Options.blendmode == 1) {
            if (executed) {
                render_with_blend(128);
            }
        }
        else if (Options.blendmode == 2) {
            if (executed && prev_executed) {
                render_with_blend(200);
            } 
            else {
                if (executed) render_single();
            }
        }
#if !defined(__ANDROID_NDK__) && !defined(__GODOT__)
#ifdef VECTOR06_GUI
        /* GUI overlay is drawn above the picture and below the present. */
        if (!Options.opengl) {
            draw_ui_overlay();
        }
#endif
        /* it is actually better to call SDL_RenderPresent
         * because it maintains the pace. Otherwise we use 100% CPU
         * when stopped in debugger. */
        if (Options.opengl) {
            SDL_GL_SwapWindow(window);
        } 
        else {
            SDL_RenderPresent(this->renderer);
        } 
#endif
        prev_executed = executed;
    }
}

int TV::get_refresh_rate() const
{
    return this->refresh_rate;
}

std::function<uint32_t(uint8_t,uint8_t,uint8_t)> TV::get_rgb2pixelformat() const
{
    switch(this->pixelformat) {
        case SDL_PIXELFORMAT_RGB888:
        case SDL_PIXELFORMAT_ARGB8888:
            return [](uint8_t r, uint8_t g, uint8_t b) {
                uint32_t result =
                    0xff000000 |
                    (r << (5 + 16)) |
                    (g << (5 + 8)) |
                    (b << (6 + 0));
                 return result;
            };
            break;
        case SDL_PIXELFORMAT_BGR888:
        case SDL_PIXELFORMAT_ABGR8888:
            return [](uint8_t r, uint8_t g, uint8_t b) {
                uint32_t result =
                    0xff000000 |
                    (b << (6 + 16)) |
                    (g << (5 + 8)) |
                    (r << (5 + 0));
                 return result;
            };
            break;
    }
    printf("Impossible pixelformat: %08x\n", this->pixelformat);
    return nullptr;
}


