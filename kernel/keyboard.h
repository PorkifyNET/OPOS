#ifndef OPOS_KEYBOARD_H
#define OPOS_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Non-printable key codes queued alongside characters. KEY_SUPER is a lone
 * Super (Windows key) tap. A key pressed while Super is held is queued as
 * KEY_SUPER_PREFIX followed by that key.
 */
#define KEY_UP ((char)0x11)
#define KEY_DOWN ((char)0x12)
#define KEY_LEFT ((char)0x13)
#define KEY_RIGHT ((char)0x14)
#define KEY_SUPER ((char)0x15)
#define KEY_SUPER_PREFIX ((char)0x16)
#define KEY_DELETE ((char)0x17)
#define KEY_HOME ((char)0x18)
#define KEY_END ((char)0x19)
#define KEY_PAGE_UP ((char)0x1a)
#define KEY_PAGE_DOWN ((char)0x1c)
#define KEY_ESCAPE ((char)0x1b)
/* Menu (Application) key or Shift+F10: opens a context menu. */
#define KEY_MENU ((char)0x1d)

void keyboard_initialize(void);
void keyboard_irq_handler(void);
bool keyboard_read_character(char *character);
bool keyboard_wait_character(char *character);
uint64_t keyboard_dropped_characters(void);
bool keyboard_ctrl_held(void);
/* Whether Shift is held now (for Shift+click). */
bool keyboard_shift_held(void);
/* Whether Ctrl was held when the last character read was typed. Use this
   for shortcuts: Ctrl may be released before the character is handled. */
bool keyboard_last_ctrl(void);
/* Whether Shift was held when the last character read was typed (for
   Shift+arrow selection; letters already arrive in upper case). */
bool keyboard_last_shift(void);
/* Num Lock state: on, the keypad types digits; off, it moves the cursor. */
bool keyboard_num_lock(void);
/* Sets the keyboard LEDs; call once interrupts are on and the mouse is
   set up (the ACK arrives on port 0x60). */
void keyboard_start_leds(void);

#endif
