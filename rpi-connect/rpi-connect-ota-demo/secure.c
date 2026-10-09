#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "pico/secure.h"
#include "hardware/flash.h"


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


int main()
{
    stdio_init_all();

    // If this was a watchdog reboot, reset to USB boot
    if (watchdog_enable_caused_reboot()) {
        printf("This was a watchdog reboot - resetting\n");
        rom_reset_usb_boot(0, 0);
    }

    // Create a repeating timer to update the watchdog every 100ms
    struct repeating_timer timer;
    watchdog_enable(110, true);
    watchdog_enabled_scratch4 = watchdog_hw->scratch[4];
    add_repeating_timer_ms(-100, repeating_timer_callback, NULL, &timer);

    // Get boot partition
    boot_info_t info;
    rom_get_boot_info(&info);
    printf("Boot partition: %d\n", info.partition);

    // Roll QMI to matching Non-Secure partition
    int ns_partition = rom_get_owned_partition(info.partition);
    printf("Matching Non-Secure partition: %d\n", ns_partition);
    int rc = flash_roll_qmi_to_partition(ns_partition);
    printf("Rolled QMI to Non-Secure partition, rc=%d\n", rc);

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

    // Configure SAU regions
    secure_sau_configure_split();

    // Enable SAU
    secure_sau_set_enabled(true);
    printf("SAU Configured & Enabled\n");

    // Install default hardfault handler, with callback to reset to USB boot
    secure_install_default_hardfault_handler(hardfault_callback);

    printf("Launching NonSecure\n");

    // Launch Non-Secure binary
    secure_launch_nonsecure_binary(XIP_BASE, SRAM8_BASE);

    // Should never return from non-secure code
    printf("Shouldn't return from non-secure code\n");
}
