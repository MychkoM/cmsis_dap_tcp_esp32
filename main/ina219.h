/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include "esp_err.h"

/* One triggered conversion, serialized across USB/TCP console clients. */
esp_err_t ina219_print_sample(FILE *out);
esp_err_t ina219_scan(FILE *out);

/* Tare the zero-current shunt offset (persisted in NVS); call with the
 * shunt path open. Set the fitted shunt resistance at runtime (mOhm). */
esp_err_t ina219_zero(FILE *out);
esp_err_t ina219_set_shunt(uint32_t milliohms, FILE *out);

/* Starts continuous 20Hz sampling and a 200-record, RAM-only average history. */
esp_err_t ina219_monitor_start(void);
esp_err_t ina219_set_interval(uint32_t interval_ms);
esp_err_t ina219_print_interval(FILE *out);
esp_err_t ina219_print_history(FILE *out, unsigned limit);
