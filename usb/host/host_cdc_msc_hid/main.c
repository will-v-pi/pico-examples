/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "picoboot_app.h"
#include "boot/bootrom_constants.h"

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+
void led_blinking_task(void);
extern void cdc_app_task(void);
extern void hid_app_task(void);
void print_devinfo_task(void);
void picoboot_demo_task(void);
static void print_utf16(uint16_t* temp_buf, size_t buf_len);

#if CFG_TUH_ENABLED && CFG_TUH_MAX3421
// API to read/rite MAX3421's register. Implemented by TinyUSB
extern uint8_t tuh_max3421_reg_read(uint8_t rhport, uint8_t reg, bool in_isr);
extern bool tuh_max3421_reg_write(uint8_t rhport, uint8_t reg, uint8_t data, bool in_isr);
#endif

// Declare for buffer for usb transfer, may need to be in USB/DMA section and
// multiple of dcache line size if dcache is enabled (for some ports).
CFG_TUH_MEM_SECTION struct {
  TUH_EPBUF_TYPE_DEF(tusb_desc_device_t, device);
  TUH_EPBUF_DEF(serial, 64*sizeof(uint16_t));
  TUH_EPBUF_DEF(buf, 128*sizeof(uint16_t));
} desc;

// One flag per possible device address — set by tuh_mount_cb (host task) and
// cleared by print_devinfo_task (separate task / main loop) once the device's
// descriptor info has been printed.
static volatile bool need_devinfo[CFG_TUH_DEVICE_MAX + 1];

/*------------- MAIN -------------*/
int main(void) {
  board_init();

  printf("TinyUSB Host CDC MSC HID Example\r\n");

  // init host stack on configured roothub port
  tuh_init(BOARD_TUH_RHPORT);

  if (board_init_after_tusb) {
    board_init_after_tusb();
  }

#if CFG_TUH_ENABLED && CFG_TUH_MAX3421
  // FeatherWing MAX3421E use MAX3421E's GPIO0 for VBUS enable
  enum { IOPINS1_ADDR  = 20u << 3, /* 0xA0 */ };
  tuh_max3421_reg_write(BOARD_TUH_RHPORT, IOPINS1_ADDR, 0x01, false);
#endif

  while (1) {
    // tinyusb host task
    tuh_task();

    print_devinfo_task();
    picoboot_app_task();
    picoboot_demo_task();

    led_blinking_task();
    cdc_app_task();
    hid_app_task();
  }
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

void tuh_mount_cb(uint8_t dev_addr) {
  // application set-up
  printf("A device with address %d is mounted\r\n", dev_addr);
  if (dev_addr < TU_ARRAY_SIZE(need_devinfo)) {
    need_devinfo[dev_addr] = true;
  }
  picoboot_app_mount(dev_addr);
}

void tuh_umount_cb(uint8_t dev_addr) {
  // application tear-down
  if (dev_addr < TU_ARRAY_SIZE(need_devinfo)) {
    need_devinfo[dev_addr] = false;
  }
  picoboot_app_unmount(dev_addr);
  printf("A device with address %d is unmounted \r\n", dev_addr);
}


//--------------------------------------------------------------------+
// Blinking Task
//--------------------------------------------------------------------+
void led_blinking_task(void) {
  const uint32_t interval_ms = 1000;
  static uint32_t start_ms = 0;

  static bool led_state = false;

  // Blink every interval ms
  if (board_millis() - start_ms < interval_ms) return; // not enough time
  start_ms += interval_ms;

  board_led_write(led_state);
  led_state = 1 - led_state; // toggle
}

//--------------------------------------------------------------------+
// Print device info task — serialises descriptor fetching across all
// mounted devices using the sync helpers. Sync calls are safe here because
// this task runs outside the host-task callback context (main loop on
// OS_NONE / dedicated FreeRTOS task on RTOS).
//--------------------------------------------------------------------+

// English
#define LANGUAGE_ID 0x0409

static void print_one_device(uint8_t daddr) {
  // Get Device Descriptor
  uint8_t xfer_result = tuh_descriptor_get_device_sync(daddr, &desc.device, 18);
  if (XFER_RESULT_SUCCESS != xfer_result) {
    printf("Failed to get device descriptor\r\n");
    return;
  }

  printf("Device %u: ID %04x:%04x SN ", daddr, desc.device.idVendor, desc.device.idProduct);

  xfer_result = XFER_RESULT_FAILED;
  if (desc.device.iSerialNumber != 0) {
    xfer_result = tuh_descriptor_get_serial_string_sync(daddr, LANGUAGE_ID, desc.serial, sizeof(desc.serial));
  }
  if (XFER_RESULT_SUCCESS != xfer_result) {
    uint16_t* serial = (uint16_t*)(uintptr_t) desc.serial;
    serial[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * 1 + 2));
    serial[1] = '0';
    serial[2] = 0;
  }
  print_utf16((uint16_t*)(uintptr_t) desc.serial, sizeof(desc.serial)/2);
  printf("\r\n");

  printf("Device Descriptor:\r\n");
  printf("  bLength             %u\r\n", desc.device.bLength);
  printf("  bDescriptorType     %u\r\n", desc.device.bDescriptorType);
  printf("  bcdUSB              %04x\r\n", desc.device.bcdUSB);
  printf("  bDeviceClass        %u\r\n", desc.device.bDeviceClass);
  printf("  bDeviceSubClass     %u\r\n", desc.device.bDeviceSubClass);
  printf("  bDeviceProtocol     %u\r\n", desc.device.bDeviceProtocol);
  printf("  bMaxPacketSize0     %u\r\n", desc.device.bMaxPacketSize0);
  printf("  idVendor            0x%04x\r\n", desc.device.idVendor);
  printf("  idProduct           0x%04x\r\n", desc.device.idProduct);
  printf("  bcdDevice           %04x\r\n", desc.device.bcdDevice);

  printf("  iManufacturer       %u     ", desc.device.iManufacturer);
  if (desc.device.iManufacturer != 0) {
    if (XFER_RESULT_SUCCESS == tuh_descriptor_get_manufacturer_string_sync(daddr, LANGUAGE_ID, desc.buf, sizeof(desc.buf))) {
      print_utf16((uint16_t*)(uintptr_t) desc.buf, sizeof(desc.buf)/2);
    }
  }
  printf("\r\n");

  printf("  iProduct            %u     ", desc.device.iProduct);
  if (desc.device.iProduct != 0) {
    if (XFER_RESULT_SUCCESS == tuh_descriptor_get_product_string_sync(daddr, LANGUAGE_ID, desc.buf, sizeof(desc.buf))) {
      print_utf16((uint16_t*)(uintptr_t) desc.buf, sizeof(desc.buf)/2);
    }
  }
  printf("\r\n");

  printf("  iSerialNumber       %u     ", desc.device.iSerialNumber);
  printf("%s\r\n", (char*)desc.serial); // serial is already UTF-8
  printf("  bNumConfigurations  %u\r\n", desc.device.bNumConfigurations);
}

void print_devinfo_task(void) {
  for (uint8_t daddr = 1; daddr < TU_ARRAY_SIZE(need_devinfo); daddr++) {
    if (need_devinfo[daddr]) {
      need_devinfo[daddr] = false;
      print_one_device(daddr);
    }
  }
}

//--------------------------------------------------------------------+
// PICOBOOT demo task
//
// Runs once when a PICOBOOT device first becomes ready. Queries the
// chip-info SYS_INFO block and reads the first 16 bytes of SRAM as a
// smoke-test. Extend or replace this with your own picoboot_* calls.
//--------------------------------------------------------------------+

// Buffer for GET_INFO results — must be DMA-accessible (use TUH_EPBUF_DEF
// or a static with CFG_TUH_MEM_SECTION for RP2350 DCache builds).
CFG_TUH_MEM_SECTION static uint8_t picoboot_info_buf[64];
CFG_TUH_MEM_SECTION static uint8_t picoboot_read_buf[16];

void picoboot_demo_task(void) {
  static uint8_t last_daddr = 0;
  uint8_t daddr = picoboot_app_get_daddr();

  if (daddr == last_daddr) return;  // no change
  last_daddr = daddr;

  if (daddr == 0) return;  // just disconnected, nothing to do here

  bool is_rp2350 = picoboot_app_is_rp2350(daddr);
  printf("\r\n--- PICOBOOT demo (addr %u, %s) ---\r\n",
         daddr, is_rp2350 ? "RP2350" : "RP2040");

  // PC_GET_INFO is RP2350-only — skip it entirely on RP2040 since the
  // bootrom does not respond to unknown commands, causing an indefinite hang.
  if (is_rp2350) {
    struct picoboot_get_info_cmd info_cmd = {
      .bType      = PICOBOOT_GET_INFO_SYS,
      .dParams[0] = SYS_INFO_CHIP_INFO,
    };
    if (picoboot_get_info(daddr, &info_cmd, picoboot_info_buf,
                          sizeof(picoboot_info_buf)) == 0) {
      uint32_t chip_word;
      memcpy(&chip_word, picoboot_info_buf, sizeof(chip_word));
      printf("PICOBOOT: SYS_INFO_CHIP_INFO word 0 = 0x%08lx\r\n", (unsigned long)chip_word);
    } else {
      printf("PICOBOOT: GET_INFO failed\r\n");
    }
  }

  // Read the first 16 bytes of SRAM — supported on both RP2040 and RP2350.
  if (picoboot_read(daddr, 0x20000000u, picoboot_read_buf,
                    sizeof(picoboot_read_buf)) == 0) {
    printf("PICOBOOT: SRAM[0..15] =");
    for (int i = 0; i < (int)sizeof(picoboot_read_buf); i++) {
      printf(" %02x", picoboot_read_buf[i]);
    }
    printf("\r\n");
  } else {
    printf("PICOBOOT: SRAM read failed\r\n");
  }

  printf("--- PICOBOOT demo done ---\r\n\r\n");
}

//--------------------------------------------------------------------+
// String Descriptor Helper
//--------------------------------------------------------------------+

static void _convert_utf16le_to_utf8(const uint16_t* utf16, size_t utf16_len, uint8_t* utf8, size_t utf8_len) {
  // TODO: Check for runover.
  (void) utf8_len;
  // Get the UTF-16 length out of the data itself.

  for (size_t i = 0; i < utf16_len; i++) {
    uint16_t chr = utf16[i];
    if (chr < 0x80) {
      *utf8++ = chr & 0xffu;
    } else if (chr < 0x800) {
      *utf8++ = (uint8_t) (0xC0 | (chr >> 6 & 0x1F));
      *utf8++ = (uint8_t) (0x80 | (chr >> 0 & 0x3F));
    } else {
      // TODO: Verify surrogate.
      *utf8++ = (uint8_t) (0xE0 | (chr >> 12 & 0x0F));
      *utf8++ = (uint8_t) (0x80 | (chr >> 6 & 0x3F));
      *utf8++ = (uint8_t) (0x80 | (chr >> 0 & 0x3F));
    }
    // TODO: Handle UTF-16 code points that take two entries.
  }
}

// Count how many bytes a utf-16-le encoded string will take in utf-8.
static int _count_utf8_bytes(const uint16_t* buf, size_t len) {
  size_t total_bytes = 0;
  for (size_t i = 0; i < len; i++) {
    uint16_t chr = buf[i];
    if (chr < 0x80) {
      total_bytes += 1;
    } else if (chr < 0x800) {
      total_bytes += 2;
    } else {
      total_bytes += 3;
    }
    // TODO: Handle UTF-16 code points that take two entries.
  }
  return (int) total_bytes;
}

static void print_utf16(uint16_t* temp_buf, size_t buf_len) {
  if ((temp_buf[0] & 0xff) == 0) return;  // empty
  size_t utf16_len = ((temp_buf[0] & 0xff) - 2) / sizeof(uint16_t);
  size_t utf8_len = (size_t) _count_utf8_bytes(temp_buf + 1, utf16_len);
  _convert_utf16le_to_utf8(temp_buf + 1, utf16_len, (uint8_t*) temp_buf, sizeof(uint16_t) * buf_len);
  ((uint8_t*) temp_buf)[utf8_len] = '\0';

  printf("%s", (char*) temp_buf);
}