/*
  SDL video driver for OPOS windows.
  Copyright (C) 2026 the OPOS authors

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
 * OPOS gives each program one window. The program draws 0x00RRGGBB pixels
 * in its own memory and presents them; the desktop sends key, mouse,
 * resize and close events. This driver uses that through the window
 * system calls in sdk/include/opos.h (the same numbers, written out here
 * so SDL needs no OPOS headers):
 * - one SDL window (a second one fails);
 * - a framebuffer in SDL_PIXELFORMAT_RGB888, presented whole;
 * - key presses and releases as scan codes (OPOS_WINDOW_KEY_STATE), typed
 *   characters as text input;
 * - mouse buttons, motion and wheel.
 * There is no OpenGL or Vulkan; SDL's software renderer draws.
 */
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_OPOS

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_windowevents_c.h"

#define OPOSVID_DRIVER_NAME "opos"

#define OPOS_SYS_WINDOW_CREATE 0x4f500010L
#define OPOS_SYS_WINDOW_PRESENT 0x4f500011L
#define OPOS_SYS_WINDOW_EVENT 0x4f500012L
#define OPOS_SYS_WINDOW_OPTIONS 0x4f500016L
#define OPOS_WINDOW_KEY_STATE 1u
#define OPOS_WINDOW_QUIET 2u
#define OPOS_WINDOW_FULLSCREEN 4u
#define OPOS_SYS_SCREEN_SIZE 0x4f500017L
#define OPOS_SYS_WINDOW_RESIZE 0x4f500018L
/* "1" keeps notifications quiet while the window is in front, "0" never
   does; unset, the window is quiet while it is full screen or holds the
   mouse in relative mode (a game being played). */
#define OPOS_HINT_QUIET "SDL_OPOS_QUIET"

#define OPOS_EVENT_KEY 1
#define OPOS_EVENT_MOUSE_DOWN 2
#define OPOS_EVENT_MOUSE_UP 3
#define OPOS_EVENT_MOUSE_MOVE 4
#define OPOS_EVENT_RESIZE 5
#define OPOS_EVENT_CLOSE 6
#define OPOS_EVENT_WHEEL 7
#define OPOS_EVENT_KEY_STATE 8
#define OPOS_MOD_CTRL 1u
#define OPOS_BUTTON_LEFT 1u
#define OPOS_BUTTON_RIGHT 2u

struct opos_event {
    Uint32 type;
    Uint32 key;
    Sint32 x;
    Sint32 y;
    Uint32 button;
    Uint32 buttons;
    Uint32 width;
    Uint32 height;
};

static long opos_call(long number, long a, long b, long c, long d)
{
    long result;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

/* The one window; whether its OPOS window is open (that waits for the
   first framebuffer, as SDL sets the title after creating a window), and
   whether the desktop sends key presses and releases (else each typed key
   becomes a press and a release). */
static SDL_Window *opos_window;
static SDL_bool opos_opened;
static SDL_bool opos_key_state;
static SDL_bool opos_closed;
static SDL_bool opos_relative;
static SDL_bool opos_fullscreen;
static SDL_bool opos_quiet_supported = SDL_TRUE;
static SDL_bool opos_fullscreen_supported = SDL_TRUE;
static int opos_quiet_hint = -1; /* -1 unset, 0 or 1 */
static Uint32 opos_options;

/* Sends the window options when they change. OPOS before 0.1.5 refuses
   OPOS_WINDOW_QUIET and before 0.6.2 OPOS_WINDOW_FULLSCREEN; the window
   then goes without that option. */
static void OPOS_UpdateOptions(void)
{
    SDL_bool quiet;
    Uint32 options;
    if (!opos_opened) {
        return;
    }
    quiet = opos_quiet_hint >= 0 ? (SDL_bool)opos_quiet_hint
                                 : (SDL_bool)(opos_relative
                                              || opos_fullscreen);
    options = opos_key_state ? OPOS_WINDOW_KEY_STATE : 0;
    if (quiet && opos_quiet_supported) {
        options |= OPOS_WINDOW_QUIET;
    }
    if (opos_fullscreen && opos_fullscreen_supported) {
        options |= OPOS_WINDOW_FULLSCREEN;
    }
    if (options == opos_options) {
        return;
    }
    if (opos_call(OPOS_SYS_WINDOW_OPTIONS, options, 0, 0, 0) < 0) {
        /* Find out which option this OPOS lacks, then send the rest. */
        if ((options & OPOS_WINDOW_FULLSCREEN)
            && opos_call(OPOS_SYS_WINDOW_OPTIONS,
                         options & ~OPOS_WINDOW_FULLSCREEN, 0, 0, 0) == 0) {
            opos_fullscreen_supported = SDL_FALSE;
            opos_options = options & ~OPOS_WINDOW_FULLSCREEN;
        } else if (options & OPOS_WINDOW_QUIET) {
            opos_quiet_supported = SDL_FALSE;
            opos_fullscreen_supported = SDL_FALSE;
        }
        return;
    }
    opos_options = options;
}

static void SDLCALL OPOS_QuietHintChanged(void *userdata, const char *name,
                                          const char *old_value,
                                          const char *value)
{
    (void)userdata;
    (void)name;
    (void)old_value;
    opos_quiet_hint = (value && *value) ? (SDL_GetStringBoolean(value,
                                                                SDL_FALSE)
                                           ? 1 : 0)
                                        : -1;
    OPOS_UpdateOptions();
}

/* OPOS has no pointer capture: motion keeps coming as absolute positions,
   from which SDL works out the relative motion. Relative mode only marks
   the window quiet. */
static int OPOS_SetRelativeMouseMode(SDL_bool enabled)
{
    opos_relative = enabled;
    OPOS_UpdateOptions();
    return 0;
}

#define OPOS_SURFACE "_SDL_OposSurface"

/* PC set-1 scan codes (0x80 added after 0xe0) to SDL scan codes. */
static SDL_Scancode OPOS_Scancode(Uint32 key)
{
    static const SDL_Scancode plain[0x59] = {
        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_ESCAPE,
        SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
        SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
        SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS,
        SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
        SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I,
        SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET,
        SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_RETURN, SDL_SCANCODE_LCTRL,
        SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F,
        SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K,
        SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_APOSTROPHE,
        SDL_SCANCODE_GRAVE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSLASH,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V,
        SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA,
        SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_RSHIFT,
        SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_LALT, SDL_SCANCODE_SPACE,
        SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_F1, SDL_SCANCODE_F2,
        SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6,
        SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10,
        SDL_SCANCODE_NUMLOCKCLEAR, SDL_SCANCODE_SCROLLLOCK,
        SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9,
        SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5,
        SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_1,
        SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_0,
        SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
        SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    };
    if (key < 0x80) {
        return key < SDL_arraysize(plain) ? plain[key] : SDL_SCANCODE_UNKNOWN;
    }
    switch (key & 0x7f) {
    case 0x1c: return SDL_SCANCODE_KP_ENTER;
    case 0x1d: return SDL_SCANCODE_RCTRL;
    case 0x35: return SDL_SCANCODE_KP_DIVIDE;
    case 0x38: return SDL_SCANCODE_RALT;
    case 0x47: return SDL_SCANCODE_HOME;
    case 0x48: return SDL_SCANCODE_UP;
    case 0x49: return SDL_SCANCODE_PAGEUP;
    case 0x4b: return SDL_SCANCODE_LEFT;
    case 0x4d: return SDL_SCANCODE_RIGHT;
    case 0x4f: return SDL_SCANCODE_END;
    case 0x50: return SDL_SCANCODE_DOWN;
    case 0x51: return SDL_SCANCODE_PAGEDOWN;
    case 0x52: return SDL_SCANCODE_INSERT;
    case 0x53: return SDL_SCANCODE_DELETE;
    case 0x5b: return SDL_SCANCODE_LGUI;
    case 0x5c: return SDL_SCANCODE_RGUI;
    case 0x5d: return SDL_SCANCODE_APPLICATION;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

/* Without key state: the scan code of a typed character, if it has one. */
static SDL_Scancode OPOS_CharacterScancode(Uint32 character)
{
    switch (character) {
    case '\n': return SDL_SCANCODE_RETURN;
    case '\b': return SDL_SCANCODE_BACKSPACE;
    case '\t': return SDL_SCANCODE_TAB;
    case 0x11: return SDL_SCANCODE_UP;
    case 0x12: return SDL_SCANCODE_DOWN;
    case 0x13: return SDL_SCANCODE_LEFT;
    case 0x14: return SDL_SCANCODE_RIGHT;
    case 0x17: return SDL_SCANCODE_DELETE;
    case 0x18: return SDL_SCANCODE_HOME;
    case 0x19: return SDL_SCANCODE_END;
    case 0x1a: return SDL_SCANCODE_PAGEUP;
    case 0x1b: return SDL_SCANCODE_ESCAPE;
    case 0x1c: return SDL_SCANCODE_PAGEDOWN;
    default: break;
    }
    if (character >= 'A' && character <= 'Z') {
        character += 'a' - 'A';
    }
    if (character >= 0x20 && character < 0x7f) {
        return SDL_GetScancodeFromKey((SDL_Keycode)character);
    }
    return SDL_SCANCODE_UNKNOWN;
}

static void OPOS_PumpEvents(_THIS)
{
    struct opos_event event;
    SDL_Window *window = opos_window;
    (void)_this;
    /* After the close event the next wait ends the program. */
    while (window && opos_opened && !opos_closed
           && opos_call(OPOS_SYS_WINDOW_EVENT, (long)&event, 0, 0, 0) == 1) {
        switch (event.type) {
        case OPOS_EVENT_KEY_STATE: {
            SDL_Scancode code = OPOS_Scancode(event.key);
            if (code != SDL_SCANCODE_UNKNOWN) {
                SDL_SendKeyboardKey(event.button ? SDL_PRESSED : SDL_RELEASED,
                                    code);
            }
            break;
        }
        case OPOS_EVENT_KEY:
            if (!opos_key_state) {
                SDL_Scancode code = OPOS_CharacterScancode(event.key);
                if (code != SDL_SCANCODE_UNKNOWN) {
                    SDL_SendKeyboardKey(SDL_PRESSED, code);
                    SDL_SendKeyboardKey(SDL_RELEASED, code);
                }
            }
            if (event.key >= 0x20 && event.key < 0x7f
                && (event.buttons & OPOS_MOD_CTRL) == 0) {
                char text[2] = { (char)event.key, '\0' };
                SDL_SendKeyboardText(text);
            }
            break;
        case OPOS_EVENT_MOUSE_MOVE:
            SDL_SendMouseMotion(window, 0, 0, event.x, event.y);
            break;
        case OPOS_EVENT_MOUSE_DOWN:
        case OPOS_EVENT_MOUSE_UP:
            SDL_SendMouseMotion(window, 0, 0, event.x, event.y);
            SDL_SendMouseButton(window, 0,
                                event.type == OPOS_EVENT_MOUSE_DOWN
                                    ? SDL_PRESSED : SDL_RELEASED,
                                event.button == OPOS_BUTTON_RIGHT
                                    ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT);
            break;
        case OPOS_EVENT_WHEEL:
            SDL_SendMouseWheel(window, 0, 0.0f, (float)(Sint32)event.key,
                               SDL_MOUSEWHEEL_NORMAL);
            break;
        case OPOS_EVENT_RESIZE:
            /* A window that is not resizable keeps its size; the desktop
               shows its pixels at the top left. */
            if ((window->flags & SDL_WINDOW_RESIZABLE) != 0
                && event.width > 0 && event.height > 0) {
                SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED,
                                    (int)event.width, (int)event.height);
            }
            break;
        case OPOS_EVENT_CLOSE:
            opos_closed = SDL_TRUE;
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0);
            break;
        default:
            break;
        }
    }
}

static int OPOS_CreateWindow(_THIS, SDL_Window *window)
{
    (void)_this;
    if (opos_window) {
        return SDL_SetError("OPOS gives a program one window");
    }
    opos_window = window;
    window->x = 0;
    window->y = 0;
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);
    return 0;
}

static int OPOS_OpenWindow(SDL_Window *window)
{
    const char *title = window->title && *window->title ? window->title
                                                         : "SDL";
    long result;
    if (opos_opened) {
        return 0;
    }
    result = opos_call(OPOS_SYS_WINDOW_CREATE, (long)title,
                       (long)SDL_strlen(title), window->w, window->h);
    if (result < 0) {
        return SDL_SetError("Couldn't open an OPOS window (error %ld)",
                            -result);
    }
    opos_opened = SDL_TRUE;
    opos_key_state = opos_call(OPOS_SYS_WINDOW_OPTIONS,
                               OPOS_WINDOW_KEY_STATE, 0, 0, 0) == 0;
    opos_options = opos_key_state ? OPOS_WINDOW_KEY_STATE : 0;
    opos_fullscreen = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    OPOS_UpdateOptions();
    return 0;
}

/* The program changed its window size (e.g. another resolution picked in
   a game's settings). The desktop resizes the window as far as the screen
   allows and answers with a RESIZE event. Older OPOS versions refuse the
   call; the window then keeps its size. */
static void OPOS_RequestSize(int w, int h)
{
    if (opos_opened && !opos_closed && w > 0 && h > 0) {
        (void)opos_call(OPOS_SYS_WINDOW_RESIZE, w, h, 0, 0);
    }
}

static void OPOS_SetWindowSize(_THIS, SDL_Window *window)
{
    (void)_this;
    if (window == opos_window) {
        OPOS_RequestSize(window->w, window->h);
    }
}

static void OPOS_SetWindowFullscreen(_THIS, SDL_Window *window,
                                     SDL_VideoDisplay *display,
                                     SDL_bool fullscreen)
{
    (void)_this;
    (void)display;
    if (window == opos_window) {
        opos_fullscreen = fullscreen;
        OPOS_UpdateOptions();
        if (!fullscreen) {
            /* Back to the size the window had (or was given) before. */
            OPOS_RequestSize(window->windowed.w, window->windowed.h);
        }
    }
}

/* OPOS never changes the screen mode: a full-screen window smaller than
   the screen is scaled up by the desktop. Accepting the mode lets
   exclusive full-screen requests (e.g. 640x480) work. */
static int OPOS_SetDisplayMode(_THIS, SDL_VideoDisplay *display,
                               SDL_DisplayMode *mode)
{
    (void)_this;
    (void)display;
    (void)mode;
    return 0;
}

static void OPOS_DestroyWindow(_THIS, SDL_Window *window)
{
    (void)_this;
    if (window == opos_window) {
        opos_window = NULL;
    }
    /* The OPOS window stays until the program ends (it cannot be closed
       early); a later SDL window draws into it again. */
}

static int OPOS_CreateWindowFramebuffer(_THIS, SDL_Window *window,
                                        Uint32 *format, void **pixels,
                                        int *pitch)
{
    SDL_Surface *surface;
    int w, h;
    (void)_this;
    if (window != opos_window) {
        return SDL_SetError("OPOS gives a program one window");
    }
    if (OPOS_OpenWindow(window) < 0) {
        return -1;
    }
    SDL_FreeSurface((SDL_Surface *)SDL_SetWindowData(window, OPOS_SURFACE,
                                                     NULL));
    SDL_GetWindowSizeInPixels(window, &w, &h);
    surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 0,
                                             SDL_PIXELFORMAT_RGB888);
    if (!surface) {
        return -1;
    }
    SDL_SetWindowData(window, OPOS_SURFACE, surface);
    *format = SDL_PIXELFORMAT_RGB888;
    *pixels = surface->pixels;
    *pitch = surface->pitch;
    return 0;
}

static int OPOS_UpdateWindowFramebuffer(_THIS, SDL_Window *window,
                                        const SDL_Rect *rects, int numrects)
{
    SDL_Surface *surface =
        (SDL_Surface *)SDL_GetWindowData(window, OPOS_SURFACE);
    long result;
    (void)_this;
    (void)rects;
    (void)numrects;
    if (!surface) {
        return SDL_SetError("Couldn't find the OPOS window surface");
    }
    if (window != opos_window || opos_closed) {
        return 0;
    }
    result = opos_call(OPOS_SYS_WINDOW_PRESENT, (long)surface->pixels,
                       surface->w, surface->h, surface->pitch / 4);
    return result < 0 ? SDL_SetError("OPOS present failed (%ld)", -result)
                      : 0;
}

static void OPOS_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    (void)_this;
    SDL_FreeSurface((SDL_Surface *)SDL_SetWindowData(window, OPOS_SURFACE,
                                                     NULL));
}

static int OPOS_VideoInit(_THIS)
{
    /* The desktop mode is the screen (older OPOS cannot tell; 1024x768
       then). A full-screen window gets the whole screen; a smaller mode
       is scaled up by the desktop, so every mode below works too. */
    static const int sizes[][2] = {
        { 2560, 1600 }, { 2560, 1440 }, { 1920, 1200 }, { 1920, 1080 },
        { 1680, 1050 }, { 1600, 1200 }, { 1600, 900 }, { 1440, 900 },
        { 1366, 768 }, { 1280, 1024 }, { 1280, 800 }, { 1280, 720 },
        { 1024, 768 }, { 800, 600 }, { 640, 480 }
    };
    SDL_DisplayMode mode;
    long screen = opos_call(OPOS_SYS_SCREEN_SIZE, 0, 0, 0, 0);
    int screen_w, screen_h;
    size_t i;
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB888;
    mode.w = sizes[0][0];
    mode.h = sizes[0][1];
    if (screen > 0 && (screen >> 32) > 0 && (screen & 0xffffffffL) > 0) {
        mode.w = (int)(screen >> 32);
        mode.h = (int)(screen & 0xffffffffL);
    }
    mode.refresh_rate = 60;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        return -1;
    }
    SDL_AddDisplayMode(&_this->displays[0], &mode);
    screen_w = mode.w;
    screen_h = mode.h;
    for (i = 0; i < SDL_arraysize(sizes); i++) {
        if (sizes[i][0] > screen_w || sizes[i][1] > screen_h
            || (sizes[i][0] == screen_w && sizes[i][1] == screen_h)) {
            continue;
        }
        mode.w = sizes[i][0];
        mode.h = sizes[i][1];
        SDL_AddDisplayMode(&_this->displays[0], &mode);
    }
    SDL_GetMouse()->SetRelativeMouseMode = OPOS_SetRelativeMouseMode;
    SDL_AddHintCallback(OPOS_HINT_QUIET, OPOS_QuietHintChanged, NULL);
    return 0;
}

static void OPOS_VideoQuit(_THIS)
{
    (void)_this;
    SDL_DelHintCallback(OPOS_HINT_QUIET, OPOS_QuietHintChanged, NULL);
}

static void OPOS_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static SDL_VideoDevice *OPOS_CreateDevice(void)
{
    SDL_VideoDevice *device =
        (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }
    device->VideoInit = OPOS_VideoInit;
    device->VideoQuit = OPOS_VideoQuit;
    device->PumpEvents = OPOS_PumpEvents;
    device->CreateSDLWindow = OPOS_CreateWindow;
    device->DestroyWindow = OPOS_DestroyWindow;
    device->SetWindowFullscreen = OPOS_SetWindowFullscreen;
    device->SetWindowSize = OPOS_SetWindowSize;
    device->SetDisplayMode = OPOS_SetDisplayMode;
    device->CreateWindowFramebuffer = OPOS_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = OPOS_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = OPOS_DestroyWindowFramebuffer;
    device->free = OPOS_DeleteDevice;
    return device;
}

VideoBootStrap OPOS_bootstrap = {
    OPOSVID_DRIVER_NAME, "SDL video driver for OPOS windows",
    OPOS_CreateDevice,
    NULL /* no ShowMessageBox implementation */
};

#endif /* SDL_VIDEO_DRIVER_OPOS */
