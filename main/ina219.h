/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdio.h>
#include "esp_err.h"

/* One triggered conversion, serialized across USB/TCP console clients. */
esp_err_t ina219_print_sample(FILE *out);
esp_err_t ina219_scan(FILE *out);
