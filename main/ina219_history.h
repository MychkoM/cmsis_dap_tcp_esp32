/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INA219_HISTORY_CAPACITY 200
#define INA219_INTERVAL_MIN_MS 100
#define INA219_INTERVAL_MAX_MS 3600000

struct ina219_sample {
    double voltage_v, shunt_mv, current_ma, power_mw;
};

struct ina219_average {
    uint64_t sequence, timestamp_ms, start_ms, end_ms;
    uint32_t interval_ms, samples, errors;
    struct ina219_sample mean;
};

/* No internal locking: the caller serializes access. Time is monotonic uptime. */
struct ina219_history {
    struct ina219_average records[INA219_HISTORY_CAPACITY];
    size_t next, count;
    uint64_t sequence, start_ms;
    uint32_t interval_ms, samples, errors;
    struct ina219_sample sum;
};

bool ina219_history_set_interval(struct ina219_history *h, uint32_t interval_ms,
                                uint64_t now_ms);
void ina219_history_advance(struct ina219_history *h, uint64_t now_ms);
void ina219_history_add(struct ina219_history *h, uint64_t now_ms,
                       const struct ina219_sample *sample);
size_t ina219_history_copy(const struct ina219_history *h,
                          struct ina219_average *out, size_t limit);
