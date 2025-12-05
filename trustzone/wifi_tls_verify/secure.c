#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "pico/secure.h"


bool repeating_timer_callback(__unused struct repeating_timer *t) {
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
    add_repeating_timer_ms(-100, repeating_timer_callback, NULL, &timer);

    // Get boot partition
    boot_info_t info;
    rom_get_boot_info(&info);
    printf("Boot partition: %d\n", info.partition);

    // Roll QMI to matching Non-Secure partition
    int ns_partition = rom_get_owned_partition(info.partition);
    printf("Matching Non-Secure partition: %d\n", ns_partition);
    int rc = rom_roll_qmi_to_partition(ns_partition);
    printf("Rolled QMI to Non-Secure partition, rc=%d\n", rc);

    // Enable NS GPIO access to CYW43 GPIOs
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_REG_ON, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_DATA_OUT, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_DATA_IN, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_HOST_WAKE, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_CLOCK, true);
    gpio_assign_to_ns(CYW43_DEFAULT_PIN_WL_CS, true);

    // Disable SAU before configuring
    secure_sau_set_enabled(false);

    // Configure SAU regions
    secure_sau_configure_split();

    // Enable SAU
    secure_sau_set_enabled(true);
    printf("SAU Configured & Enabled\n");

    // Install default hardfault handler, with callback to reset to USB boot
    secure_install_default_hardfault_handler(hardfault_callback);

    // Launch Non-Secure binary
    secure_launch_nonsecure_binary(XIP_BASE, SRAM8_BASE);

    // Should never return from non-secure code
    printf("Shouldn't return from non-secure code\n");
}
