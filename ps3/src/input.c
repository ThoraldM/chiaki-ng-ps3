// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// DualShock 3 -> PS4 controller mapping.
//
// Direct: face buttons, D-pad, L1/R1, L3/R3, sticks, L2/R2 (analog through
// pressure mode when the pad supports it), Start -> OPTIONS.
//
// SELECT is a modifier for the buttons a DS3 does not have:
//   SELECT alone (tap)   -> SHARE
//   SELECT + START       -> PS button
//   SELECT + CROSS       -> touchpad click (center)
//   SELECT + L1 / R1     -> touchpad click on the left / right half
//   SELECT + CIRCLE 1 s  -> quit the stream
//   SELECT + TRIANGLE    -> show / hide the stats overlay
// (The real PS button opens the PS3's own XMB menu.)

#include "input.h"

#include <string.h>

#include <io/pad.h>
#include <sys/systime.h>

#define TOUCH_W 1920
#define TOUCH_H 942
#define SHARE_PULSE_US (100 * 1000)
#define QUIT_HOLD_US   (1000 * 1000)

static bool initialized;
static int active_port = -1;
static bool press_mode;

static bool select_used;      // another button was pressed while SELECT was held
static bool select_was_down;
static int64_t share_until;
static int64_t quit_since;
static bool stats_combo_down;
static bool combos_armed;     // false until SELECT has been seen released (new session)

// One cache for the menu and the stream: the pad only reports changes (len == 0
// means "same as before"), so two readers with separate caches go stale.
static padData pad_cache;
static int pad_cache_port = -1;

static bool read_pad(int port, padData *out)
{
	padData pd;
	memset(&pd, 0, sizeof(pd));
	if(port != pad_cache_port)
	{
		memset(&pad_cache, 0, sizeof(pad_cache));
		pad_cache_port = port;
	}
	if(ioPadGetData(port, &pd) != 0)
		return false;
	if(pd.len > 0)
		pad_cache = pd;
	*out = pad_cache;
	return out->len > 0;
}

// --- vibration --------------------------------------------------------------
// DS3: large motor 0-255, small motor on/off only.
#define RUMBLE_SMALL_ON      96            // right strength that switches the small motor on
#define RUMBLE_REFRESH_US    (500 * 1000)  // re-send while active, in case the pad drops it

static volatile uint8_t rumble_left, rumble_right;
static volatile bool rumble_enabled = true;
static uint8_t sent_large, sent_small;
static int64_t rumble_sent_at;

void input_set_rumble(uint8_t left, uint8_t right)
{
	rumble_left = left;
	rumble_right = right;
}

void input_set_rumble_enabled(bool enabled)
{
	rumble_enabled = enabled;
}

static void apply_rumble(int port, uint8_t large, uint8_t small, int64_t now)
{
	padActParam act;
	memset(&act, 0, sizeof(act));
	act.small_motor = small;
	act.large_motor = large;
	ioPadSetActDirect((u32)port, &act);
	sent_large = large;
	sent_small = small;
	rumble_sent_at = now;
}

static void update_rumble(int port, int64_t now)
{
	uint8_t l = rumble_left, r = rumble_right;
	uint8_t large = 0, small = 0;
	if(rumble_enabled)
	{
		// light (right) effects still come through on the large motor at reduced strength
		unsigned from_right = (unsigned)r * 2 / 5;
		large = (uint8_t)(l > from_right ? l : from_right);
		small = r >= RUMBLE_SMALL_ON ? 1 : 0;
	}
	bool changed = large != sent_large || small != sent_small;
	bool refresh = (large || small) && now - rumble_sent_at >= RUMBLE_REFRESH_US;
	if(changed || refresh)
		apply_rumble(port, large, small, now);
}

void input_stop_rumble(void)
{
	rumble_left = rumble_right = 0;
	if(initialized && active_port >= 0)
		apply_rumble(active_port, 0, 0, sysGetSystemTime());
}

static int16_t stick(unsigned v)
{
	int r = ((int)v - 128) * 258;
	if(r > 32767) r = 32767;
	if(r < -32768) r = -32768;
	return (int16_t)r;
}

bool input_init(void)
{
	if(ioPadInit(MAX_PORT_NUM) != 0)
		return false;
	initialized = true;
	return true;
}

void input_fini(void)
{
	input_stop_rumble();
	if(initialized)
		ioPadEnd();
	initialized = false;
}

static int find_port(void)
{
	padInfo info;
	if(ioPadGetInfo(&info) != 0)
		return -1;
	for(int i = 0; i < MAX_PORT_NUM; i++)
		if(info.status[i])
			return i;
	return -1;
}

static void set_touch(ChiakiControllerState *s, uint16_t x)
{
	s->buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
	s->touches[0].id = 0;
	s->touches[0].x = x;
	s->touches[0].y = TOUCH_H / 2;
}

InputAction input_poll(ChiakiControllerState *s)
{
	chiaki_controller_state_set_idle(s);
	if(!initialized)
		return INPUT_ACTION_NONE;

	int port = find_port();
	if(port < 0)
	{
		active_port = -1;
		return INPUT_ACTION_NONE;
	}
	if(port != active_port)
	{
		active_port = port;
		sent_large = sent_small = 0; // a newly connected pad starts with motors off
		press_mode = ioPadInfoPressMode(port) == PAD_INFO_SUPPORTED_PRESS_MODE
			&& ioPadSetPressMode(port, PAD_PRESS_MODE_ON) == 0;
	}

	padData pd;
	if(!read_pad(port, &pd))
		return INPUT_ACTION_NONE;

	int64_t now = sysGetSystemTime();
	InputAction action = INPUT_ACTION_NONE;
	update_rumble(port, now);

	s->left_x = stick(pd.ANA_L_H);
	s->left_y = stick(pd.ANA_L_V);
	s->right_x = stick(pd.ANA_R_H);
	s->right_y = stick(pd.ANA_R_V);

	if(press_mode)
	{
		s->l2_state = (uint8_t)(pd.PRE_L2 > 255 ? 255 : pd.PRE_L2);
		s->r2_state = (uint8_t)(pd.PRE_R2 > 255 ? 255 : pd.PRE_R2);
	}
	else
	{
		s->l2_state = pd.BTN_L2 ? 255 : 0;
		s->r2_state = pd.BTN_R2 ? 255 : 0;
	}
	if(s->l2_state)
		s->buttons |= CHIAKI_CONTROLLER_ANALOG_BUTTON_L2;
	if(s->r2_state)
		s->buttons |= CHIAKI_CONTROLLER_ANALOG_BUTTON_R2;

	if(pd.BTN_UP) s->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
	if(pd.BTN_DOWN) s->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
	if(pd.BTN_LEFT) s->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
	if(pd.BTN_RIGHT) s->buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
	if(pd.BTN_TRIANGLE) s->buttons |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;
	if(pd.BTN_SQUARE) s->buttons |= CHIAKI_CONTROLLER_BUTTON_BOX;
	if(pd.BTN_L3) s->buttons |= CHIAKI_CONTROLLER_BUTTON_L3;
	if(pd.BTN_R3) s->buttons |= CHIAKI_CONTROLLER_BUTTON_R3;

	if(pd.BTN_SELECT && !combos_armed)
	{
		// SELECT still held from the menu / previous stream: ignore it until released
		pd.BTN_SELECT = 0;
		pd.BTN_CIRCLE = 0;
	}
	else if(!pd.BTN_SELECT)
		combos_armed = true;

	if(pd.BTN_SELECT)
	{
		// Modifier mode: these buttons are consumed by the combos.
		if(pd.BTN_START)
		{
			s->buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
			select_used = true;
		}
		if(pd.BTN_CROSS)
		{
			set_touch(s, TOUCH_W / 2);
			select_used = true;
		}
		if(pd.BTN_L1)
		{
			set_touch(s, TOUCH_W / 4);
			select_used = true;
		}
		if(pd.BTN_R1)
		{
			set_touch(s, TOUCH_W * 3 / 4);
			select_used = true;
		}
		if(pd.BTN_CIRCLE)
		{
			select_used = true;
			if(!quit_since)
				quit_since = now;
			else if(now - quit_since >= QUIT_HOLD_US)
				action = INPUT_ACTION_QUIT;
		}
		else
			quit_since = 0;
		if(pd.BTN_TRIANGLE && !stats_combo_down && action == INPUT_ACTION_NONE)
			action = INPUT_ACTION_TOGGLE_STATS;
		stats_combo_down = pd.BTN_TRIANGLE != 0;
		s->buttons &= ~(uint32_t)CHIAKI_CONTROLLER_BUTTON_PYRAMID; // the combo is not sent to the console
		if(pd.BTN_SQUARE || pd.BTN_TRIANGLE || pd.BTN_UP || pd.BTN_DOWN || pd.BTN_LEFT || pd.BTN_RIGHT)
			select_used = true;
		select_was_down = true;
	}
	else
	{
		if(select_was_down && !select_used)
			share_until = now + SHARE_PULSE_US; // a plain tap of SELECT
		select_was_down = false;
		select_used = false;
		quit_since = 0;
		stats_combo_down = false;

		if(pd.BTN_CROSS) s->buttons |= CHIAKI_CONTROLLER_BUTTON_CROSS;
		if(pd.BTN_CIRCLE) s->buttons |= CHIAKI_CONTROLLER_BUTTON_MOON;
		if(pd.BTN_L1) s->buttons |= CHIAKI_CONTROLLER_BUTTON_L1;
		if(pd.BTN_R1) s->buttons |= CHIAKI_CONTROLLER_BUTTON_R1;
		if(pd.BTN_START) s->buttons |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
	}
	if(now < share_until)
		s->buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;

	return action;
}

// --- menu navigation -----------------------------------------------------------

#define REPEAT_DELAY_US (400 * 1000)
#define REPEAT_RATE_US  (100 * 1000)

static uint32_t read_menu_buttons(void)
{
	int port = find_port();
	if(port < 0)
		return 0;
	padData pd;
	if(!read_pad(port, &pd))
		return 0;
	uint32_t b = 0;
	if(pd.BTN_UP) b |= MENU_EV_UP;
	if(pd.BTN_DOWN) b |= MENU_EV_DOWN;
	if(pd.BTN_LEFT) b |= MENU_EV_LEFT;
	if(pd.BTN_RIGHT) b |= MENU_EV_RIGHT;
	if(pd.BTN_CROSS) b |= MENU_EV_CROSS;
	if(pd.BTN_CIRCLE) b |= MENU_EV_CIRCLE;
	if(pd.BTN_TRIANGLE) b |= MENU_EV_TRIANGLE;
	if(pd.BTN_SQUARE) b |= MENU_EV_SQUARE;
	if(pd.BTN_START) b |= MENU_EV_START;
	if(pd.BTN_SELECT) b |= MENU_EV_SELECT;
	if(pd.BTN_L1) b |= MENU_EV_L1;
	if(pd.BTN_R1) b |= MENU_EV_R1;
	// The left stick also navigates.
	if(pd.ANA_L_V < 40) b |= MENU_EV_UP;
	if(pd.ANA_L_V > 215) b |= MENU_EV_DOWN;
	if(pd.ANA_L_H < 40) b |= MENU_EV_LEFT;
	if(pd.ANA_L_H > 215) b |= MENU_EV_RIGHT;
	return b;
}

static uint32_t menu_prev;

uint32_t input_menu_events(void)
{
	uint32_t prev = menu_prev;
	static int64_t repeat_at;
	if(!initialized)
		return 0;
	uint32_t now_b = read_menu_buttons();
	int64_t now = sysGetSystemTime();
	uint32_t ev = now_b & ~prev;
	const uint32_t dirs = MENU_EV_UP | MENU_EV_DOWN | MENU_EV_LEFT | MENU_EV_RIGHT;
	if(ev & dirs)
		repeat_at = now + REPEAT_DELAY_US;
	else if((now_b & dirs) && now >= repeat_at)
	{
		ev |= now_b & dirs;
		repeat_at = now + REPEAT_RATE_US;
	}
	menu_prev = now_b;
	return ev;
}

void input_session_reset(void)
{
	select_used = false;
	select_was_down = false;
	share_until = 0;
	quit_since = 0;
	combos_armed = false;
}

void input_menu_reset(void)
{
	// Buttons still held when the menu comes back (e.g. SELECT + CIRCLE that ended
	// the stream) must not count as new presses.
	menu_prev = initialized ? read_menu_buttons() : 0;
}
