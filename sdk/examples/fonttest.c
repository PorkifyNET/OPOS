/*
 * Fonttest: shows the bundled Noto fonts at several sizes.
 * Run with "run /bin/fonttest"; Escape or closing the window quits.
 */
#include <opos.h>
#include <opos_font.h>
#include <opos_gfx.h>
#include <stdio.h>

#define DIR "/share/fonts/"

int main(void) {
    if (opos_window_create("FONT TEST", 900, 640) < 0) {
        return 1;
    }
    static const char *names[] = {"NotoSans-Regular.ttf", "NotoSans-Bold.ttf",
                                  "NotoSans-Italic.ttf",
                                  "NotoSans-BoldItalic.ttf",
                                  "NotoSansMono-Regular.ttf"};
    struct opos_font *fonts[5];
    for (int i = 0; i < 5; i++) {
        char path[64];
        snprintf(path, sizeof(path), DIR "%s", names[i]);
        fonts[i] = opos_font_load(path);
    }
    uint32_t width = 900, height = 640;
    struct opos_canvas canvas = opos_canvas_create(1920, 1080);
    struct opos_event event;
    for (;;) {
        opos_fill_rect(&canvas, 0, 0, canvas.width, canvas.height,
                       OPOS_RGB(250, 250, 246));
        int y = 8;
        static const int sizes[] = {12, 16, 24, 40};
        for (int f = 0; f < 5; f++) {
            if (fonts[f] == NULL) {
                opos_draw_text(&canvas, 10, y, "FONT MISSING", OPOS_RGB(200, 0, 0), 2);
                y += 24;
                continue;
            }
            int size = sizes[f % 4];
            if (f == 0) {
                for (int s = 0; s < 4; s++) {
                    const char *text = "The quick brown fox jumps over the lazy dog 0123";
                    int ascent = opos_font_ascent(fonts[0], sizes[s]);
                    opos_font_draw(fonts[0], &canvas, 10, y + ascent, sizes[s],
                                   text, __builtin_strlen(text), OPOS_RGB(20, 20, 30));
                    y += opos_font_line_height(fonts[0], sizes[s]) + 2;
                }
                continue;
            }
            const char *text = f == 4
                ? "Mono: if (x != 0) { return 42; } // AVATAR"
                : "Gamma \xce\xb3 caf\xc3\xa9 \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xe2\x80\x9cquoted\xe2\x80\x9d \xe2\x82\xac 12.50 AVATAR To";
            int ascent = opos_font_ascent(fonts[f], size);
            int end = opos_font_draw(fonts[f], &canvas, 10, y + ascent, size,
                                     text, __builtin_strlen(text), OPOS_RGB(20, 20, 30));
            char info[48];
            snprintf(info, sizeof(info), "w=%d", opos_font_text_width(fonts[f], size, text, __builtin_strlen(text)));
            opos_draw_text(&canvas, end + 12, y + 4, info, OPOS_RGB(160, 0, 0), 1);
            y += opos_font_line_height(fonts[f], size) + 6;
        }
        opos_canvas_present_area(&canvas, (int)width, (int)height);
        long got = opos_window_event(&event, -1);
        if (got < 0 || (got > 0 && (event.type == OPOS_EVENT_CLOSE
                || (event.type == OPOS_EVENT_KEY && event.key == OPOS_KEY_ESCAPE)))) {
            break;
        }
        if (got > 0 && event.type == OPOS_EVENT_RESIZE) {
            width = event.width;
            height = event.height;
        }
    }
    return 0;
}
