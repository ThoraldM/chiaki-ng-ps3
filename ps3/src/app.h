// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Helpers implemented in main.c and shared with the menu shell.
#ifndef CHIAKI_PS3_APP_H
#define CHIAKI_PS3_APP_H

#include <chiaki/log.h>
#include <stdbool.h>
#include <stddef.h>

extern ChiakiLog app_log_ctx;
extern volatile bool app_exit;  // set when the XMB asks the app to quit

void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// XMB message dialogs. The *_wait/_yes_no variants keep the display running until answered.
void dialog_status(const char *text);
void dialog_close(void);
void dialog_wait_ok(bool error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool dialog_yes_no(const char *text);

// The PS3's on-screen keyboard (ASCII). Returns 1 with the text in out, 0 if cancelled or empty,
// -1 if the keyboard could not be opened.
int osk_input(const char *title, const char *initial, char *out, size_t out_size);

// One frame of system callbacks + display; returns false once the app should exit.
bool keep_display_alive(void);

#endif
