/*
 * Solitaire (Klondike) for OPOS.
 *
 * Build the four foundations up by suit from Ace to King. On the seven
 * columns, cards go down in alternating colours; only a King goes on an
 * empty column. The stock deals one card (or three) to the waste and
 * starts again when it runs out.
 *
 * Mouse: drag cards between piles, click the stock to deal, double-click
 * or right-click a card to send it to a foundation.
 * Keys: arrows choose a pile (Up/Down also pick how many cards of a
 * column), Space or Enter picks up and puts down, Esc lets go, A sends
 * every card it can to the foundations, U undoes, N deals a new game and
 * D switches between drawing one and three cards (a new game).
 */
#include <stdbool.h>
#include <stdint.h>

#include <opos.h>
#include <opos_gfx.h>

#include "fit.h"

OPOS_MANIFEST("Solitaire", "1.0", 0);

#define CARD_W 72
#define CARD_H 100
#define GAP 12
#define TOP 12
#define DOWN_STEP 8        /* face-down cards in a column */
#define UP_STEP 24         /* face-up cards in a column */
#define STATUS_HEIGHT 28
#define WASTE_FAN 28       /* drawing three: room for "10" */
#define BOARD_WIDTH (7 * CARD_W + 8 * GAP)
/* Smaller windows show the table shrunk (fit.h). */
#define MIN_HEIGHT 480

#define TABLE OPOS_RGB(28, 96, 64)
#define TABLE_DARK OPOS_RGB(20, 72, 48)
#define CARD_FACE OPOS_RGB(250, 250, 246)
#define CARD_EDGE OPOS_RGB(120, 126, 140)
#define BACK OPOS_RGB(44, 84, 170)
#define BACK_LINE OPOS_RGB(90, 130, 210)
#define RED OPOS_RGB(204, 36, 36)
#define BLACK OPOS_RGB(24, 24, 28)
#define FOCUS OPOS_RGB(255, 210, 80)
#define TEXT OPOS_RGB(230, 240, 232)

enum {
    STOCK,
    WASTE,
    FOUNDATION,
    TABLEAU = FOUNDATION + 4,
    PILES = TABLEAU + 7,
};

struct pile {
    uint8_t card[52];
    bool up[52];
    int count;
};

struct game {
    struct pile piles[PILES];
    int moves;
};

#define UNDO_DEPTH 64

static struct game game;
static struct game undo[UNDO_DEPTH];
static int undo_count;
static bool draw_three;
static long start_seconds = -1, end_seconds;
static bool won;

static struct fit fit;
/* What the game draws on (fit.h shrinks it into a small window). */
static struct opos_canvas *const canvas = &fit.drawing;


/* Keyboard: the pile in focus, how deep into a column, and cards held. */
static int focus = STOCK;
static int focus_depth;     /* tableau: index of the chosen card */
static bool show_focus;
static int held_pile = -1;  /* cards picked up (keys or mouse) */
static int held_index;
/* Mouse dragging. */
static bool dragging;
static int drag_x, drag_y;  /* the held cards' top-left corner */
static int grab_dx, grab_dy;
static int press_x, press_y;
static bool moved;
static long last_click_ms = -1000;
static int last_click_pile = -1, last_click_index = -1;

/* --------------------------------------------------------------- cards */

static int rank_of(uint8_t card) { return card % 13 + 1; }
static int suit_of(uint8_t card) { return card / 13; } /* S H D C */
static bool red_card(uint8_t card) {
    int suit = suit_of(card);
    return suit == 1 || suit == 2;
}

static struct pile *pile(int index) { return &game.piles[index]; }

static uint8_t top_card(int index) {
    struct pile *p = pile(index);
    return p->card[p->count - 1];
}

/* ------------------------------------------------------------- random */

static uint64_t random_state;

static uint64_t random_next(void) {
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    return random_state * 0x2545f4914f6cdd1dull;
}

static void random_seed(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    struct opos_timespec now;
    opos_uptime(&now);
    random_state = ((uint64_t)high << 32 | low)
                 ^ (uint64_t)now.nanoseconds * 0x9e3779b97f4a7c15ull
                 ^ (uint64_t)now.seconds;
    if (random_state == 0) {
        random_state = 0x853c49e6748fea9bull;
    }
}

/* --------------------------------------------------------------- time */

static long now_ms(void) {
    struct opos_timespec now;
    opos_uptime(&now);
    return now.seconds * 1000 + now.nanoseconds / 1000000;
}

static long seconds_now(void) { return now_ms() / 1000; }

static int elapsed(void) {
    if (start_seconds < 0) {
        return 0;
    }
    long end = won ? end_seconds : seconds_now();
    return (int)(end - start_seconds);
}

/* --------------------------------------------------------------- game */

static void deal(void) {
    uint8_t deck[52];
    for (int i = 0; i < 52; i++) {
        deck[i] = (uint8_t)i;
    }
    for (int i = 51; i > 0; i--) {
        int j = (int)(random_next() % (uint64_t)(i + 1));
        uint8_t swap = deck[i];
        deck[i] = deck[j];
        deck[j] = swap;
    }
    for (int i = 0; i < PILES; i++) {
        game.piles[i].count = 0;
    }
    int next = 0;
    for (int column = 0; column < 7; column++) {
        struct pile *p = pile(TABLEAU + column);
        for (int i = 0; i <= column; i++) {
            p->card[p->count] = deck[next++];
            p->up[p->count] = i == column;
            p->count++;
        }
    }
    struct pile *stock = pile(STOCK);
    while (next < 52) {
        stock->card[stock->count] = deck[next++];
        stock->up[stock->count] = false;
        stock->count++;
    }
    game.moves = 0;
    undo_count = 0;
    start_seconds = -1;
    won = false;
    held_pile = -1;
    dragging = false;
    focus = STOCK;
    focus_depth = 0;
}

static void remember(void) {
    if (undo_count == UNDO_DEPTH) {
        for (int i = 1; i < UNDO_DEPTH; i++) {
            undo[i - 1] = undo[i];
        }
        undo_count--;
    }
    undo[undo_count++] = game;
}

static void undo_move(void) {
    if (undo_count == 0 || won) {
        return;
    }
    game = undo[--undo_count];
    held_pile = -1;
    dragging = false;
}

/* After any move: start the clock, turn up uncovered column cards and
   check for a win. */
static void moved_cards(void) {
    game.moves++;
    if (start_seconds < 0) {
        start_seconds = seconds_now();
    }
    for (int i = 0; i < 7; i++) {
        struct pile *p = pile(TABLEAU + i);
        if (p->count > 0) {
            p->up[p->count - 1] = true;
        }
    }
    int done = 0;
    for (int i = 0; i < 4; i++) {
        done += pile(FOUNDATION + i)->count;
    }
    if (done == 52) {
        won = true;
        end_seconds = seconds_now();
    }
}

/* Whether the cards from index on in pile from may go on pile to. */
static bool can_move(int from, int index, int to) {
    struct pile *source = pile(from);
    if (from == to || index < 0 || index >= source->count
        || !source->up[index]) {
        return false;
    }
    uint8_t card = source->card[index];
    int count = source->count - index;
    struct pile *target = pile(to);
    if (to >= FOUNDATION && to < TABLEAU) {
        if (count != 1) {
            return false;
        }
        if (target->count == 0) {
            return rank_of(card) == 1;
        }
        uint8_t below = top_card(to);
        return suit_of(below) == suit_of(card)
            && rank_of(card) == rank_of(below) + 1;
    }
    if (to >= TABLEAU) {
        if (from == WASTE || (from >= FOUNDATION && from < TABLEAU)) {
            if (count != 1) {
                return false;
            }
        }
        if (target->count == 0) {
            return rank_of(card) == 13;
        }
        uint8_t below = top_card(to);
        return red_card(below) != red_card(card)
            && rank_of(below) == rank_of(card) + 1;
    }
    return false;
}

static void move_cards(int from, int index, int to) {
    remember();
    struct pile *source = pile(from), *target = pile(to);
    for (int i = index; i < source->count; i++) {
        target->card[target->count] = source->card[i];
        target->up[target->count] = true;
        target->count++;
    }
    source->count = index;
    moved_cards();
}

static bool try_move(int from, int index, int to) {
    if (!can_move(from, index, to)) {
        return false;
    }
    move_cards(from, index, to);
    return true;
}

/* The top card of a pile to a foundation, if one takes it. */
static bool to_foundation(int from) {
    struct pile *p = pile(from);
    if (p->count == 0 || from == STOCK
        || (from >= FOUNDATION && from < TABLEAU)) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (try_move(from, p->count - 1, FOUNDATION + i)) {
            return true;
        }
    }
    return false;
}

static void all_to_foundations(void) {
    bool any = true;
    while (any && !won) {
        any = to_foundation(WASTE);
        for (int i = 0; i < 7; i++) {
            any = to_foundation(TABLEAU + i) || any;
        }
    }
}

/* Deals from the stock, or turns the waste over when it is empty. */
static void deal_stock(void) {
    struct pile *stock = pile(STOCK), *waste = pile(WASTE);
    if (stock->count == 0 && waste->count == 0) {
        return;
    }
    remember();
    if (stock->count == 0) {
        while (waste->count > 0) {
            waste->count--;
            stock->card[stock->count] = waste->card[waste->count];
            stock->up[stock->count] = false;
            stock->count++;
        }
    } else {
        int deal_count = draw_three ? 3 : 1;
        for (int i = 0; i < deal_count && stock->count > 0; i++) {
            stock->count--;
            waste->card[waste->count] = stock->card[stock->count];
            waste->up[waste->count] = true;
            waste->count++;
        }
    }
    moved_cards();
}

/* ------------------------------------------------------------- layout */

static int board_left(void) {
    int x = (fit.width - BOARD_WIDTH) / 2;
    return x < 0 ? 0 : x;
}

static int pile_x(int index) {
    int column = index == STOCK ? 0 : index == WASTE ? 1
               : index < TABLEAU ? 3 + (index - FOUNDATION)
               : index - TABLEAU;
    return board_left() + GAP + column * (CARD_W + GAP);
}

static int tableau_top(void) { return TOP + CARD_H + 16; }

/* The step for face-up cards in a column, smaller when it would not fit. */
static int up_step(int index) {
    struct pile *p = pile(index);
    int down = 0;
    while (down < p->count && !p->up[down]) {
        down++;
    }
    int ups = p->count - down;
    int room = fit.height - STATUS_HEIGHT - tableau_top() - CARD_H - 4
             - down * DOWN_STEP;
    if (ups <= 1 || room >= (ups - 1) * UP_STEP) {
        return UP_STEP;
    }
    int step = room / (ups - 1);
    return step < 12 ? 12 : step;
}

/* Where card index of a pile is drawn. The waste fans out its last three
   cards when drawing three. */
static void card_place(int index, int card_index, int *x, int *y) {
    *x = pile_x(index);
    *y = TOP;
    if (index >= TABLEAU) {
        struct pile *p = pile(index);
        int step = up_step(index);
        *y = tableau_top();
        for (int i = 0; i < card_index; i++) {
            *y += p->up[i] ? step : DOWN_STEP;
        }
    } else if (index == WASTE && draw_three) {
        int count = pile(WASTE)->count;
        int fan = card_index - (count - 3);
        if (fan > 0) {
            *x += fan * WASTE_FAN;
        }
    }
}

/* -------------------------------------------------------------- drawing */

static const char *const suit_shapes[4][9] = {
    {"....#....", "...###...", "..#####..", ".#######.", "#########",
     "#########", ".##.#.##.", "....#....", "...###..."},
    {".##...##.", "####.####", "#########", "#########", ".#######.",
     "..#####..", "...###...", "....#....", "........."},
    {"....#....", "...###...", "..#####..", ".#######.", "#########",
     ".#######.", "..#####..", "...###...", "....#...."},
    {"...###...", "..#####..", "..#####..", ".##.#.##.", "#########",
     "#########", ".##.#.##.", "....#....", "...###..."},
};

static void draw_suit(int x, int y, int suit, int scale, uint32_t colour) {
    for (int row = 0; row < 9; row++) {
        for (int column = 0; column < 9; column++) {
            if (suit_shapes[suit][row][column] == '#') {
                opos_fill_rect(canvas, x + column * scale, y + row * scale,
                               scale, scale, colour);
            }
        }
    }
}

/* A card-shaped area with its corners cut. */
static void card_shape(int x, int y, uint32_t edge, uint32_t inside) {
    opos_fill_rect(canvas, x + 2, y, CARD_W - 4, CARD_H, edge);
    opos_fill_rect(canvas, x, y + 2, CARD_W, CARD_H - 4, edge);
    opos_fill_rect(canvas, x + 1, y + 1, CARD_W - 2, CARD_H - 2, edge);
    opos_fill_rect(canvas, x + 2, y + 1, CARD_W - 4, CARD_H - 2, inside);
    opos_fill_rect(canvas, x + 1, y + 2, CARD_W - 2, CARD_H - 4, inside);
}

static void draw_card(int x, int y, uint8_t card, bool up) {
    if (!up) {
        card_shape(x, y, CARD_EDGE, BACK);
        /* A dotted pattern inside a frame. */
        for (int row = y + 6; row < y + CARD_H - 6; row += 6) {
            for (int column = x + 6 + (row / 6 % 2) * 3;
                 column < x + CARD_W - 6; column += 6) {
                opos_fill_rect(canvas, column, row, 2, 2, BACK_LINE);
            }
        }
        opos_draw_rect(canvas, x + 4, y + 4, CARD_W - 8, CARD_H - 8,
                       BACK_LINE);
        return;
    }
    card_shape(x, y, CARD_EDGE, CARD_FACE);
    uint32_t colour = red_card(card) ? RED : BLACK;
    static const char *const names[13] = {"A", "2", "3", "4", "5", "6", "7",
                                          "8", "9", "10", "J", "Q", "K"};
    const char *name = names[rank_of(card) - 1];
    opos_draw_text(canvas, x + 5, y + 5, name, colour, 2);
    opos_draw_text(canvas, x + 6, y + 5, name, colour, 2);
    draw_suit(x + 6, y + 26, suit_of(card), 1, colour);
    /* The other corner, the right way up (the font cannot turn). */
    int name_width = opos_text_width(name, 2);
    opos_draw_text(canvas, x + CARD_W - 6 - name_width,
                   y + CARD_H - 5 - opos_char_height(2), name, colour, 2);
    draw_suit(x + CARD_W - 15, y + CARD_H - 37, suit_of(card), 1, colour);
    draw_suit(x + (CARD_W - 27) / 2, y + (CARD_H - 27) / 2 + 4,
              suit_of(card), 3, colour);
}

static void draw_slot(int x, int y, const char *label) {
    opos_draw_rect(canvas, x, y, CARD_W, CARD_H, TABLE_DARK);
    opos_draw_rect(canvas, x + 1, y + 1, CARD_W - 2, CARD_H - 2, TABLE_DARK);
    if (label != 0) {
        int width = opos_text_width(label, 2);
        opos_draw_text(canvas, x + (CARD_W - width) / 2,
                       y + (CARD_H - opos_char_height(2)) / 2, label,
                       TABLE_DARK, 2);
    }
}

static void draw_focus(int x, int y, int height) {
    opos_draw_rect(canvas, x - 3, y - 3, CARD_W + 6, height + 6, FOCUS);
    opos_draw_rect(canvas, x - 2, y - 2, CARD_W + 4, height + 4, FOCUS);
}

static bool is_held(int index, int card_index) {
    return held_pile == index && card_index >= held_index;
}

static void draw_pile(int index) {
    struct pile *p = pile(index);
    int x = pile_x(index), y = index >= TABLEAU ? tableau_top() : TOP;
    if (index == STOCK) {
        draw_slot(x, y, p->count == 0 && pile(WASTE)->count > 0 ? "O" : 0);
        if (p->count > 0) {
            draw_card(x, y, 0, false);
        }
        return;
    }
    if (index >= FOUNDATION && index < TABLEAU) {
        draw_slot(x, y, "A");
    } else if (index >= TABLEAU) {
        draw_slot(x, y, "K");
    } else {
        draw_slot(x, y, 0);
    }
    int first = 0;
    if (index == WASTE || (index >= FOUNDATION && index < TABLEAU)) {
        /* Only the cards that show. */
        first = p->count - (index == WASTE && draw_three ? 3 : 2);
        if (first < 0) {
            first = 0;
        }
    }
    for (int i = first; i < p->count; i++) {
        if (dragging && is_held(index, i)) {
            break;
        }
        int cx, cy;
        card_place(index, i, &cx, &cy);
        draw_card(cx, cy, p->card[i], p->up[i]);
    }
}

static void format_time(char *out, int seconds) {
    int minutes = seconds / 60;
    int used = 0;
    if (minutes >= 10) {
        out[used++] = (char)('0' + minutes / 10 % 10);
    }
    out[used++] = (char)('0' + minutes % 10);
    out[used++] = ':';
    out[used++] = (char)('0' + seconds % 60 / 10);
    out[used++] = (char)('0' + seconds % 10);
    out[used] = '\0';
}

static int append(char *out, int used, const char *text) {
    while (*text != '\0' && used < 95) {
        out[used++] = *text++;
    }
    out[used] = '\0';
    return used;
}

static int append_number(char *out, int used, int value) {
    char digits[12];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0);
    while (count > 0 && used < 95) {
        out[used++] = digits[--count];
    }
    out[used] = '\0';
    return used;
}

static void draw(void) {
    if (canvas->pixels == 0) {
        return;
    }
    opos_fill_rect(canvas, 0, 0, fit.width, fit.height, TABLE);
    for (int i = 0; i < PILES; i++) {
        draw_pile(i);
    }
    /* Keyboard focus, and cards held with the keyboard. */
    if (show_focus && !dragging) {
        struct pile *p = pile(focus);
        int x, y;
        int height = CARD_H;
        if (focus >= TABLEAU && p->count > 0) {
            card_place(focus, focus_depth, &x, &y);
            int last_x, last_y;
            card_place(focus, p->count - 1, &last_x, &last_y);
            height = last_y + CARD_H - y;
        } else if (p->count > 0 && focus != STOCK) {
            card_place(focus, p->count - 1, &x, &y);
        } else {
            x = pile_x(focus);
            y = focus >= TABLEAU ? tableau_top() : TOP;
        }
        draw_focus(x, y, height);
    }
    if (held_pile >= 0 && !dragging) {
        int x, y;
        card_place(held_pile, held_index, &x, &y);
        opos_draw_rect(canvas, x - 1, y - 1, CARD_W + 2, CARD_H + 2,
                       OPOS_RGB(80, 200, 255));
        opos_draw_rect(canvas, x, y, CARD_W, CARD_H, OPOS_RGB(80, 200, 255));
    }
    /* Cards being dragged, over everything. */
    if (dragging) {
        struct pile *p = pile(held_pile);
        for (int i = held_index; i < p->count; i++) {
            draw_card(drag_x, drag_y + (i - held_index) * UP_STEP, p->card[i],
                      true);
        }
    }
    /* The status line. */
    char line[96];
    char time[8];
    format_time(time, elapsed());
    int used = append(line, 0, "MOVES ");
    used = append_number(line, used, game.moves);
    used = append(line, used, "   TIME ");
    used = append(line, used, time);
    append(line, used, draw_three ? "   DRAW 3" : "   DRAW 1");
    int status_y = fit.height - STATUS_HEIGHT;
    int text_y = status_y + (STATUS_HEIGHT - opos_char_height(2)) / 2;
    opos_fill_rect(canvas, 0, status_y, fit.width, STATUS_HEIGHT,
                   TABLE_DARK);
    int line_end = opos_draw_text(canvas, 10, text_y, line, TEXT, 2);
    const char *hints = "N:NEW U:UNDO D:DRAW";
    int hints_x = fit.width - 10 - opos_text_width(hints, 2);
    if (hints_x > line_end + 24) {
        opos_draw_text(canvas, hints_x, text_y, hints,
                       OPOS_RGB(150, 190, 165), 2);
    }
    if (won) {
        char message[96];
        int n = append(message, 0, "YOU WON IN ");
        n = append(message, n, time);
        n = append(message, n, " WITH ");
        n = append_number(message, n, game.moves);
        n = append(message, n, " MOVES");
        int width = opos_text_width(message, 3) + 40;
        int x = (fit.width - width) / 2, y = fit.height / 2 - 40;
        opos_fill_rect(canvas, x, y, width, 80, OPOS_RGB(16, 50, 34));
        opos_draw_rect(canvas, x, y, width, 80, FOCUS);
        opos_draw_text(canvas, x + 20, y + 14, message, FOCUS, 3);
        const char *again = "N: DEAL AGAIN";
        opos_draw_text(canvas, (fit.width - opos_text_width(again, 2)) / 2,
                       y + 52, again, TEXT, 2);
    }
    fit_present(&fit);
}

/* ---------------------------------------------------------------- mouse */

/* The pile and card under a point; card -1 for an empty pile's place. */
static bool hit(int px, int py, int *index, int *card_index) {
    for (int i = 0; i < PILES; i++) {
        struct pile *p = pile(i);
        int x = pile_x(i);
        int width = CARD_W + (i == WASTE && draw_three ? 2 * WASTE_FAN : 0);
        if (px < x || px >= x + width) {
            continue;
        }
        if (i >= TABLEAU) {
            for (int c = p->count - 1; c >= 0; c--) {
                int cx, cy;
                card_place(i, c, &cx, &cy);
                if (px >= cx && px < cx + CARD_W && py >= cy
                    && py < cy + CARD_H) {
                    *index = i;
                    *card_index = c;
                    return true;
                }
            }
            int y = tableau_top();
            if (p->count == 0 && py >= y && py < y + CARD_H) {
                *index = i;
                *card_index = -1;
                return true;
            }
            continue;
        }
        if (p->count > 0) {
            int cx, cy;
            card_place(i, p->count - 1, &cx, &cy);
            if (px >= cx && px < cx + CARD_W && py >= cy && py < cy + CARD_H) {
                *index = i;
                *card_index = p->count - 1;
                return true;
            }
        }
        if (px < x + CARD_W && py >= TOP && py < TOP + CARD_H) {
            *index = i;
            *card_index = p->count > 0 ? p->count - 1 : -1;
            return true;
        }
    }
    return false;
}

/* Where dropped cards land: the pile whose place overlaps them most. */
static int drop_target(void) {
    int best = -1, best_area = 0;
    for (int i = WASTE + 1; i < PILES; i++) {
        struct pile *p = pile(i);
        int x, y;
        if (p->count > 0 && !(i == held_pile)) {
            card_place(i, p->count - 1, &x, &y);
        } else {
            x = pile_x(i);
            y = i >= TABLEAU ? tableau_top() : TOP;
        }
        int w = (drag_x < x ? drag_x + CARD_W - x : x + CARD_W - drag_x);
        int h = (drag_y < y ? drag_y + CARD_H - y : y + CARD_H - drag_y);
        if (w > 0 && h > 0 && w * h > best_area
            && can_move(held_pile, held_index, i)) {
            best = i;
            best_area = w * h;
        }
    }
    return best;
}

static void mouse_down(const struct opos_event *event) {
    int index, card_index;
    show_focus = false;
    held_pile = -1;
    if (won || !hit(event->x, event->y, &index, &card_index)) {
        return;
    }
    if (index == STOCK) {
        deal_stock();
        return;
    }
    if (card_index < 0 || !pile(index)->up[card_index]) {
        return;
    }
    if (event->button == OPOS_BUTTON_RIGHT) {
        to_foundation(index);
        return;
    }
    long now = now_ms();
    if (now - last_click_ms < 450 && last_click_pile == index
        && last_click_index == card_index
        && card_index == pile(index)->count - 1) {
        last_click_ms = -1000;
        to_foundation(index);
        return;
    }
    last_click_ms = now;
    last_click_pile = index;
    last_click_index = card_index;
    int x, y;
    card_place(index, card_index, &x, &y);
    held_pile = index;
    held_index = card_index;
    dragging = true;
    moved = false;
    grab_dx = event->x - x;
    grab_dy = event->y - y;
    drag_x = x;
    drag_y = y;
    press_x = event->x;
    press_y = event->y;
}

static void mouse_move(const struct opos_event *event) {
    if (!dragging) {
        return;
    }
    int dx = event->x - press_x, dy = event->y - press_y;
    if (dx * dx + dy * dy > 9) {
        moved = true;
    }
    drag_x = event->x - grab_dx;
    drag_y = event->y - grab_dy;
}

static void mouse_up(void) {
    if (!dragging) {
        return;
    }
    if (moved) {
        int target = drop_target();
        if (target >= 0) {
            move_cards(held_pile, held_index, target);
        }
    }
    dragging = false;
    held_pile = -1;
}

/* ------------------------------------------------------------------ keys */

/* Left and Right go through the top row, then the columns. */
static const int key_order[PILES] = {
    STOCK, WASTE, FOUNDATION, FOUNDATION + 1, FOUNDATION + 2, FOUNDATION + 3,
    TABLEAU, TABLEAU + 1, TABLEAU + 2, TABLEAU + 3, TABLEAU + 4, TABLEAU + 5,
    TABLEAU + 6,
};

static int first_up(int index) {
    struct pile *p = pile(index);
    int i = 0;
    while (i < p->count && !p->up[i]) {
        i++;
    }
    return i < p->count ? i : (p->count > 0 ? p->count - 1 : 0);
}

static void set_focus(int index) {
    focus = index;
    struct pile *p = pile(index);
    focus_depth = p->count > 0 ? p->count - 1 : 0;
}

/* The column under a top-row pile, and the other way round. */
static int column_below(int index) {
    int column = (pile_x(index) - board_left() - GAP) / (CARD_W + GAP);
    return TABLEAU + column;
}

static int pile_above(int index) {
    int column = index - TABLEAU;
    return column == 0 ? STOCK : column == 1 ? WASTE
         : column == 2 ? WASTE : FOUNDATION + column - 3;
}

static void key_activate(void) {
    struct pile *p = pile(focus);
    if (held_pile >= 0) {
        int from = held_pile;
        held_pile = -1;
        if (from != focus) {
            try_move(from, held_index, focus);
        }
        set_focus(focus);
        return;
    }
    if (focus == STOCK) {
        deal_stock();
        return;
    }
    if (p->count == 0) {
        return;
    }
    int index = focus >= TABLEAU ? focus_depth : p->count - 1;
    if (!p->up[index]) {
        return;
    }
    held_pile = focus;
    held_index = index;
}

static void key(const struct opos_event *event) {
    uint32_t character = event->key;
    if (character == 'n' || character == 'N') {
        deal();
        return;
    }
    if (character == 'd' || character == 'D') {
        draw_three = !draw_three;
        deal();
        return;
    }
    if (character == 'u' || character == 'U'
        || (character == 'z' && (event->buttons & OPOS_MOD_CTRL) != 0)) {
        undo_move();
        return;
    }
    if (won) {
        return;
    }
    if (character == 'a' || character == 'A') {
        held_pile = -1;
        all_to_foundations();
        return;
    }
    if (character == OPOS_KEY_ESCAPE) {
        held_pile = -1;
        return;
    }
    show_focus = true;
    int position = 0;
    while (key_order[position] != focus) {
        position++;
    }
    if (character == OPOS_KEY_LEFT) {
        set_focus(key_order[(position + PILES - 1) % PILES]);
    } else if (character == OPOS_KEY_RIGHT) {
        set_focus(key_order[(position + 1) % PILES]);
    } else if (character == OPOS_KEY_UP) {
        if (focus >= TABLEAU && focus_depth > first_up(focus)) {
            focus_depth--;
        } else if (focus >= TABLEAU) {
            set_focus(pile_above(focus));
        }
    } else if (character == OPOS_KEY_DOWN) {
        struct pile *p = pile(focus);
        if (focus < TABLEAU) {
            set_focus(column_below(focus));
            focus_depth = held_pile >= 0 || pile(focus)->count == 0
                        ? focus_depth : first_up(focus);
        } else if (focus_depth + 1 < p->count) {
            focus_depth++;
        }
    } else if (character == ' ' || character == '\n') {
        key_activate();
    }
}

/* ----------------------------------------------------------------- main */

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    random_seed();
    deal();
    long created = opos_window_create("Solitaire", BOARD_WIDTH, 600);
    if (created < 0) {
        opos_print("solitaire: no window; run it from the desktop\n");
        return 1;
    }
    struct opos_event event;
    long last_second = -1;
    for (;;) {
        long timeout = start_seconds >= 0 && !won ? 500 : -1;
        long got = opos_window_event(&event, timeout);
        if (got < 0) {
            continue;
        }
        if (got == 0) {
            if (seconds_now() != last_second) {
                last_second = seconds_now();
                draw();
            }
            continue;
        }
        /* Mouse positions in the drawing's pixels. */
        event.x = fit_x(&fit, event.x);
        event.y = fit_y(&fit, event.y);
        switch (event.type) {
        case OPOS_EVENT_RESIZE:
            if (!fit_layout(&fit, (int)event.width, (int)event.height,
                            BOARD_WIDTH, MIN_HEIGHT)) {
                opos_print("solitaire: out of memory\n");
                return 1;
            }
            break;
        case OPOS_EVENT_MOUSE_DOWN:
            mouse_down(&event);
            break;
        case OPOS_EVENT_MOUSE_MOVE:
            mouse_move(&event);
            break;
        case OPOS_EVENT_MOUSE_UP:
            mouse_up();
            break;
        case OPOS_EVENT_KEY:
            key(&event);
            break;
        case OPOS_EVENT_CLOSE:
            return 0;
        default:
            break;
        }
        draw();
    }
}
