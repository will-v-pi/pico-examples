/*
 * Copyright (c) 2024 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// TinyUSB-based PICOBOOT connection for use in a USB host application.
//
// Adapts picoboot_connection.c from picotool (which uses libusb) to run on an
// RP2040/RP2350 acting as a USB host via TinyUSB.
//
// Usage:
//   1. Call picoboot_app_mount(daddr) from tuh_mount_cb().
//   2. Call picoboot_app_unmount(daddr) from tuh_umount_cb().
//   3. Call picoboot_app_task() each iteration of the main loop — it performs
//      deferred endpoint setup for newly detected PICOBOOT devices.
//   4. Use picoboot_app_is_mounted() / picoboot_app_get_daddr() to check
//      whether a device is ready, then call picoboot_* commands freely.
//
// All picoboot_* command functions block until the USB transfer completes.
// They must only be called from the main loop, not from TinyUSB callbacks.
//
// Buffer alignment: user buffers passed to picoboot_read() / picoboot_write()
// must be accessible by the USB DMA controller. On RP2040 any SRAM buffer
// satisfies this. On RP2350 with DCache enabled, use TUH_EPBUF_DEF() or
// ensure proper cache coherency.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pico.h"
#include "boot/picoboot.h"

#ifdef __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// Lifecycle — wire these into tuh_mount_cb / tuh_umount_cb / main loop
// --------------------------------------------------------------------------

// Check whether daddr is a PICOBOOT device and schedule endpoint setup.
// Call this from tuh_mount_cb().
void picoboot_app_mount(uint8_t daddr);

// Clean up all state for a disconnected device.
// Call this from tuh_umount_cb().
void picoboot_app_unmount(uint8_t daddr);

// Perform deferred endpoint setup for any newly detected PICOBOOT devices.
// Call this every iteration of the main loop.
void picoboot_app_task(void);

// --------------------------------------------------------------------------
// Query
// --------------------------------------------------------------------------

// Returns true if daddr is a PICOBOOT device whose endpoints are open and
// ready to accept commands.
bool picoboot_app_is_mounted(uint8_t daddr);

// Returns the device address of the first connected PICOBOOT device, or 0 if
// none is connected.
uint8_t picoboot_app_get_daddr(void);

// Returns true if the connected device is an RP2350 (false = RP2040).
// Only valid when picoboot_app_is_mounted(daddr) is true.
bool picoboot_app_is_rp2350(uint8_t daddr);

// --------------------------------------------------------------------------
// PICOBOOT commands — all return 0 on success, non-zero on failure
// --------------------------------------------------------------------------

// Un-stall both bulk endpoints and send the vendor RESET control request.
int picoboot_reset(uint8_t daddr);

// Read the 16-byte command-status block into *status (may be NULL to discard).
int picoboot_cmd_status(uint8_t daddr, struct picoboot_cmd_status *status);

// Claim or release exclusive access (exclusive = 1 to claim, 0 to release).
int picoboot_exclusive_access(uint8_t daddr, uint8_t exclusive);

// Disable XIP so flash can be accessed via SPI commands.
int picoboot_exit_xip(uint8_t daddr);

// Re-enable command-mode XIP (makes the flash contents readable via XIP bus).
int picoboot_enter_cmd_xip(uint8_t daddr);

// Reboot the target device. pc=0 reboots via the normal boot path.
int picoboot_reboot(uint8_t daddr, uint32_t pc, uint32_t sp, uint32_t delay_ms);

// RP2350 extended reboot (see struct picoboot_reboot2_cmd).
int picoboot_reboot2(uint8_t daddr, struct picoboot_reboot2_cmd *reboot_cmd);

// Read len bytes of the GET_INFO response into buffer.
int picoboot_get_info(uint8_t daddr, struct picoboot_get_info_cmd *cmd,
                      uint8_t *buffer, uint32_t len);

// Execute code at addr on the target device (Thumb-mode on RP2040/RP2350-ARM).
int picoboot_exec(uint8_t daddr, uint32_t addr);

// Set the flash vector table address (RP2040 only).
int picoboot_vector(uint8_t daddr, uint32_t addr);

// Erase len bytes of flash starting at addr (must be sector-aligned, 4 KiB).
int picoboot_flash_erase(uint8_t daddr, uint32_t addr, uint32_t len);

// Write len bytes from buffer to device address addr (RAM or flash).
// buffer must be DMA-accessible; see note at the top of this header.
int picoboot_write(uint8_t daddr, uint32_t addr, uint8_t *buffer, uint32_t len);

// Read len bytes from device address addr into buffer.
// buffer must be DMA-accessible; see note at the top of this header.
int picoboot_read(uint8_t daddr, uint32_t addr, uint8_t *buffer, uint32_t len);

// Write OTP rows (RP2350 only).
int picoboot_otp_write(uint8_t daddr, struct picoboot_otp_cmd *otp_cmd,
                       uint8_t *buffer, uint32_t len);

// Read OTP rows (RP2350 only).
int picoboot_otp_read(uint8_t daddr, struct picoboot_otp_cmd *otp_cmd,
                      uint8_t *buffer, uint32_t len);

// Write a 32-bit word to an arbitrary address on the target via EXEC (RP2040).
int picoboot_poke(uint8_t daddr, uint32_t addr, uint32_t data);

// Read a 32-bit word from an arbitrary address on the target via EXEC (RP2040).
int picoboot_peek(uint8_t daddr, uint32_t addr, uint32_t *data);

#ifdef __cplusplus
}
#endif
