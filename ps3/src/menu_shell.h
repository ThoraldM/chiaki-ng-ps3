// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Runs the start menu on the PS3 (input, live discovery, registration, saving).
#ifndef CHIAKI_PS3_MENU_SHELL_H
#define CHIAKI_PS3_MENU_SHELL_H

#include "config.h"

// Returns the config index of the console to connect to, or -1 to quit.
int menu_shell_run(Config *cfg);

#endif
