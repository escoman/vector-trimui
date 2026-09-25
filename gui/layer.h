#pragma once

#include <inttypes.h>

/*
 * UILayer — common interface of every on-screen GUI element drawn by
 * TV::render() above the Vector picture.  TV knows nothing about
 * specific window classes; it iterates an ordered array of UILayer
 * pointers and calls draw() on each active one.
 *
 * The ordering in the array defines the z-order: layers drawn later
 * appear on top.  main.cpp builds the array in the correct order.
 *
 * Threading: is_active(), needs_repaint(), paint() follow the same
 * worker/display thread split as before.  draw() is called by the
 * display thread inside TV::render().
 */
class UILayer
{
public:
    virtual ~UILayer();

    /* Worker thread: the layer is currently visible. */
    virtual bool is_active() const = 0;

    /* Display thread: repaint machinery. */
    virtual bool needs_repaint() const = 0;
    virtual void paint() = 0;

    /* Display thread: texture upload handshake. */
    virtual bool consume_tex_upload() = 0;
    virtual const uint8_t * tex_data() const = 0;
    virtual const uint32_t * clut_data() const = 0;

    /* Texture power-of-two dimensions (for GE texture setup). */
    virtual int tex_width() const = 0;
    virtual int tex_height() const = 0;

    /* On-screen display dimensions (may differ from texture size). */
    virtual int display_width() const = 0;
    virtual int display_height() const = 0;

    /* Whether this layer wants the global dim overlay drawn behind it.
     * Default: true.  MessageDialog and VirtualKeyboard override to
     * false (they handle their own transparency). */
    virtual bool wants_dim() const { return true; }

    /* Display thread: draw this layer via the PSP GE.  The default
     * implementation draws a single centered textured quad using the
     * layer's own indexed texture — sufficient for most popup windows.
     * Subclasses with supplementary rendering (thumbnails, previews)
     * override and add their extra quads after the base call. */
    virtual void draw();
};
