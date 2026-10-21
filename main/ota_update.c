/* SPDX-License-Identifier: Apache-2.0 */
#include "ota_update.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "psa/crypto.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define OTA_URL_MAX 180
#define OTA_BUFFER_SIZE 4096

struct ota_job {
    char url[OTA_URL_MAX + 1];
    uint8_t expected_hash[32];
};

static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool active;
static const char *phase = "idle";
static esp_err_t last_error;
static uint32_t downloaded, total;

static void set_status(const char *state, esp_err_t error, bool busy)
{
    portENTER_CRITICAL(&state_mux);
    phase = state;
    last_error = error;
    active = busy;
    portEXIT_CRITICAL(&state_mux);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static esp_err_t read_exact(esp_http_client_handle_t client, void *data, size_t length)
{
    uint8_t *p = data;
    while (length) {
        int n = esp_http_client_read(client, (char *)p, length);
        if (n <= 0)
            return ESP_FAIL;
        p += n;
        length -= n;
    }
    return ESP_OK;
}

static void update_task(void *arg)
{
    struct ota_job *job = arg;
    esp_err_t err = ESP_FAIL;
    esp_ota_handle_t handle = 0;
    bool ota_open = false;
    esp_http_client_handle_t client = NULL;
    uint8_t *buffer = malloc(OTA_BUFFER_SIZE);
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target || target == esp_ota_get_running_partition()) {
        err = ESP_ERR_NOT_FOUND; goto done;
    }
    if (!buffer) { err = ESP_ERR_NO_MEM; goto done; }

    esp_http_client_config_t cfg = {
        .url = job->url,
        .timeout_ms = 10000,
        .buffer_size = OTA_BUFFER_SIZE,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    client = esp_http_client_init(&cfg);
    if (!client) { err = ESP_ERR_NO_MEM; goto done; }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) goto done;
    int64_t length = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 || length <= 0 ||
            length > target->size) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    portENTER_CRITICAL(&state_mux);
    total = (uint32_t)length;
    portEXIT_CRITICAL(&state_mux);

    /* Assemble the prefix across arbitrary network read boundaries. */
    const size_t prefix_size = sizeof(esp_image_header_t) +
                              sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (length < prefix_size) { err = ESP_ERR_INVALID_SIZE; goto done; }
    err = read_exact(client, buffer, prefix_size);
    if (err != ESP_OK) goto done;
    esp_app_desc_t desc;
    memcpy(&desc, buffer + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           sizeof(desc));
    if (buffer[0] != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(desc.project_name, esp_app_get_description()->project_name,
                sizeof(desc.project_name)) != 0) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    err = esp_ota_begin(target, (size_t)length, &handle);
    if (err != ESP_OK) goto done;
    ota_open = true;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) { err = ESP_FAIL; goto done; }
    size_t received = 0, chunk = prefix_size;
    for (;;) {
        if (psa_hash_update(&sha, buffer, chunk) != PSA_SUCCESS) { err = ESP_FAIL; goto done; }
        err = esp_ota_write(handle, buffer, chunk);
        if (err != ESP_OK) goto done;
        received += chunk;
        portENTER_CRITICAL(&state_mux);
        downloaded = received;
        portEXIT_CRITICAL(&state_mux);
        if (received == (size_t)length) break;
        size_t remaining = (size_t)length - received;
        int n = esp_http_client_read(client, (char *)buffer,
                                    remaining < OTA_BUFFER_SIZE ? remaining : OTA_BUFFER_SIZE);
        if (n <= 0) { err = ESP_FAIL; goto done; }
        chunk = n;
        /* Yield to Wi-Fi, consoles and the INA219 sampler during flash writes. */
        vTaskDelay(1);
    }
    if (!esp_http_client_is_complete_data_received(client)) { err = ESP_FAIL; goto done; }
    uint8_t digest[32];
    size_t digest_length = 0;
    if (psa_hash_finish(&sha, digest, sizeof(digest), &digest_length) != PSA_SUCCESS ||
        digest_length != sizeof(digest)) { err = ESP_FAIL; goto done; }
    if (memcmp(digest, job->expected_hash, sizeof(digest)) != 0) {
        err = ESP_ERR_INVALID_CRC;
        goto done;
    }
    err = esp_ota_end(handle);  /* Verifies image format/chip/revision/checksum. */
    ota_open = false;          /* esp_ota_end frees its handle even on failure. */
    if (err != ESP_OK) goto done;
    err = esp_ota_set_boot_partition(target); /* Only commit after all checks pass. */

done:
    if (ota_open) esp_ota_abort(handle);
    if (client) esp_http_client_cleanup(client);
    psa_hash_abort(&sha);
    free(buffer);
    free(job);
    if (err == ESP_OK) {
        set_status("rebooting", ESP_OK, true);
        printf("OTA: verified image, rebooting into %s\n", target->label);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    printf("OTA failed: %s; current boot partition unchanged\n", esp_err_to_name(err));
    set_status("failed", err, false);
    vTaskDelete(NULL);
}

esp_err_t ota_update_start(const char *url, const char *hash)
{
    if (!url || !hash || strlen(url) > OTA_URL_MAX || strlen(hash) != 64 ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0))
        return ESP_ERR_INVALID_ARG;
    struct ota_job *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < 32; ++i) {
        int high = hex_digit(hash[2*i]), low = hex_digit(hash[2*i + 1]);
        if (high < 0 || low < 0) { free(job); return ESP_ERR_INVALID_ARG; }
        job->expected_hash[i] = (high << 4) | low;
    }
    strcpy(job->url, url);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        free(job);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&state_mux);
    bool busy = active;
    if (!busy) {
        active = true;
        phase = "downloading";
        last_error = ESP_OK;
        downloaded = total = 0;
    }
    portEXIT_CRITICAL(&state_mux);
    if (busy) { free(job); return ESP_ERR_INVALID_STATE; }
    if (xTaskCreate(update_task, "ota_update", 8192, job, 4, NULL) != pdPASS) {
        free(job);
        set_status("failed", ESP_ERR_NO_MEM, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- Synchronous console push ----------------------------------------
 * The TCP console's linenoise reads the command line one byte at a time
 * directly from the socket, so once the 'ota push' line is consumed the
 * stream position is exactly at the first payload byte. Read exactly
 * image_size bytes, flash to the next OTA slot, verify the SHA-256, and
 * reboot. Runs on the console connection task; other connections can
 * watch progress via 'ota status'. */
esp_err_t ota_push_run(FILE *out, int in_fd, size_t image_size, const char *sha256_hex)
{
    uint8_t expected[32];
    if (!out || in_fd < 0 || image_size == 0 || !sha256_hex || strlen(sha256_hex) != 64)
        return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < 32; ++i) {
        int high = hex_digit(sha256_hex[2*i]), low = hex_digit(sha256_hex[2*i + 1]);
        if (high < 0 || low < 0) return ESP_ERR_INVALID_ARG;
        expected[i] = (uint8_t)((high << 4) | low);
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target || target == esp_ota_get_running_partition())
        return ESP_ERR_NOT_FOUND;
    if (image_size > target->size)
        return ESP_ERR_INVALID_SIZE;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY)
        return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&state_mux);
    bool busy = active;
    if (!busy) {
        active = true;
        phase = "push";
        last_error = ESP_OK;
        downloaded = 0;
        total = (uint32_t)image_size;
    }
    portEXIT_CRITICAL(&state_mux);
    if (busy)
        return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_FAIL;
    esp_ota_handle_t handle = 0;
    bool ota_open = false;
    uint8_t *buffer = malloc(OTA_BUFFER_SIZE);
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;

    /* Idle payload reads get 60s; a stalled sender aborts the push instead
     * of pinning the connection task forever. */
    struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
    bool had_timeout = setsockopt(in_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0;
    (void)had_timeout;

    fprintf(out, "OTA PUSH READY %u\n", (unsigned)image_size);
    fflush(out);

    const size_t prefix_size = sizeof(esp_image_header_t) +
                               sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (image_size < prefix_size) { err = ESP_ERR_INVALID_SIZE; goto push_done; }
    if (!buffer) { err = ESP_ERR_NO_MEM; goto push_done; }

    size_t got = 0;                       /* read the prefix, raw and short-read safe */
    while (got < prefix_size) {
        ssize_t n = read(in_fd, buffer + got, prefix_size - got);
        if (n <= 0) { err = ESP_ERR_TIMEOUT; goto push_done; }  /* peer gone / stalled */
        got += (size_t)n;
    }
    esp_app_desc_t desc;
    memcpy(&desc, buffer + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           sizeof(desc));
    if (buffer[0] != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(desc.project_name, esp_app_get_description()->project_name,
                sizeof(desc.project_name)) != 0) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto push_done;
    }
    err = esp_ota_begin(target, image_size, &handle);
    if (err != ESP_OK) goto push_done;
    ota_open = true;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) { err = ESP_FAIL; goto push_done; }

    size_t received = 0, chunk = prefix_size;
    for (;;) {
        if (psa_hash_update(&sha, buffer, chunk) != PSA_SUCCESS) {
            err = ESP_FAIL;
            goto push_done;
        }
        err = esp_ota_write(handle, buffer, chunk);
        if (err != ESP_OK)
            goto push_done;
        received += chunk;
        portENTER_CRITICAL(&state_mux);
        downloaded = (uint32_t)received;
        portEXIT_CRITICAL(&state_mux);
        if (received == image_size)
            break;
        size_t remaining = image_size - received;
        ssize_t n = read(in_fd, buffer,
                         remaining < OTA_BUFFER_SIZE ? remaining : OTA_BUFFER_SIZE);
        if (n <= 0) { err = ESP_ERR_TIMEOUT; goto push_done; }  /* peer gone / stalled */
        chunk = (size_t)n;
        /* Yield to Wi-Fi, other consoles and the INA219 sampler during flash writes. */
        vTaskDelay(1);
    }

    uint8_t digest[32];
    size_t digest_length = 0;
    if (psa_hash_finish(&sha, digest, sizeof(digest), &digest_length) != PSA_SUCCESS ||
        digest_length != sizeof(digest) || memcmp(digest, expected, sizeof(digest)) != 0) {
        err = ESP_ERR_INVALID_CRC;
        goto push_done;
    }
    err = esp_ota_end(handle);
    ota_open = false;
    if (err != ESP_OK) goto push_done;
    err = esp_ota_set_boot_partition(target);

push_done:
    if (ota_open) esp_ota_abort(handle);
    psa_hash_abort(&sha);
    free(buffer);
    tv.tv_sec = 0;
    setsockopt(in_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (err == ESP_OK) {
        fprintf(out, "OTA PUSH OK: verified %u bytes, rebooting\n", (unsigned)image_size);
        fflush(out);
        set_status("rebooting", ESP_OK, true);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();                     /* never returns */
    }
    fprintf(out, "OTA PUSH FAILED: %s\n", esp_err_to_name(err));
    fflush(out);
    set_status("failed", err, false);
    return err;
}

void ota_update_print_status(FILE *out)
{
    portENTER_CRITICAL(&state_mux);
    const char *state = phase;
    esp_err_t error = last_error;
    uint32_t bytes = downloaded, size = total;
    portEXIT_CRITICAL(&state_mux);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t image_state;
    const char *validation = "undefined";
    if (esp_ota_get_state_partition(running, &image_state) == ESP_OK) {
        if (image_state == ESP_OTA_IMG_PENDING_VERIFY) validation = "pending_verify";
        else if (image_state == ESP_OTA_IMG_VALID) validation = "valid";
    }
    fprintf(out, "{\"type\":\"ota_status\",\"state\":\"%s\",\"error\":\"%s\","
            "\"bytes\":%" PRIu32 ",\"total\":%" PRIu32 ",\"version\":\"%s\","
            "\"running\":\"%s\",\"boot\":\"%s\",\"next\":\"%s\","
            "\"slot_size\":%" PRIu32 ",\"validation\":\"%s\"}\n",
            state, esp_err_to_name(error), bytes, size, esp_app_get_description()->version,
            running->label, boot ? boot->label : "none", next ? next->label : "none",
            next ? next->size : 0, validation);
}

void ota_update_confirm_boot(bool startup_ok)
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY)
        return;
    if (!startup_ok) {
        printf("OTA: startup failed, rolling back\n");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    printf("OTA: boot confirmation: %s\n", esp_err_to_name(err));
}
