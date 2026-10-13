#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "hardware/watchdog.h"
#include "pico/trustzone.h"
#include "hardware/flash.h"
#include "pico/rpi_connect_identity.h"
#include "hardware/structs/otp.h"


// Value watchdog_enable() leaves in watchdog scratch 4. Any reboot (rom_reboot() or watchdog_reboot())
// overwrites it, so if it has changed then a reboot is pending and the watchdog must not be fed,
// otherwise the reboot would never happen.
static uint32_t watchdog_enabled_scratch4;

bool repeating_timer_callback(__unused struct repeating_timer *t) {
    if (watchdog_hw->scratch[4] != watchdog_enabled_scratch4) {
        // Reboot pending - stop feeding the watchdog
        return false;
    }
    watchdog_update();
    return true;
}


void hardfault_callback(void) {
    if (m33_hw->dhcsr & M33_DHCSR_C_DEBUGEN_BITS) {
        // If in debug mode, breakpoint
        __breakpoint();
    } else {
        // If not in debug mode, reset to USB boot
        rom_reset_usb_boot(0, 0);
    }
}


// Pick the CYW43 firmware partition in the same way as cyw43_driver_init() does, so it can be protected
// before Non-Secure starts. The pick only depends on the contents of flash, so Non-Secure will make the
// same choice. Returns the partition number, or a negative error code.
// Not inlined, so the buffers aren't left on the Secure stack under main() after Non-Secure starts.
static int __noinline pick_cyw43_firmware_partition(void) {
    uint32_t buffer[(16 * 4) + 1] = {}; // maximum of 16 partitions, each with maximum of 4 words returned, plus 1
    int ret = rom_get_partition_table_info(buffer, count_of(buffer), PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_PARTITION_ID);
    if (ret < 0) return ret;

    int picked_p = PICO_ERROR_NOT_FOUND;
    for (int i = 1, p = 0; i < ret; p++) {
        i++; // skip location and permissions
        uint32_t flags_and_permissions = buffer[i++];
        if (flags_and_permissions & PICOBIN_PARTITION_FLAGS_HAS_ID_BITS) {
            uint64_t id = buffer[i] | ((uint64_t)buffer[i + 1] << 32);
            i += 2;
            if (id == CYW43_FIRMWARE_PARTITION_ID) {
                picked_p = p;
                break;
            }
        }
    }
    if (picked_p < 0) return picked_p;

    uint32_t *workarea = malloc(0x1000);
    if (!workarea) return PICO_ERROR_INSUFFICIENT_RESOURCES;
    int ret_p = rom_pick_ab_partition_during_update(workarea, 0x1000, picked_p);
    if (ret_p == BOOTROM_ERROR_INVALID_DATA) {
        // No block loop in the B partition, so write an invalid block there in order to verify the A
        // partition, as cyw43_driver_init() would
        flash_program_invalid_block_to_b_partition(picked_p);
        ret_p = rom_pick_ab_partition_during_update(workarea, 0x1000, picked_p);
    }
    free(workarea);
    return ret_p;
}

int main()
{
    stdio_init_all();

    // If this was a watchdog reboot, reset to USB boot
    if (watchdog_enable_caused_reboot()) {
        printf("This was a watchdog reboot - resetting\n");
        rom_reset_usb_boot(0, 0);
    }

    // Create a repeating timer to update the watchdog every 100ms. Interrupts are disabled during
    // flash operations, and a flash sector erase can take up to ~400ms, so the timeout must be longer
    // than that plus the 100ms update period, otherwise the watchdog can fire during an update.
    struct repeating_timer timer;
    watchdog_enable(600, true);
    watchdog_enabled_scratch4 = watchdog_hw->scratch[4];
    add_repeating_timer_ms(-100, repeating_timer_callback, NULL, &timer);

    // Get boot partition
    boot_info_t info;
    rom_get_boot_info(&info);
    printf("Boot partition: %d\n", info.partition);

#if PICO_SECURITY_SPLIT_NO_FLASH
    // The Non-Secure binary is embedded in this binary, and has already been loaded into the
    // Non-Secure SRAM region. Clear all the QMI windows, so Non-Secure can only access the
    // partitions it rolls into them (the WiFi firmware and FFS partitions).
    int window = 0;
    int rc = flash_roll_qmi_add_partition_at_window(-1, &window, true);
    printf("Cleared QMI windows, rc=%d\n", rc);
#else
    // Roll QMI to matching Non-Secure partition
    int ns_partition = rom_get_owned_partition(info.partition);
    printf("Matching Non-Secure partition: %d\n", ns_partition);
    int rc = flash_roll_qmi_to_partition(ns_partition);
    printf("Rolled QMI to Non-Secure partition, rc=%d\n", rc);
#endif

    // Stop Non-Secure writing to the partition(s) being booted, so the Non-Secure updater can only
    // ever write updates to the other A/B partition(s). This lasts until the next reboot.
    rc = rom_remove_flash_partition_permissions(info.partition, PICOBIN_PARTITION_PERMISSION_NS_W_BITS);
    printf("Removed Non-Secure write permission from boot partition, rc=%d\n", rc);
#if !PICO_SECURITY_SPLIT_NO_FLASH
    rc = rom_remove_flash_partition_permissions(ns_partition, PICOBIN_PARTITION_PERMISSION_NS_W_BITS);
    printf("Removed Non-Secure write permission from Non-Secure partition, rc=%d\n", rc);
#endif
    // Likewise for the WiFi firmware partition which will be used
    int wifi_partition = pick_cyw43_firmware_partition();
    printf("WiFi firmware partition: %d\n", wifi_partition);
    if (wifi_partition >= 0) {
        rc = rom_remove_flash_partition_permissions(wifi_partition, PICOBIN_PARTITION_PERMISSION_NS_W_BITS);
        printf("Removed Non-Secure write permission from WiFi firmware partition, rc=%d\n", rc);
    }

    // Enable NS GPIO access to CYW43 GPIOs
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_REG_ON, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_DATA_OUT, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_DATA_IN, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_HOST_WAKE, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_CLOCK, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_CS, true);

    rom_set_ns_api_permission(BOOTROM_NS_API_get_partition_table_info, true);
    rom_set_ns_api_permission(BOOTROM_NS_API_get_b_partition, true);
    rom_set_ns_api_permission(BOOTROM_NS_API_checked_flash_op, true);
#if PICO_ADD_NONSECURE_FLASH_ERASE_HELPER
    // RP2350 A2 bootrom faults on non-secure flash_op erases, so secure performs them instead
    secure_set_nonsecure_flash_erase_helper_enabled(true);
#endif
    rom_set_ns_api_permission(BOOTROM_NS_API_reboot, true);

    // For testing: soft lock the OTP page(s) holding the device identity private key, so
    // NonSecure can't read it (directly, or via the bootrom otp_access API). This only lasts
    // until the next reset - for production, set the permanent page locks when provisioning.
    const uint first_key_page = RPI_CONNECT_IDENTITY_OTP_ROW / NUM_OTP_PAGE_ROWS;
    const uint last_key_page = (RPI_CONNECT_IDENTITY_OTP_ROW + 15) / NUM_OTP_PAGE_ROWS;
    for (uint page = first_key_page; page <= last_key_page; page++) {
        // Writes can only advance the lock state, so this leaves the Secure lock unchanged
        otp_hw->sw_lock[page] = otp_hw->sw_lock[page] |
            (OTP_SW_LOCK0_NSEC_VALUE_INACCESSIBLE << OTP_SW_LOCK0_NSEC_LSB);
        printf("OTP page %u soft locked for NonSecure, SW_LOCK=%x\n", page, (unsigned)otp_hw->sw_lock[page]);
    }

    // Let NonSecure use the device identity key, without exposing the private key
    rc = rpi_connect_identity_secure_init();
    printf("Device identity key secure calls %s (rc=%d)\n", rc == BOOTROM_OK ? "enabled" : "failed", rc);

    // Configure SAU regions
    secure_sau_configure_split();
#if PICO_SECURITY_SPLIT_NO_FLASH
    // The Non-Secure code isn't in flash, so the split doesn't make XIP Non-Secure, but Non-Secure
    // still reads the WiFi firmware and FFS partitions through the QMI windows
    secure_sau_configure_region(0, XIP_BASE, XIP_END, true, false);
#endif

    // Enable SAU
    secure_sau_set_enabled(true);
    printf("SAU Configured & Enabled\n");

    // Install default hardfault handler, with callback to reset to USB boot
    secure_install_default_hardfault_handler(hardfault_callback);

    printf("Launching NonSecure\n");

    // Launch Non-Secure binary, from flash or the Non-Secure SRAM region
    secure_launch_nonsecure_binary_default();

    // Should never return from non-secure code
    printf("Shouldn't return from non-secure code\n");
}
