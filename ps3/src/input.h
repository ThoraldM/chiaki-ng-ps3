// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PS3 input: DualShock 3 / Sixaxis mapped onto a PS4 controller state.
#ifndef CHIAKI_PS3_INPUT_H
#define CHIAKI_PS3_INPUT_H

#include <chiaki/controller.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum
{
	INPUT_ACTION_NONE = 0,
	INPUT_ACTION_QUIT,     // Select + Circle held for 1 s
	INPUT_ACTION_TOGGLE_STATS, // Select + Triangle pressed
} InputAction;

// Menu navigation events (bit mask), with auto-repeat on the D-pad.
enum
{
	MENU_EV_UP = 1 << 0,
	MENU_EV_DOWN = 1 << 1,
	MENU_EV_LEFT = 1 << 2,
	MENU_EV_RIGHT = 1 << 3,
	MENU_EV_CROSS = 1 << 4,
	MENU_EV_CIRCLE = 1 << 5,
	MENU_EV_TRIANGLE = 1 << 6,
	MENU_EV_SQUARE = 1 << 7,
	MENU_EV_START = 1 << 8,
	MENU_EV_SELECT = 1 << 9,
	MENU_EV_L1 = 1 << 10,
	MENU_EV_R1 = 1 << 11,
};

bool input_init(void);
// Console rumble (0-255 each): left = low-frequency/strong, right = high-frequency/light.
// Thread safe; applied to the DS3 motors by input_poll(). enabled = false stops the motors.
void input_set_rumble(uint8_t left, uint8_t right);
void input_set_rumble_enabled(bool enabled);
// Turn the motors off now (call when a stream ends).
void input_stop_rumble(void);
// Buttons newly pressed since the last call (plus D-pad repeats). Call once per frame.
uint32_t input_menu_events(void);
// Treat buttons held right now as already pressed (call when the menu comes back).
void input_menu_reset(void);
// Call before a stream starts: clears combo state; SELECT combos need SELECT released first.
void input_session_reset(void);
void input_fini(void);
// Reads the first connected pad into *state. Returns a special action, if any.
InputAction input_poll(ChiakiControllerState *state);

#endif
