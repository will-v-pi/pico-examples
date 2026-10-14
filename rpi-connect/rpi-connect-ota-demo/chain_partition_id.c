
#include <stdio.h>
#include <inttypes.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "hardware/flash.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/mpu.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"


#ifndef CHAIN_PARTITION_ID_SCRATCH
#define CHAIN_PARTITION_ID_SCRATCH 0
#endif

#ifndef CHAIN_PARTITION_WORKAREA_SCRATCH
#define CHAIN_PARTITION_WORKAREA_SCRATCH 2
#endif

#ifndef CHAIN_PARTITION_RETURN_CODE_SCRATCH
#define CHAIN_PARTITION_RETURN_CODE_SCRATCH 3
#endif

// Error code reported (with the stage) on a HardFault
#ifndef CHAIN_PARTITION_HARDFAULT_RETURN_CODE
#define CHAIN_PARTITION_HARDFAULT_RETURN_CODE (-0x100)
#endif

static_assert(
    (CHAIN_PARTITION_ID_SCRATCH + 1 < count_of(powman_hw->scratch))
    && (CHAIN_PARTITION_ID_SCRATCH >= 0),
    "CHAIN_PARTITION_ID_SCRATCH is not the first of a valid pair of powman scratch registers"
);
static_assert(
    (CHAIN_PARTITION_WORKAREA_SCRATCH < count_of(powman_hw->scratch))
    && (CHAIN_PARTITION_WORKAREA_SCRATCH >= 0),
    "CHAIN_PARTITION_WORKAREA_SCRATCH is not a valid powman scratch register"
);
static_assert(
    (CHAIN_PARTITION_WORKAREA_SCRATCH < CHAIN_PARTITION_ID_SCRATCH)
    || (CHAIN_PARTITION_WORKAREA_SCRATCH > CHAIN_PARTITION_ID_SCRATCH + 1),
    "CHAIN_PARTITION_ID_SCRATCH and CHAIN_PARTITION_WORKAREA_SCRATCH overlap"
);
static_assert(
    (CHAIN_PARTITION_RETURN_CODE_SCRATCH < count_of(powman_hw->scratch))
    && (CHAIN_PARTITION_RETURN_CODE_SCRATCH >= 0),
    "CHAIN_PARTITION_RETURN_CODE_SCRATCH is not a valid powman scratch register"
);
static_assert(
    (CHAIN_PARTITION_RETURN_CODE_SCRATCH != CHAIN_PARTITION_WORKAREA_SCRATCH)
    && ((CHAIN_PARTITION_RETURN_CODE_SCRATCH < CHAIN_PARTITION_ID_SCRATCH)
        || (CHAIN_PARTITION_RETURN_CODE_SCRATCH > CHAIN_PARTITION_ID_SCRATCH + 1)),
    "CHAIN_PARTITION_RETURN_CODE_SCRATCH overlaps CHAIN_PARTITION_ID_SCRATCH or CHAIN_PARTITION_WORKAREA_SCRATCH"
);

// MPU region used to write-protect this binary while the bootrom loads the chained image.
// rom_chain_image doesn't touch the MPU, so this region is still enabled when the chained
// image is entered - the chained image must disable it in crt0 before loading anything
#ifndef CHAIN_PARTITION_MPU_REGION
#define CHAIN_PARTITION_MPU_REGION 0
#endif

extern char __logical_binary_start[];
extern char __StackLimit[]; // end of the RAM region (the stack is separate, so not protected)

static void mpu_protect_self(void) {
    // MPU regions have 32 byte granularity, so RAM_ORIGIN and RAM_LENGTH must be 32 byte aligned
    uintptr_t base = (uintptr_t)__logical_binary_start;
    uintptr_t end = (uintptr_t)__StackLimit;

    // AttrIndx 0 = Normal memory, non-cacheable (the reset value is Device, which is execute-never)
    mpu_hw->mair[0] = 0x44;
    mpu_hw->rnr = CHAIN_PARTITION_MPU_REGION;
    // read-only (privileged only), executable
    mpu_hw->rbar = base | (2u << M33_MPU_RBAR_AP_LSB);
    mpu_hw->rlar = (end - 32) | M33_MPU_RLAR_EN_BITS;
    mpu_hw->ctrl = M33_MPU_CTRL_PRIVDEFENA_BITS | M33_MPU_CTRL_ENABLE_BITS;
    __dsb();
    __isb();
}

static void mpu_unprotect_self(void) {
    mpu_hw->ctrl = 0;
    __dsb();
    __isb();
}

// Store the return code in a powman scratch register (which survives a watchdog reset), then reboot
static void __attribute__((noreturn)) reboot_with_return_code(int status) {
    powman_hw->scratch[CHAIN_PARTITION_RETURN_CODE_SCRATCH] = (uint32_t)status;
    // regular boot, rather than a watchdog vector boot
    watchdog_hw->scratch[4] = 0;
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    while (true) {
        __wfi();
    }
}

// Stages, reported in the upper 16 bits of the return code
enum chain_stage {
    CHAIN_STAGE_STARTUP = 0,            // before main, or main never set a stage
    CHAIN_STAGE_GET_PARTITION_TABLE,    // rom_get_partition_table_info for all partitions
    CHAIN_STAGE_FIND_PARTITION,         // searching for a partition with the ID
    CHAIN_STAGE_PICK_AB_PARTITION,      // rom_pick_ab_partition_during_update
    CHAIN_STAGE_GET_PARTITION_INFO,     // rom_get_partition_table_info for the picked partition
    CHAIN_STAGE_CHAIN_IMAGE,            // rom_chain_image
};

// Return code written to CHAIN_PARTITION_RETURN_CODE_SCRATCH is (stage << 16) | (error code & 0xffff)
// Error codes are BOOTROM_ERROR_* values, or CHAIN_PARTITION_HARDFAULT_RETURN_CODE
#define CHAIN_PARTITION_RETURN_CODE(stage, rc) (((uint32_t)(stage) << 16) | ((uint32_t)(rc) & 0xffffu))

// Only written before the MPU protection is enabled
static volatile enum chain_stage stage;

// Called when main returns
void __attribute__((noreturn)) _exit(int status) {
    reboot_with_return_code(status);
}

// The chained image tried to load over this binary - MemManage is disabled, so the
// MPU fault escalates to HardFault. The MPU is ignored in the HardFault handler, so
// the scratch register can still be written.
void isr_hardfault(void) {
    reboot_with_return_code(CHAIN_PARTITION_RETURN_CODE(stage, CHAIN_PARTITION_HARDFAULT_RETURN_CODE));
}

static int chain_partition_id(void) {
    // partition ID in CHAIN_PARTITION_ID_SCRATCH & CHAIN_PARTITION_ID_SCRATCH+1
    uint64_t chain_partition_id = *(uint64_t*)(&powman_hw->scratch[CHAIN_PARTITION_ID_SCRATCH]);

    // workarea in CHAIN_PARTITION_WORKAREA_SCRATCH
    const uint32_t workarea_size = 0xd00;
    uint32_t * workarea = (uint32_t*)(powman_hw->scratch[CHAIN_PARTITION_WORKAREA_SCRATCH]);

    stage = CHAIN_STAGE_GET_PARTITION_TABLE;
    uint32_t buffer[(16 * 4) + 1]; // maximum of 16 partitions, each with maximum of 4 words returned, plus 1
    int ret = rom_get_partition_table_info(buffer, count_of(buffer), PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_PARTITION_ID);
    if (ret < 0) return ret;
    if (buffer[0] != (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_PARTITION_ID)) return BOOTROM_ERROR_INVALID_DATA;

    stage = CHAIN_STAGE_FIND_PARTITION;
    int i = 1;
    int p = 0;
    int picked_p = BOOTROM_ERROR_NOT_FOUND;
    while (i < ret) {
        i++;
        uint32_t flags_and_permissions = buffer[i++];
        bool has_id = (flags_and_permissions & PICOBIN_PARTITION_FLAGS_HAS_ID_BITS);
        if (has_id) {
            uint64_t id = 0;
            id |= buffer[i++];
            id |= ((uint64_t)(buffer[i++]) << 32ull);
            if (id == chain_partition_id) {
                picked_p = p;
                break;
            }
        }

        p++;
    }
    if (picked_p < 0) return picked_p;

    stage = CHAIN_STAGE_PICK_AB_PARTITION;
    int check_p = rom_pick_ab_partition_during_update(workarea, workarea_size, picked_p);
    if (check_p == BOOTROM_ERROR_INVALID_DATA) {
        // verification will be done in chain_image,
        // so fine to use unverified A partition
    } else {
        // BOOTROM_ERROR_NOT_FOUND - chosen partition failed verification
        // BOOTROM_ERROR_NOT_PERMITTED - too many update boots going on at once, so cannot choose partition
        if (check_p < 0) return check_p;
        picked_p = check_p;
    }

    stage = CHAIN_STAGE_GET_PARTITION_INFO;
    ret = rom_get_partition_table_info(buffer, count_of(buffer), PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | (picked_p << 24));
    if (ret < 0) return ret;
    if (ret != 3 || buffer[0] != (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION)) return BOOTROM_ERROR_INVALID_DATA;

    uint32_t location_and_permissions = buffer[1];
    uint32_t saddr = ((location_and_permissions >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) & 0x1fffu) * FLASH_SECTOR_SIZE;
    uint32_t eaddr = (((location_and_permissions >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB) & 0x1fffu) + 1) * FLASH_SECTOR_SIZE;

    // chain - only returns on failure
    stage = CHAIN_STAGE_CHAIN_IMAGE;
    mpu_protect_self();
    ret = rom_chain_image((uint8_t*)workarea, workarea_size, saddr + XIP_BASE, eaddr - saddr);
    mpu_unprotect_self();
    return ret;
}

int main() {
    int ret = chain_partition_id();
    return (int)CHAIN_PARTITION_RETURN_CODE(stage, ret);
}
