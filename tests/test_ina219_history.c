#include "ina219_history.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static struct ina219_history h;
static struct ina219_average out[INA219_HISTORY_CAPACITY];

int main(void)
{
    assert(!ina219_history_set_interval(&h, 0, 0));
    assert(!ina219_history_set_interval(&h, 3600001, 0));
    assert(ina219_history_set_interval(&h, 1000, 100));
    struct ina219_sample a = {2, -1, -10, -20};
    struct ina219_sample b = {4, 3, 30, 120};
    ina219_history_add(&h, 200, &a);
    ina219_history_add(&h, 300, NULL);
    ina219_history_add(&h, 1099, &b);
    assert(ina219_history_copy(&h, out, 200) == 0);
    /* Boundary sample belongs to the next interval, not the closed one. */
    ina219_history_add(&h, 1100, &a);
    assert(ina219_history_copy(&h, out, 200) == 1);
    assert(out[0].start_ms == 100 && out[0].end_ms == 1100);
    assert(out[0].timestamp_ms == 1100 && out[0].samples == 2 && out[0].errors == 1);
    assert(fabs(out[0].mean.current_ma - 10) < 1e-9);
    /* Mean power is mean(V*I), not product of the means (3*10). */
    assert(fabs(out[0].mean.power_mw - 50) < 1e-9);
    ina219_history_advance(&h, 4100);
    assert(ina219_history_copy(&h, out, 200) == 4);
    assert(out[1].samples == 1 && out[2].samples == 0 && out[3].samples == 0);
    assert(out[3].timestamp_ms == 4100);
    /* Changing interval discards only the incomplete window; history stays. */
    ina219_history_add(&h, 4200, &b);
    assert(ina219_history_set_interval(&h, 100, 4250));
    for (unsigned i = 0; i < 205; ++i) {
        ina219_history_add(&h, 4250 + i * 100, &a);
        ina219_history_advance(&h, 4350 + i * 100);
    }
    assert(ina219_history_copy(&h, out, 200) == 200);
    assert(out[0].sequence == 10 && out[199].sequence == 209);
    for (unsigned i = 1; i < 200; ++i)
        assert(out[i].start_ms == out[i-1].end_ms && out[i].samples == 1);
    assert(ina219_history_copy(&h, out, 2) == 2);
    assert(out[0].sequence == 208 && out[1].sequence == 209);
    assert(ina219_history_copy(&h, out, 2) == 2); /* Non-destructive read. */
    puts("INA219 averaging, boundaries, errors, interval changes and ring wrap: PASS");
}
