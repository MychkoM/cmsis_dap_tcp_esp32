/* SPDX-License-Identifier: Apache-2.0 */
#include "ina219_history.h"
#include <string.h>

bool ina219_history_set_interval(struct ina219_history *h, uint32_t interval_ms,
                                uint64_t now_ms)
{
    if (interval_ms < INA219_INTERVAL_MIN_MS || interval_ms > INA219_INTERVAL_MAX_MS)
        return false;
    h->interval_ms = interval_ms;
    h->start_ms = now_ms;
    h->samples = h->errors = 0;
    memset(&h->sum, 0, sizeof(h->sum));
    return true;
}

void ina219_history_advance(struct ina219_history *h, uint64_t now_ms)
{
    if (!h->interval_ms)
        return;
    while (now_ms >= h->start_ms + h->interval_ms) {
        struct ina219_average *r = &h->records[h->next];
        *r = (struct ina219_average) {
            .sequence = ++h->sequence,
            .timestamp_ms = now_ms,
            .start_ms = h->start_ms,
            .end_ms = h->start_ms + h->interval_ms,
            .interval_ms = h->interval_ms,
            .samples = h->samples,
            .errors = h->errors,
        };
        if (h->samples) {
            r->mean.voltage_v = h->sum.voltage_v / h->samples;
            r->mean.shunt_mv = h->sum.shunt_mv / h->samples;
            r->mean.current_ma = h->sum.current_ma / h->samples;
            r->mean.power_mw = h->sum.power_mw / h->samples;
        }
        h->next = (h->next + 1) % INA219_HISTORY_CAPACITY;
        if (h->count < INA219_HISTORY_CAPACITY)
            ++h->count;
        h->start_ms = r->end_ms;
        h->samples = h->errors = 0;
        memset(&h->sum, 0, sizeof(h->sum));
    }
}

void ina219_history_add(struct ina219_history *h, uint64_t now_ms,
                       const struct ina219_sample *sample)
{
    ina219_history_advance(h, now_ms);
    if (!sample) {
        ++h->errors;
        return;
    }
    ++h->samples;
    h->sum.voltage_v += sample->voltage_v;
    h->sum.shunt_mv += sample->shunt_mv;
    h->sum.current_ma += sample->current_ma;
    h->sum.power_mw += sample->power_mw;
}

size_t ina219_history_copy(const struct ina219_history *h,
                          struct ina219_average *out, size_t limit)
{
    size_t count = h->count < limit ? h->count : limit;
    size_t start = (h->next + INA219_HISTORY_CAPACITY - count) % INA219_HISTORY_CAPACITY;
    for (size_t i = 0; i < count; ++i)
        out[i] = h->records[(start + i) % INA219_HISTORY_CAPACITY];
    return count;
}
