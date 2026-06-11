/*
 * Copyright (c) 2024 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// TinyUSB-based PICOBOOT connection — adapted from picotool's
// picoboot_connection/picoboot_connection.c (which uses libusb).
//
// The libusb_device_handle* parameter becomes uint8_t daddr (TinyUSB device
// address). libusb_control_transfer → tuh_control_xfer (sync),
// libusb_bulk_transfer → tuh_edpt_xfer (sync).

#include "picoboot_app.h"
#include "tusb.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

// --------------------------------------------------------------------------
// VID / PID constants
// --------------------------------------------------------------------------

#define PICOBOOT_VID            0x2e8au
#define PICOBOOT_PID_RP2040     0x0003u
#define PICOBOOT_PID_RP2350     0x000fu

// --------------------------------------------------------------------------
// Per-device state
// --------------------------------------------------------------------------

typedef enum { XIP_UNKNOWN, XIP_ACTIVE, XIP_INACTIVE } xip_state_t;

typedef struct {
    bool        mounted;
    bool        is_rp2350;
    uint8_t     itf_num;
    uint8_t     out_ep;
    uint8_t     in_ep;
    bool        definitely_exclusive;
    xip_state_t xip_state;
} picoboot_dev_t;

static picoboot_dev_t _devs[CFG_TUH_DEVICE_MAX + 1];
static volatile bool  _needs_setup[CFG_TUH_DEVICE_MAX + 1];

// --------------------------------------------------------------------------
// Static DMA-accessible buffers
//
// TinyUSB requires transfer buffers to be accessible by the USB DMA
// controller. On RP2040 all SRAM works; on RP2350 with DCache the
// CFG_TUH_MEM_SECTION / CFG_TUH_MEM_ALIGN attributes ensure correct
// placement and alignment. Stack buffers are therefore not used here.
//
// All operations are synchronous (one at a time), so a single set of shared
// static buffers is safe.
// --------------------------------------------------------------------------

CFG_TUH_MEM_SECTION CFG_TUH_MEM_ALIGN static struct picoboot_cmd        _cmd_buf;
CFG_TUH_MEM_SECTION CFG_TUH_MEM_ALIGN static struct picoboot_cmd_status _status_buf;
CFG_TUH_MEM_SECTION CFG_TUH_MEM_ALIGN static uint8_t                    _ack_buf[64];
CFG_TUH_MEM_SECTION CFG_TUH_MEM_ALIGN static uint8_t                    _config_buf[256];

static uint32_t _token = 1;

// --------------------------------------------------------------------------
// Internal transfer helpers
// --------------------------------------------------------------------------

// Synchronous vendor/standard control transfer on EP0.
// Returns 0 on success, -1 on failure.
static int _ctrl_xfer(uint8_t daddr, uint8_t bmRequestType, uint8_t bRequest,
                      uint16_t wValue, uint16_t wIndex, uint8_t *data, uint16_t len) {
    tusb_control_request_t const req = {
        .bmRequestType = bmRequestType,
        .bRequest      = bRequest,
        .wValue        = tu_htole16(wValue),
        .wIndex        = tu_htole16(wIndex),
        .wLength       = tu_htole16(len),
    };
    xfer_result_t result = XFER_RESULT_INVALID;
    tuh_xfer_t xfer = {
        .daddr       = daddr,
        .ep_addr     = 0,
        .setup       = &req,
        .buffer      = data,
        .complete_cb = NULL,
        .user_data   = (uintptr_t)&result,
    };
    if (!tuh_control_xfer(&xfer)) return -1;
    return (result == XFER_RESULT_SUCCESS) ? 0 : -1;
}

// Completion callback used by _bulk_xfer() to implement blocking behaviour.
// tuh_edpt_xfer() is purely async; sync mode requires CFG_TUH_API_EDPT_XFER=1
// so the callback is stored and invoked when the transfer completes.
static void _bulk_complete_cb(tuh_xfer_t *xfer) {
    *((volatile xfer_result_t *)xfer->user_data) = xfer->result;
}

// Synchronous (blocking) bulk/interrupt transfer on ep_addr.
// Spins on tuh_task() until the completion callback fires.
// Returns 0 on success, -1 on failure.
static int _bulk_xfer(uint8_t daddr, uint8_t ep_addr, uint8_t *buf, uint32_t len) {
    volatile xfer_result_t result = XFER_RESULT_INVALID;
    tuh_xfer_t xfer = {
        .daddr       = daddr,
        .ep_addr     = ep_addr,
        .buflen      = len,
        .buffer      = buf,
        .complete_cb = _bulk_complete_cb,
        .user_data   = (uintptr_t)&result,
    };
    if (!tuh_edpt_xfer(&xfer)) return -1;
    while (result == XFER_RESULT_INVALID) {
        tuh_task();
    }
    return (result == XFER_RESULT_SUCCESS) ? 0 : -1;
}

// bmRequestType builder helpers (matching libusb bit layout):
//   bit 7   : direction  (0=OUT, 1=IN)
//   bits 6:5: type       (0=standard, 1=class, 2=vendor)
//   bits 4:0: recipient  (0=device, 1=interface, 2=endpoint)
#define BMRT_VENDOR_ITF_OUT  ((uint8_t)((TUSB_REQ_TYPE_VENDOR << 5) | TUSB_REQ_RCPT_INTERFACE))           // 0x41
#define BMRT_VENDOR_ITF_IN   ((uint8_t)(TUSB_DIR_IN_MASK | (TUSB_REQ_TYPE_VENDOR << 5) | TUSB_REQ_RCPT_INTERFACE)) // 0xC1
#define BMRT_STD_EDPT_OUT    ((uint8_t)(TUSB_REQ_RCPT_ENDPOINT))                                           // 0x02
#define BMRT_STD_EDPT_IN     ((uint8_t)(TUSB_DIR_IN_MASK | TUSB_REQ_RCPT_ENDPOINT))                        // 0x82

// Returns true if ep_addr is currently halted (STALLed) on the device.
// Uses _ack_buf as the DMA-accessible receive buffer.
static bool _is_halted(uint8_t daddr, uint8_t ep_addr) {
    int ret = _ctrl_xfer(daddr, BMRT_STD_EDPT_IN,
                         TUSB_REQ_GET_STATUS, 0, ep_addr, _ack_buf, 2);
    if (ret != 0) return false;
    return (_ack_buf[0] & 1u) != 0;
}

// Send CLEAR_FEATURE(ENDPOINT_HALT) to un-stall ep_addr.
static void _clear_halt(uint8_t daddr, uint8_t ep_addr) {
    _ctrl_xfer(daddr, BMRT_STD_EDPT_OUT,
               TUSB_REQ_CLEAR_FEATURE, TUSB_REQ_FEATURE_EDPT_HALT, ep_addr, NULL, 0);
}

// --------------------------------------------------------------------------
// Device setup — walks the configuration descriptor to locate the PICOBOOT
// interface (class=0xFF, exactly 2 bulk endpoints) and opens those endpoints.
// Called from picoboot_app_task(), not from a TinyUSB callback, so sync
// descriptor fetches are safe.
// --------------------------------------------------------------------------

static bool _setup_device(uint8_t daddr) {
    if (XFER_RESULT_SUCCESS !=
        tuh_descriptor_get_configuration_sync(daddr, 0, _config_buf, sizeof(_config_buf))) {
        printf("PICOBOOT: failed to get config descriptor (addr %u)\r\n", daddr);
        return false;
    }

    picoboot_dev_t *dev = &_devs[daddr];
    dev->itf_num = 0xff;
    dev->out_ep  = 0;
    dev->in_ep   = 0;

    tusb_desc_configuration_t const *p_cfg = (tusb_desc_configuration_t const *)_config_buf;
    uint8_t const *p   = (uint8_t const *)p_cfg;
    uint8_t const *end = p + tu_le16toh(p_cfg->wTotalLength);
    bool in_picoboot_itf = false;
    p = tu_desc_next(p); // skip the config descriptor itself

    while (p < end) {
        switch (tu_desc_type(p)) {
            case TUSB_DESC_INTERFACE: {
                tusb_desc_interface_t const *itf = (tusb_desc_interface_t const *)p;
                if (itf->bInterfaceClass == 0xff && itf->bNumEndpoints == 2) {
                    dev->itf_num     = itf->bInterfaceNumber;
                    in_picoboot_itf  = true;
                } else {
                    in_picoboot_itf = false;
                }
                break;
            }
            case TUSB_DESC_ENDPOINT: {
                if (!in_picoboot_itf) break;
                tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
                if (!tuh_edpt_open(daddr, ep)) {
                    printf("PICOBOOT: failed to open endpoint 0x%02x\r\n",
                           ep->bEndpointAddress);
                    return false;
                }
                if (ep->bEndpointAddress & TUSB_DIR_IN_MASK) {
                    dev->in_ep = ep->bEndpointAddress;
                } else {
                    dev->out_ep = ep->bEndpointAddress;
                }
                break;
            }
            default:
                break;
        }
        p = tu_desc_next(p);
    }

    if (!dev->out_ep || !dev->in_ep) {
        printf("PICOBOOT: PICOBOOT interface not found on addr %u\r\n", daddr);
        return false;
    }

    dev->definitely_exclusive = false;
    dev->xip_state = XIP_UNKNOWN;
    dev->mounted   = true;

    printf("PICOBOOT: addr %u ready  itf=%u  out=0x%02x  in=0x%02x\r\n",
           daddr, dev->itf_num, dev->out_ep, dev->in_ep);
    return true;
}

// --------------------------------------------------------------------------
// Public lifecycle
// --------------------------------------------------------------------------

void picoboot_app_mount(uint8_t daddr) {
    uint16_t vid, pid;
    tuh_vid_pid_get(daddr, &vid, &pid);
    if (vid == PICOBOOT_VID &&
        (pid == PICOBOOT_PID_RP2040 || pid == PICOBOOT_PID_RP2350)) {
        printf("PICOBOOT: device detected  addr=%u  VID=%04x  PID=%04x  chip=%s\r\n",
               daddr, vid, pid, pid == PICOBOOT_PID_RP2350 ? "RP2350" : "RP2040");
        if (daddr < TU_ARRAY_SIZE(_needs_setup)) {
            _devs[daddr].is_rp2350 = (pid == PICOBOOT_PID_RP2350);
            _needs_setup[daddr] = true;
        }
    }
}

void picoboot_app_unmount(uint8_t daddr) {
    if (daddr < TU_ARRAY_SIZE(_devs)) {
        _devs[daddr].mounted = false;
        _needs_setup[daddr]  = false;
        printf("PICOBOOT: addr %u disconnected\r\n", daddr);
    }
}

void picoboot_app_task(void) {
    for (uint8_t daddr = 1; daddr < TU_ARRAY_SIZE(_needs_setup); daddr++) {
        if (_needs_setup[daddr]) {
            _needs_setup[daddr] = false;
            _setup_device(daddr);
        }
    }
}

bool picoboot_app_is_mounted(uint8_t daddr) {
    if (daddr >= TU_ARRAY_SIZE(_devs)) return false;
    return _devs[daddr].mounted;
}

uint8_t picoboot_app_get_daddr(void) {
    for (uint8_t daddr = 1; daddr < TU_ARRAY_SIZE(_devs); daddr++) {
        if (_devs[daddr].mounted) return daddr;
    }
    return 0;
}

bool picoboot_app_is_rp2350(uint8_t daddr) {
    if (daddr >= TU_ARRAY_SIZE(_devs)) return false;
    return _devs[daddr].is_rp2350;
}

// --------------------------------------------------------------------------
// PICOBOOT protocol: control requests
// --------------------------------------------------------------------------

int picoboot_reset(uint8_t daddr) {
    picoboot_dev_t *dev = &_devs[daddr];
    printf("PICOBOOT RESET\r\n");
    if (_is_halted(daddr, dev->in_ep))  _clear_halt(daddr, dev->in_ep);
    if (_is_halted(daddr, dev->out_ep)) _clear_halt(daddr, dev->out_ep);
    int ret = _ctrl_xfer(daddr, BMRT_VENDOR_ITF_OUT,
                         PICOBOOT_IF_RESET, 0, dev->itf_num, NULL, 0);
    if (ret != 0) printf("PICOBOOT RESET failed\r\n");
    dev->definitely_exclusive = false;
    return ret;
}

int picoboot_cmd_status(uint8_t daddr, struct picoboot_cmd_status *status) {
    picoboot_dev_t *dev = &_devs[daddr];
    int ret = _ctrl_xfer(daddr, BMRT_VENDOR_ITF_IN,
                         PICOBOOT_IF_CMD_STATUS, 0, dev->itf_num,
                         (uint8_t *)&_status_buf, sizeof(_status_buf));
    if (ret == 0 && status) {
        *status = _status_buf;
    }
    return ret;
}

// --------------------------------------------------------------------------
// PICOBOOT protocol: bulk command engine
//
// Layout of a PICOBOOT transaction:
//   1. Host sends 32-byte picoboot_cmd struct OUT on out_ep.
//   2. If dTransferLength > 0:
//        IN command  (bCmdId bit 7 set): host receives data on in_ep.
//        OUT command (bCmdId bit 7 clear): host sends data on out_ep.
//   3. ACK (opposite direction):
//        After IN data:  host sends 1-byte OUT on out_ep.
//        After OUT data: host receives ZLP (or short packet) IN on in_ep.
// --------------------------------------------------------------------------

static int _picoboot_cmd(uint8_t daddr, struct picoboot_cmd *cmd,
                         uint8_t *buffer, uint32_t buf_size) {
    picoboot_dev_t *dev = &_devs[daddr];

    // Fill magic and token into the DMA-accessible command buffer
    memcpy(&_cmd_buf, cmd, sizeof(_cmd_buf));
    _cmd_buf.dMagic = PICOBOOT_MAGIC;
    _cmd_buf.dToken = _token++;

    // 1. Send command
    int ret = _bulk_xfer(daddr, dev->out_ep,
                         (uint8_t *)&_cmd_buf, sizeof(struct picoboot_cmd));
    if (ret != 0) {
        printf("PICOBOOT: command send failed (cmd=0x%02x)\r\n", cmd->bCmdId);
        return ret;
    }

    xip_state_t saved_xip       = dev->xip_state;
    bool        saved_exclusive = dev->definitely_exclusive;
    dev->xip_state            = XIP_UNKNOWN;
    dev->definitely_exclusive = false;

    // 2. Data phase
    if (cmd->dTransferLength != 0) {
        assert(buf_size >= cmd->dTransferLength);
        if (cmd->bCmdId & 0x80u) {
            ret = _bulk_xfer(daddr, dev->in_ep, buffer, cmd->dTransferLength);
        } else {
            ret = _bulk_xfer(daddr, dev->out_ep, buffer, cmd->dTransferLength);
        }
        if (ret != 0) {
            printf("PICOBOOT: data phase failed (cmd=0x%02x)\r\n", cmd->bCmdId);
            picoboot_cmd_status(daddr, NULL);
            return ret;
        }
    }

    // 3. ACK phase (opposite direction to data)
    if (cmd->bCmdId & 0x80u) {
        // IN command: host sends a short OUT packet as acknowledgement
        ret = _bulk_xfer(daddr, dev->out_ep, _ack_buf, 1);
    } else {
        // OUT command: host receives ZLP / short IN packet from device
        ret = _bulk_xfer(daddr, dev->in_ep, _ack_buf, sizeof(_ack_buf));
    }

    if (ret == 0) {
        // Track XIP state
        switch (cmd->bCmdId) {
            case PC_EXIT_XIP:      dev->xip_state = XIP_INACTIVE;  break;
            case PC_ENTER_CMD_XIP: dev->xip_state = XIP_ACTIVE;    break;
            case PC_READ:
            case PC_WRITE:         dev->xip_state = saved_xip;     break;
            default:               dev->xip_state = XIP_UNKNOWN;   break;
        }
        // Track exclusive-access state
        switch (cmd->bCmdId) {
            case PC_EXCLUSIVE_ACCESS:
                dev->definitely_exclusive = cmd->exclusive_cmd.bExclusive;
                break;
            case PC_ENTER_CMD_XIP:
            case PC_EXIT_XIP:
            case PC_READ:
            case PC_WRITE:
                dev->definitely_exclusive = saved_exclusive;
                break;
            default:
                dev->definitely_exclusive = false;
                break;
        }
    }
    return ret;
}

// --------------------------------------------------------------------------
// Public command wrappers
// --------------------------------------------------------------------------

int picoboot_exclusive_access(uint8_t daddr, uint8_t exclusive) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId                 = PC_EXCLUSIVE_ACCESS;
    cmd.bCmdSize               = sizeof(struct picoboot_exclusive_cmd);
    cmd.dTransferLength        = 0;
    cmd.exclusive_cmd.bExclusive = exclusive;
    return _picoboot_cmd(daddr, &cmd, NULL, 0);
}

int picoboot_exit_xip(uint8_t daddr) {
    picoboot_dev_t *dev = &_devs[daddr];
    if (dev->definitely_exclusive && dev->xip_state == XIP_INACTIVE) return 0;
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId          = PC_EXIT_XIP;
    cmd.bCmdSize        = 0;
    cmd.dTransferLength = 0;
    return _picoboot_cmd(daddr, &cmd, NULL, 0);
}

int picoboot_enter_cmd_xip(uint8_t daddr) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId          = PC_ENTER_CMD_XIP;
    cmd.bCmdSize        = 0;
    cmd.dTransferLength = 0;
    return _picoboot_cmd(daddr, &cmd, NULL, 0);
}

int picoboot_reboot(uint8_t daddr, uint32_t pc, uint32_t sp, uint32_t delay_ms) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId              = PC_REBOOT;
    cmd.bCmdSize            = sizeof(cmd.reboot_cmd);
    cmd.dTransferLength     = 0;
    cmd.reboot_cmd.dPC      = pc;
    cmd.reboot_cmd.dSP      = sp;
    cmd.reboot_cmd.dDelayMS = delay_ms;
    return _picoboot_cmd(daddr, &cmd, NULL, 0);
}

int picoboot_reboot2(uint8_t daddr, struct picoboot_reboot2_cmd *reboot_cmd) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId          = PC_REBOOT2;
    cmd.bCmdSize        = sizeof(cmd.reboot2_cmd);
    cmd.reboot2_cmd     = *reboot_cmd;
    cmd.dTransferLength = 0;
    return _picoboot_cmd(daddr, &cmd, NULL, 0);
}

int picoboot_exec(uint8_t daddr, uint32_t addr) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId                    = PC_EXEC;
    cmd.bCmdSize                  = sizeof(cmd.address_only_cmd);
    cmd.dTransferLength           = 0;
    cmd.address_only_cmd.dAddr    = addr;
    return _picoboot_cmd(daddr, &cmd, NULL, 0);
}

int picoboot_vector(uint8_t daddr, uint32_t addr) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId                 = PC_VECTORIZE_FLASH;
    cmd.bCmdSize               = sizeof(cmd.address_only_cmd);
    cmd.address_only_cmd.dAddr = addr;
    cmd.dTransferLength        = 0;
    return _picoboot_cmd(daddr, &cmd, NULL, 0);
}

int picoboot_flash_erase(uint8_t daddr, uint32_t addr, uint32_t len) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId              = PC_FLASH_ERASE;
    cmd.bCmdSize            = sizeof(cmd.range_cmd);
    cmd.range_cmd.dAddr     = addr;
    cmd.range_cmd.dSize     = len;
    cmd.dTransferLength     = 0;
    return _picoboot_cmd(daddr, &cmd, NULL, 0);
}

int picoboot_write(uint8_t daddr, uint32_t addr, uint8_t *buffer, uint32_t len) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId           = PC_WRITE;
    cmd.bCmdSize         = sizeof(cmd.range_cmd);
    cmd.range_cmd.dAddr  = addr;
    cmd.range_cmd.dSize  = len;
    cmd.dTransferLength  = len;
    return _picoboot_cmd(daddr, &cmd, buffer, len);
}

int picoboot_read(uint8_t daddr, uint32_t addr, uint8_t *buffer, uint32_t len) {
    memset(buffer, 0xaa, len);
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId           = PC_READ;
    cmd.bCmdSize         = sizeof(cmd.range_cmd);
    cmd.range_cmd.dAddr  = addr;
    cmd.range_cmd.dSize  = len;
    cmd.dTransferLength  = len;
    return _picoboot_cmd(daddr, &cmd, buffer, len);
}

int picoboot_get_info(uint8_t daddr, struct picoboot_get_info_cmd *get_info_cmd,
                      uint8_t *buffer, uint32_t len) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId           = PC_GET_INFO;
    cmd.bCmdSize         = sizeof(cmd.get_info_cmd);
    cmd.get_info_cmd     = *get_info_cmd;
    cmd.dTransferLength  = len;
    return _picoboot_cmd(daddr, &cmd, buffer, len);
}

int picoboot_otp_write(uint8_t daddr, struct picoboot_otp_cmd *otp_cmd,
                       uint8_t *buffer, uint32_t len) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId          = PC_OTP_WRITE;
    cmd.bCmdSize        = sizeof(cmd.otp_cmd);
    cmd.otp_cmd         = *otp_cmd;
    cmd.dTransferLength = len;
    return _picoboot_cmd(daddr, &cmd, buffer, len);
}

int picoboot_otp_read(uint8_t daddr, struct picoboot_otp_cmd *otp_cmd,
                      uint8_t *buffer, uint32_t len) {
    struct picoboot_cmd cmd = {0};
    cmd.bCmdId          = PC_OTP_READ;
    cmd.bCmdSize        = sizeof(cmd.otp_cmd);
    cmd.otp_cmd         = *otp_cmd;
    cmd.dTransferLength = len;
    return _picoboot_cmd(daddr, &cmd, buffer, len);
}

// --------------------------------------------------------------------------
// Peek / poke via EXEC
//
// These work by writing a small ARM Thumb snippet to the target device's SRAM
// at PEEK_POKE_CODE_LOC and executing it. The target runs the code on its own
// CPU. The routines are RP2040 / ARM-mode RP2350 only.
//
// Code blobs from picotool (picoboot_connection/picoboot_connection.c).
// --------------------------------------------------------------------------

#define PEEK_POKE_CODE_LOC  0x20000000u

// STR: write `data` to `addr` on the target
//   ldr r0, [pc, #4]   @ data
//   ldr r1, [pc, #8]   @ addr
//   str r0, [r1, #0]
//   bx  lr
static const uint8_t _poke_code[] = {
    0x01, 0x48, 0x02, 0x49, 0x08, 0x60, 0x70, 0x47
};
// Append: uint32_t data, uint32_t addr
#define POKE_PROG_SIZE (sizeof(_poke_code) + 8u)

// LDR and store result back to (pc+4) so the host can read it
//   ldr r0, [pc, #8]   @ src addr
//   ldr r0, [r0, #0]
//   mov r1, pc
//   str r0, [r1, #4]
//   bx  lr
//   nop
static const uint8_t _peek_code[] = {
    0x02, 0x48, 0x00, 0x68, 0x79, 0x46, 0x48, 0x60, 0x70, 0x47, 0xc0, 0x46
};
// Append: uint32_t src_addr
#define PEEK_PROG_SIZE (sizeof(_peek_code) + 4u)

// DMA-accessible buffers for poke/peek programs
CFG_TUH_MEM_SECTION CFG_TUH_MEM_ALIGN static uint8_t _exec_buf[PEEK_PROG_SIZE > POKE_PROG_SIZE
                                                                 ? PEEK_PROG_SIZE : POKE_PROG_SIZE];

int picoboot_poke(uint8_t daddr, uint32_t addr, uint32_t data) {
    memcpy(_exec_buf, _poke_code, sizeof(_poke_code));
    memcpy(_exec_buf + sizeof(_poke_code),     &data, 4);
    memcpy(_exec_buf + sizeof(_poke_code) + 4, &addr, 4);
    int ret = picoboot_write(daddr, PEEK_POKE_CODE_LOC, _exec_buf, POKE_PROG_SIZE);
    if (ret) return ret;
    return picoboot_exec(daddr, PEEK_POKE_CODE_LOC);
}

int picoboot_peek(uint8_t daddr, uint32_t addr, uint32_t *data) {
    memcpy(_exec_buf, _peek_code, sizeof(_peek_code));
    memcpy(_exec_buf + sizeof(_peek_code), &addr, 4);
    int ret = picoboot_write(daddr, PEEK_POKE_CODE_LOC, _exec_buf, PEEK_PROG_SIZE);
    if (ret) return ret;
    ret = picoboot_exec(daddr, PEEK_POKE_CODE_LOC);
    if (ret) return ret;
    // Result is written by the snippet at (code_loc + sizeof(_peek_code))
    CFG_TUH_MEM_SECTION CFG_TUH_MEM_ALIGN static uint8_t _peek_result[4];
    ret = picoboot_read(daddr,
                        PEEK_POKE_CODE_LOC + (uint32_t)sizeof(_peek_code),
                        _peek_result, 4);
    if (ret == 0) memcpy(data, _peek_result, 4);
    return ret;
}
