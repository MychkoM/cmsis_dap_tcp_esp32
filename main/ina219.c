/* SPDX-License-Identifier: Apache-2.0 */
#include "sdkconfig.h"
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_timer.h"
#include "ina219_history.h"
#include "driver/i2c_master.h"
#include "nvs.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ina219.h"

/* TI INA219: https://www.ti.com/lit/ds/symlink/ina219.pdf
 * Bus voltage LSB=4mV, signed shunt voltage LSB=10uV.
 * Calculate signed current from the configured physical shunt resistance;
 * the calibration/current/power registers are deliberately not used.
 */
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t device;
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;

/* Runtime settings persisted in NVS (namespace "ina219"):
 *   zero_uv    - measured open-circuit shunt offset (input offset), uV
 *   shunt_mohm - fitted shunt resistance, milliohms
 * Defaults come from sdkconfig; the console can override both live. */
static int32_t zero_offset_uv;
static uint32_t shunt_mohm = CONFIG_ESP_INA219_SHUNT_MILLIOHMS;
static bool settings_loaded;

static void load_settings(void)
{
    if (settings_loaded)
        return;
    settings_loaded = true;
    nvs_handle_t h;
    if (nvs_open("ina219", NVS_READONLY, &h) == ESP_OK) {
        int64_t v;
        if (nvs_get_i64(h, "zero_uv", &v) == ESP_OK)
            zero_offset_uv = (int32_t)v;
        if (nvs_get_i64(h, "shunt_mohm", &v) == ESP_OK && v >= 1 && v <= 100000)
            shunt_mohm = (uint32_t)v;
        nvs_close(h);
    }
}

static esp_err_t save_setting(const char *key, int64_t value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("ina219", NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_i64(h, key, value);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t write_register(uint8_t reg, uint16_t value)
{
    uint8_t bytes[] = {reg, value >> 8, value & 0xff};
    return i2c_master_transmit(device, bytes, sizeof(bytes), 100);
}

static esp_err_t read_register(uint8_t reg, uint16_t *value)
{
    uint8_t bytes[2];
    esp_err_t err = i2c_master_transmit_receive(device, &reg, 1,
                                               bytes, sizeof(bytes), 100);
    if (err == ESP_OK)
        *value = ((uint16_t)bytes[0] << 8) | bytes[1];
    return err;
}

static esp_err_t init_device(void)
{
    if (device)
        return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_ESP_INA219_SDA_PIN,
        .scl_io_num = CONFIG_ESP_INA219_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK)
        return err;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_ESP_INA219_ADDRESS,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &device);
    if (err != ESP_OK) {
        i2c_del_master_bus(bus);
        bus = NULL;
    }
    return err;
}

static bool take_lock(void)
{
    portENTER_CRITICAL(&init_mux);
    if (!lock)
        lock = xSemaphoreCreateMutexStatic(&lock_storage);
    portEXIT_CRITICAL(&init_mux);
    return lock && xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) == pdTRUE;
}

esp_err_t ina219_scan(FILE *out)
{
    if (!take_lock()) {
        if (out) fprintf(out, "INA219 error: busy\n");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = init_device();
    if (err == ESP_OK) {
        fprintf(out, "I2C scan SDA=%d SCL=%d:", CONFIG_ESP_INA219_SDA_PIN,
                CONFIG_ESP_INA219_SCL_PIN);
        unsigned found = 0;
        for (unsigned addr = 0x08; addr <= 0x77; ++addr) {
            if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
                fprintf(out, " 0x%02x", addr);
                ++found;
            }
        }
        fprintf(out, " (%u devices)\n", found);
    } else {
        fprintf(out, "INA219 scan error: %s\n", esp_err_to_name(err));
    }
    xSemaphoreGive(lock);
    return err;
}

static esp_err_t read_sample_impl(struct ina219_sample *sample, FILE *out,
                                  bool corrected)
{
    if (!take_lock()) {
        if (out) fprintf(out, "INA219 error: busy\n");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    const char *stage = "init";
    err = init_device();
    uint16_t bus_raw = 0, shunt_raw = 0, config = 0;
    /* 32V bus range, +/-320mV shunt, 12-bit bus and shunt, triggered. */
    if (err == ESP_OK) {
        stage = "trigger";
        err = write_register(0x00, 0x399b);
    }
    if (err == ESP_OK) {
        stage = "config";
        err = read_register(0x00, &config);
    }
    if (err == ESP_OK && config != 0x399b)
        err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) {
        stage = "conversion";
        for (int i = 0; i < 20; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
            err = read_register(0x02, &bus_raw);
            if (err != ESP_OK || (bus_raw & 2))
                break;
        }
        if (err == ESP_OK && !(bus_raw & 2))
            err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        stage = "shunt";
        err = read_register(0x01, &shunt_raw);
    }
    if (err == ESP_OK && (bus_raw & 1))
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        load_settings();
        double voltage = (bus_raw >> 3) * 0.004;
        double shunt_mv = (int16_t)shunt_raw * 0.01;
        if (corrected)
            shunt_mv -= zero_offset_uv / 1000.0;
        double current_ma = shunt_mv * 1000.0 / shunt_mohm;
        *sample = (struct ina219_sample) {voltage, shunt_mv, current_ma,
                                         voltage * current_ma};
    } else if (out) {
        fprintf(out, "INA219 error: %s (stage=%s config=0x%04x address=0x%02x SDA=%d SCL=%d)\n",
                esp_err_to_name(err), stage, config, CONFIG_ESP_INA219_ADDRESS,
                CONFIG_ESP_INA219_SDA_PIN, CONFIG_ESP_INA219_SCL_PIN);
    }
    xSemaphoreGive(lock);
    return err;
}

static esp_err_t read_sample(struct ina219_sample *sample, FILE *out)
{
    return read_sample_impl(sample, out, true);
}

esp_err_t ina219_print_sample(FILE *out)
{
    struct ina219_sample s;
    esp_err_t err = read_sample(&s, out);
    if (err == ESP_OK)
        fprintf(out, "{\"sensor\":\"INA219\",\"address\":%d,\"bus_voltage_v\":%.3f,"
                "\"shunt_voltage_mv\":%.3f,\"current_ma\":%.3f,\"power_mw\":%.3f,"
                "\"shunt_milliohms\":%u,\"zero_offset_uv\":%d}\n",
                CONFIG_ESP_INA219_ADDRESS, s.voltage_v, s.shunt_mv, s.current_ma,
                s.power_mw, (unsigned)shunt_mohm, (int)zero_offset_uv);
    return err;
}

esp_err_t ina219_zero(FILE *out)
{
    /* Tare: average the open-circuit shunt voltage and store it as the
     * zero-current offset. Run while no current flows through the shunt. */
    const unsigned count = 20;
    double sum = 0;
    unsigned good = 0;
    esp_err_t err = ESP_OK;
    for (unsigned i = 0; i < count; ++i) {
        struct ina219_sample s;
        err = read_sample_impl(&s, NULL, false);
        if (err == ESP_OK) {
            sum += s.shunt_mv;
            ++good;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!good) {
        if (out) fprintf(out, "INA219 zero error: %s\n", esp_err_to_name(err));
        return err ? err : ESP_FAIL;
    }
    double mean_mv = sum / good;
    int32_t offset_uv = (int32_t)llround(mean_mv * 1000.0);
    if (offset_uv > 5000 || offset_uv < -5000) {
        if (out) fprintf(out, "{\"type\":\"ina219_zero\",\"error\":\"offset out of range\","
                "\"mean_raw_mv\":%.3f}\n", mean_mv);
        return ESP_ERR_INVALID_STATE;
    }
    err = save_setting("zero_uv", offset_uv);
    if (err != ESP_OK) {
        if (out) fprintf(out, "INA219 zero save error: %s\n", esp_err_to_name(err));
        return err;
    }
    if (!take_lock())
        return ESP_ERR_TIMEOUT;
    load_settings();
    zero_offset_uv = offset_uv;
    xSemaphoreGive(lock);
    if (out) fprintf(out, "{\"type\":\"ina219_zero\",\"samples\":%u,\"mean_raw_mv\":%.3f,"
            "\"offset_uv\":%d,\"shunt_mohm\":%u}\n",
            good, mean_mv, (int)offset_uv, (unsigned)shunt_mohm);
    return ESP_OK;
}

esp_err_t ina219_set_shunt(uint32_t milliohms, FILE *out)
{
    if (milliohms < 1 || milliohms > 100000) {
        if (out) fprintf(out, "INA219 shunt error: out of range\n");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_setting("shunt_mohm", (int64_t)milliohms);
    if (err != ESP_OK) {
        if (out) fprintf(out, "INA219 shunt save error: %s\n", esp_err_to_name(err));
        return err;
    }
    if (!take_lock())
        return ESP_ERR_TIMEOUT;
    load_settings();
    shunt_mohm = milliohms;
    xSemaphoreGive(lock);
    if (out) fprintf(out, "{\"type\":\"ina219_shunt\",\"shunt_mohm\":%u}\n",
                     (unsigned)milliohms);
    return ESP_OK;
}

static struct ina219_history history;
static StaticSemaphore_t history_lock_storage;
static SemaphoreHandle_t history_lock;
static uint32_t interval_generation;
static bool monitor_running;

static uint64_t uptime_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static void monitor_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        xSemaphoreTake(history_lock, portMAX_DELAY);
        uint32_t generation = interval_generation;
        ina219_history_advance(&history, uptime_ms());
        xSemaphoreGive(history_lock);

        struct ina219_sample s;
        esp_err_t err = read_sample(&s, NULL);
        xSemaphoreTake(history_lock, portMAX_DELAY);
        /* A conversion already in flight when the interval changes is dropped. */
        if (generation == interval_generation)
            ina219_history_add(&history, uptime_ms(), err == ESP_OK ? &s : NULL);
        xSemaphoreGive(history_lock);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(50));
    }
}

esp_err_t ina219_monitor_start(void)
{
    /* Called once at startup, before USB/TCP consoles become available. */
    if (monitor_running)
        return ESP_OK;
    history_lock = xSemaphoreCreateMutexStatic(&history_lock_storage);
    if (!history_lock)
        return ESP_ERR_NO_MEM;
    ina219_history_set_interval(&history, 1000, uptime_ms());
    if (xTaskCreate(monitor_task, "ina219_monitor", 4096, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    monitor_running = true;
    return ESP_OK;
}

esp_err_t ina219_set_interval(uint32_t interval_ms)
{
    if (!monitor_running)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(history_lock, portMAX_DELAY);
    bool valid = ina219_history_set_interval(&history, interval_ms, uptime_ms());
    if (valid)
        ++interval_generation;
    xSemaphoreGive(history_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t ina219_print_interval(FILE *out)
{
    if (!monitor_running) {
        fprintf(out, "INA219 monitor is not running\n");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(history_lock, portMAX_DELAY);
    uint32_t interval_ms = history.interval_ms;
    size_t count = history.count;
    xSemaphoreGive(history_lock);
    fprintf(out, "{\"type\":\"ina219_interval\",\"interval_ms\":%" PRIu32
            ",\"sample_period_ms\":50,\"count\":%u,\"capacity\":200,"
            "\"shunt_mohm\":%u,\"zero_offset_uv\":%d,\"timebase\":\"uptime_ms\"}\n",
            interval_ms, (unsigned)count, (unsigned)shunt_mohm,
            (int)zero_offset_uv);
    return ESP_OK;
}

esp_err_t ina219_print_history(FILE *out, unsigned limit)
{
    if (!monitor_running) {
        fprintf(out, "INA219 monitor is not running\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (!limit || limit > INA219_HISTORY_CAPACITY)
        return ESP_ERR_INVALID_ARG;
    /* Snapshot under the history lock; never hold a lock while writing TCP. */
    struct ina219_average *rows = calloc(limit, sizeof(*rows));
    if (!rows) {
        fprintf(out, "INA219 history error: out of memory\n");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(history_lock, portMAX_DELAY);
    size_t count = ina219_history_copy(&history, rows, limit);
    xSemaphoreGive(history_lock);
    fprintf(out, "{\"type\":\"ina219_history\",\"count\":%u,\"capacity\":200,"
            "\"timebase\":\"uptime_ms\"}\n", (unsigned)count);
    for (size_t i = 0; i < count; ++i) {
        const struct ina219_average *r = &rows[i];
        fprintf(out, "{\"type\":\"ina219_average\",\"sequence\":%" PRIu64
                ",\"timestamp_ms\":%" PRIu64 ",\"start_ms\":%" PRIu64
                ",\"end_ms\":%" PRIu64 ",\"interval_ms\":%" PRIu32
                ",\"samples\":%" PRIu32 ",\"errors\":%" PRIu32 ",",
                r->sequence, r->timestamp_ms, r->start_ms, r->end_ms,
                r->interval_ms, r->samples, r->errors);
        if (r->samples)
            fprintf(out, "\"bus_voltage_v\":%.3f,\"shunt_voltage_mv\":%.3f,"
                    "\"current_ma\":%.3f,\"power_mw\":%.3f}\n",
                    r->mean.voltage_v, r->mean.shunt_mv,
                    r->mean.current_ma, r->mean.power_mw);
        else
            fprintf(out, "\"bus_voltage_v\":null,\"shunt_voltage_mv\":null,"
                    "\"current_ma\":null,\"power_mw\":null}\n");
    }
    free(rows);
    return ESP_OK;
}
