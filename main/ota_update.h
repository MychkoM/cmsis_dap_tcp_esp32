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

/* Synchronous push: read exactly image_size raw bytes from in_fd (a TCP
 * console socket positioned right after the command line), flash them to
 * the next OTA slot, and reboot. Handshake and verdict go to out. */
esp_err_t ota_push_run(FILE *out, int in_fd, size_t image_size, const char *sha256_hex);
