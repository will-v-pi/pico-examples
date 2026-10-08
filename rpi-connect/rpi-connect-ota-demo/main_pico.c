/**
 * Copyright (c) 2025 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rpi_connect_ota_demo.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/ffs.h"
#include "pico/rpi_connect.h"
#include "pico/rpi_connect_ota.h"
#include "pico/stdio.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "hardware/xip_cache.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "hardware/flash.h"
#include "hardware/structs/powman.h"

static char *rpi_connect_ota_demo_wifi_password;
static char *rpi_connect_ota_demo_wifi_ssid;

static int wifi_load_credentials(void) {
    char *wifi_ssid = ffs_get_string(FFS_WIFI_SSID_FILE_ID);
    char *wifi_password = ffs_get_string(FFS_WIFI_PASSWORD_FILE_ID);

    if (!wifi_ssid || !*wifi_ssid || !wifi_password) {
        RPI_CONNECT_OTA_DEMO_ERROR("No WiFi credentials in FFS - provision with the combined UF2\n");
        free(wifi_ssid);
        free(wifi_password);
        return PICO_ERROR_BADAUTH;
    }

    RPI_CONNECT_OTA_DEMO_INFO("WiFi SSID: %s\n", wifi_ssid);
    rpi_connect_ota_demo_wifi_password = wifi_password;
    rpi_connect_ota_demo_wifi_ssid = wifi_ssid;
    return PICO_OK;
}

static void wifi_free_credentials(void) {
    free(rpi_connect_ota_demo_wifi_password);
    rpi_connect_ota_demo_wifi_password = NULL;

    free(rpi_connect_ota_demo_wifi_ssid);
    rpi_connect_ota_demo_wifi_ssid = NULL;
}

async_context_t *rpi_connect_default_async_context(void) {
    return cyw43_arch_async_context();
}

int64_t watchdog_alarm_callback(alarm_id_t id, void *alarm_id) {
    RPI_CONNECT_OTA_DEMO_DEBUG("Watchdog reached %dms in timer %d\n", watchdog_get_time_remaining_ms(), id);
    bool watchdog_running = watchdog_get_time_remaining_ms() > 0;
    if (watchdog_running) {
        watchdog_hw->load = WATCHDOG_LOAD_BITS;
    }
    *(int*)alarm_id = 0;
    return 0;
}

void reload_watchdog_timeout(bool with_alarm) { // with_alarm is ~32s, otherwise ~16s, skipped if watchdog isn't running
    static int alarm_id = 0;
    bool watchdog_running = watchdog_get_time_remaining_ms() > 0;
    if (watchdog_running) {
        RPI_CONNECT_OTA_DEMO_DEBUG("Watchdog reached %dms\n", watchdog_get_time_remaining_ms());
        watchdog_hw->load = WATCHDOG_LOAD_BITS;
        if (alarm_id > 0) {
            // Cancel any existing alarms to reload it
            RPI_CONNECT_OTA_DEMO_DEBUG("Cancelling existing alarm\n");
            cancel_alarm(alarm_id);
        }
        if (with_alarm) {
            // Give watchdog 2x the time using an alarm
            alarm_id = add_alarm_in_ms((WATCHDOG_LOAD_BITS / 1000) - 500, watchdog_alarm_callback, &alarm_id, false);
        }
    }
}

int main() {
    char serial_number[2*PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    char *token = NULL;
    int rc = 0;

    stdio_init_all();

    pico_get_unique_board_id_string(serial_number, sizeof(serial_number));

    RPI_CONNECT_OTA_DEMO_INFO("rpi_connect_ota_demo version: %s (image v%d.%d)\n",
        PICO_PROGRAM_VERSION_STRING, RPI_CONNECT_VERSION_MAJOR, RPI_CONNECT_BUILD_NUMBER);

    if (cyw43_arch_init()) {
        RPI_CONNECT_OTA_DEMO_ERROR("Failed to initialise cyw43\n");
        return 1;
    }

    rc = ffs_initialise();
    if (rc != PICO_OK) {
        RPI_CONNECT_OTA_DEMO_ERROR("Failed to initialise FFS %d\n", rc);
        goto end;
    }

    rc = wifi_load_credentials();
    if (rc != PICO_OK) {
        goto end;
    }

    RPI_CONNECT_OTA_DEMO_INFO("Connecting to WiFi %s (client_id=%s serial=%s)\n",
           rpi_connect_ota_demo_wifi_ssid, rpi_connect_client_id(), serial_number);

    cyw43_arch_enable_sta_mode();
    int retries = 5;
    // Reload watchdog giving ~32s to attempt connection
    reload_watchdog_timeout(true);
    while (cyw43_arch_wifi_connect_timeout_ms(rpi_connect_ota_demo_wifi_ssid,
                                              rpi_connect_ota_demo_wifi_password,
                                              CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        if (--retries == 0) {
            RPI_CONNECT_OTA_DEMO_ERROR("Failed to connect to WiFi\n");
            rc = -1;
            goto end;
        }
        RPI_CONNECT_OTA_DEMO_INFO("WiFi connection failed, retrying (%d remaining)\n", retries);
        if (retries > 0) {
            // Briefly reload watchdog before 2s sleep
            reload_watchdog_timeout(false);
        }
        sleep_ms(2000);
        if (retries > 0) {
            // Reload watchdog giving ~32s for next attempt
            reload_watchdog_timeout(true);
        }
    }
    RPI_CONNECT_OTA_DEMO_INFO("Connected\n");

    // Reload watchdog with ~16s now connected to WiFi
    reload_watchdog_timeout(false);

    retries = 5;
    while ((rc = rpi_connect_ota_demo_init(rpi_connect_client_id(), serial_number, "rpi_connect_ota_demo")) != PICO_OK && --retries) {
        sleep_ms(2000);
    }
    if (rc != PICO_OK) {
        RPI_CONNECT_OTA_DEMO_ERROR("Failed to sign in to Pi Connect (%d)\n", rc);
        goto end;
    }

    token = rpi_connect_ota_get_auth_token();
    rpi_connect_ota_demo_main(token);
    rc = 0;

end:
    cyw43_arch_disable_sta_mode();
    cyw43_arch_deinit();
    RPI_CONNECT_OTA_DEMO_INFO("Disconnected\n");
    free(token);
    wifi_free_credentials();

#ifdef CHAIN_ON_EXIT_ID
    uint64_t chain_id = CHAIN_ON_EXIT_ID;
    uint64_t * chain_id_ptr = (uint64_t*)(powman_hw->scratch);
    printf("Chain on exit to ID %" PRIx64 "\n", chain_id);
    // xip_cache_pin_range(XIP_SRAM_END - sizeof(uint64_t) - XIP_BASE, sizeof(uint64_t));
    *chain_id_ptr = chain_id;
    printf("Pinned and written %" PRIx64 "\n", *chain_id_ptr);
    uint8_t* workarea = malloc(0x1000);
    
    // maximum of 16 partitions, each with maximum of 4 words returned, plus 1
    uint32_t buffer[(16 * 4) + 1] = { };
    int ret = rom_get_partition_table_info(buffer, count_of(buffer), PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | (5 << 24));
    hard_assert(buffer[0] == (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION));
    hard_assert(ret == 3);

    uint32_t location_and_permissions = buffer[1];
    uint32_t saddr = ((location_and_permissions >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) & 0x1fffu) * FLASH_SECTOR_SIZE;
    uint32_t eaddr = (((location_and_permissions >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB) & 0x1fffu) + 1) * FLASH_SECTOR_SIZE;
    printf("Chaining to %08x\n", XIP_BASE + saddr);
    stdio_deinit_all();
    rc = rom_chain_image(workarea, 0x1000, XIP_BASE + saddr, eaddr - saddr);
    stdio_init_all();
    printf("rom_chain_image returned %d\n", rc);
    free(workarea);
#endif

    return rc;
}
