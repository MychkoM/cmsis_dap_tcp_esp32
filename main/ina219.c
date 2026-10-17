/* SPDX-License-Identifier: Apache-2.0 */
#include "sdkconfig.h"
#include <stdint.h>
#include "driver/i2c_master.h"
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
        fprintf(out, "INA219 error: busy\n");
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

esp_err_t ina219_print_sample(FILE *out)
{
    if (!take_lock()) {
        fprintf(out, "INA219 error: busy\n");
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
        double voltage = (bus_raw >> 3) * 0.004;
        double shunt_mv = (int16_t)shunt_raw * 0.01;
        double current_ma = shunt_mv * 1000.0 / CONFIG_ESP_INA219_SHUNT_MILLIOHMS;
        fprintf(out, "{\"sensor\":\"INA219\",\"address\":%d,\"bus_voltage_v\":%.3f,"
                "\"shunt_voltage_mv\":%.3f,\"current_ma\":%.3f,\"power_mw\":%.3f,"
                "\"shunt_milliohms\":%d}\n",
                CONFIG_ESP_INA219_ADDRESS, voltage, shunt_mv, current_ma,
                voltage * current_ma, CONFIG_ESP_INA219_SHUNT_MILLIOHMS);
    } else {
        fprintf(out, "INA219 error: %s (stage=%s config=0x%04x address=0x%02x SDA=%d SCL=%d)\n",
                esp_err_to_name(err), stage, config, CONFIG_ESP_INA219_ADDRESS,
                CONFIG_ESP_INA219_SDA_PIN, CONFIG_ESP_INA219_SCL_PIN);
    }
    xSemaphoreGive(lock);
    return err;
}
