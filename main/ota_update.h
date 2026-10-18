/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"

/* Asynchronous download; neither URL nor console FILE is borrowed. */
esp_err_t ota_update_start(const char *url, const char *sha256);
void ota_update_print_status(FILE *out);
/* Called after services start; pending images must survive a 10s startup trial. */
void ota_update_confirm_boot(bool startup_ok);
