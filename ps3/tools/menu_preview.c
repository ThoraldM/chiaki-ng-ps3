// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Renders the PS3 start menu on a PC (same drawing code) to PPM images.
// Build: see ps3/tools/menu_preview.sh

#include "../src/menu.h"
#include "../src/input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t pixels[UI_W * UI_H];

static void save(Menu *m, const char *path)
{
	Canvas c = { pixels, UI_W, UI_H };
	menu_draw(m, &c);
	FILE *f = fopen(path, "wb");
	fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
	for(int i = 0; i < UI_W * UI_H; i++)
	{
		uint8_t rgb[3] = { (uint8_t)(pixels[i] >> 16), (uint8_t)(pixels[i] >> 8), (uint8_t)pixels[i] };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	printf("wrote %s\n", path);
}

static void press(Menu *m, uint32_t ev)
{
	MenuAction a = menu_handle(m, ev);
	if(a.type != MENU_ACT_NONE)
		printf("  action %d host %d pin %08u %s\n", a.type, a.host, (unsigned)a.pin, a.text);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	char path[512];
	static Config cfg;
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.path, sizeof(cfg.path), "/dev_hdd0/game/CHIAKIPS3/USRDIR/chiaki.cfg");
	cfg.resolution = 720;
	cfg.fps = 60;
	cfg.pacing = 1;
	cfg.low_latency = true;
	cfg.vibration = true;

	static Menu m;

	// 1. empty
	menu_init(&m, &cfg);
	snprintf(m.ps3_ip, sizeof(m.ps3_ip), "192.168.1.10");
	snprintf(path, sizeof(path), "%s/1_empty.ppm", dir); save(&m, path);

	// 2. registered console + a discovered one
	ConfigHost *h = config_add_host(&cfg);
	snprintf(h->name, sizeof(h->name), "PS4-123");
	snprintf(h->host, sizeof(h->host), "192.168.1.20");
	snprintf(h->host_id, sizeof(h->host_id), "0123456789AB");
	h->have_regist_key = h->have_morning = true;
	cfg.have_account_id = true;
	DiscoveredHost d[2] = {
		{ "PS4-123", "192.168.1.20", "0123456789AB", false, false },
		{ "PS5-123", "192.168.1.21", "001122334455", true, true },
	};
	menu_set_discovered(&m, d, 2);
	snprintf(path, sizeof(path), "%s/2_hosts.ppm", dir); save(&m, path);

	// 3. second console selected (not registered, rest mode)
	press(&m, MENU_EV_DOWN);
	menu_set_note(&m, "");
	snprintf(path, sizeof(path), "%s/3_hosts_unregistered.ppm", dir); save(&m, path);

	// 4. options for the first console
	press(&m, MENU_EV_UP);
	press(&m, MENU_EV_TRIANGLE);
	snprintf(path, sizeof(path), "%s/4_options.ppm", dir); save(&m, path);
	press(&m, MENU_EV_CIRCLE);

	// 5. settings
	press(&m, MENU_EV_START);
	press(&m, MENU_EV_DOWN);
	press(&m, MENU_EV_DOWN);
	press(&m, MENU_EV_RIGHT);
	press(&m, MENU_EV_RIGHT);
	snprintf(path, sizeof(path), "%s/5_settings.ppm", dir); save(&m, path);
	// 5b. account id row (set) and import row
	config_parse_account_id("AQIDBAUGBwg=", cfg.psn_account_id);
	for(int i = 0; i < 6; i++) press(&m, MENU_EV_DOWN);
	snprintf(path, sizeof(path), "%s/5b_settings_account.ppm", dir); save(&m, path);
	press(&m, MENU_EV_CROSS); // -> ACCOUNT action
	press(&m, MENU_EV_DOWN);
	snprintf(path, sizeof(path), "%s/5c_settings_import.ppm", dir); save(&m, path);
	press(&m, MENU_EV_CROSS); // -> IMPORT action
	press(&m, MENU_EV_CIRCLE);

	// overlay
	{
		static uint32_t ov[640 * 64];
		Canvas oc = { ov, 640, 64 };
		ui_fill(&oc, 0, 0, 640, 64, 0x0b1220);
		ui_fill(&oc, 0, 62, 640, 2, 0x6fb6ff);
		ui_text_fit(&oc, &font_small, 12, 5, "1280x720   in 60.0 fps   shown 59.2 fps   skipped 1   lost 0", 0xf2f4f8, 616);
		ui_text_fit(&oc, &font_small, 12, 32, "decoder 17 ms   audio buffer 62 ms   9.8 Mbit/s", 0x9aa6b8, 616);
		snprintf(path, sizeof(path), "%s/9_overlay.ppm", dir);
		FILE *f = fopen(path, "wb");
		fprintf(f, "P6\n640 64\n255\n");
		for(int i = 0; i < 640 * 64; i++)
		{
			uint8_t rgb[3] = { (uint8_t)(ov[i] >> 16), (uint8_t)(ov[i] >> 8), (uint8_t)ov[i] };
			fwrite(rgb, 1, 3, f);
		}
		fclose(f);
	}

	// 6. PIN entry for the unregistered console
	press(&m, MENU_EV_DOWN);
	press(&m, MENU_EV_CROSS);
	press(&m, MENU_EV_UP); press(&m, MENU_EV_UP); press(&m, MENU_EV_RIGHT); press(&m, MENU_EV_DOWN); press(&m, MENU_EV_RIGHT);
	snprintf(path, sizeof(path), "%s/6_pin.ppm", dir); save(&m, path);
	press(&m, MENU_EV_CROSS); // -> REGISTER action

	// 7. add by IP
	press(&m, MENU_EV_SQUARE);
	press(&m, MENU_EV_RIGHT); press(&m, MENU_EV_UP); press(&m, MENU_EV_UP);
	snprintf(path, sizeof(path), "%s/7_ip.ppm", dir); save(&m, path);
	press(&m, MENU_EV_CROSS); // -> SAVE action, console added
	snprintf(path, sizeof(path), "%s/8_added.ppm", dir); save(&m, path);

	printf("config now has %d hosts, last host=%s name=%s\n", cfg.host_count,
			cfg.hosts[cfg.host_count - 1].host, cfg.hosts[cfg.host_count - 1].name);
	return 0;
}
