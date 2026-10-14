
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "hardware/structs/powman.h"
#include "hardware/flash.h"


#ifndef UPDATE_CHECK_INTERVAL_MINS
#define UPDATE_CHECK_INTERVAL_MINS 1
#endif

#define UPDATE_CHECK_INTERVAL_MS (UPDATE_CHECK_INTERVAL_MINS * 60 * 1000)


int main() {
    stdio_init_all();

    printf("Hello from chained image v%d.%d\n", VERSION_MAJOR, VERSION_MINOR);

    if (rom_get_last_boot_type_with_chained_flag() == BOOT_TYPE_FLASH_UPDATE) {
        printf("Flash update detected - running self checks\n");
        // put self checks here
        bool self_checks_passed = true;
        if (self_checks_passed) {
            printf("Self checks passed - chaining to report success\n");
            goto chain_to_update;
        } else {
            printf("Self checks failed - rebooting without buying\n");
            rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL, 500, 0, 0);
            while (true)
                sleep_ms(100);
        }
    }

    absolute_time_t update_check_time = make_timeout_time_ms(UPDATE_CHECK_INTERVAL_MS);
    printf("Running for %d mins then checking for update\n", UPDATE_CHECK_INTERVAL_MINS);

    while (!time_reached(update_check_time)) {
        printf("Doing something...\n");
        sleep_ms(10000);
    }

chain_to_update:

    uint64_t chain_id = 0x636e6374;
    printf("Chaining ID %" PRIx64 "\n", chain_id);
    uint32_t* workarea = malloc(0x1000);
    int picked_p = rom_pick_ab_partition_by_id(workarea, 0x1000, chain_id, false);
    free(workarea);

    uint32_t buffer[4 + 1] = {}; // single partition, with maximum of 4 words returned, plus 1
    int ret = rom_get_partition_table_info(buffer, count_of(buffer), PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | (picked_p << 24));
    if (
        buffer[0] != (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION)
        || ret != 3
    ) {
        printf("rom_get_partition_table_info returned %d with buffer[0] %08x\n", ret, buffer[0]);
        return ret;
    }

    uint32_t location_and_permissions = buffer[1];
    uint32_t saddr = ((location_and_permissions >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) & 0x1fffu) * FLASH_SECTOR_SIZE;
    uint32_t eaddr = (((location_and_permissions >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB) & 0x1fffu) + 1) * FLASH_SECTOR_SIZE;

    int32_t chain_addr = XIP_BASE + saddr;
    if (rom_get_last_boot_type_with_chained_flag() == BOOT_TYPE_FLASH_UPDATE) {
        boot_info_t boot_info = {};
        int ret = rom_get_boot_info(&boot_info);
        if (ret) {
            uint32_t flash_update_base = boot_info.reboot_params[0];
            if (flash_update_base == (uint32_t)chain_addr) {
                chain_addr = -chain_addr;
            }
        }
    }

    printf("Chaining to %08x%s\n", abs(chain_addr), chain_addr < 0 ? " (update)" : "");
    stdio_deinit_all();
    rom_chain_image_no_return(chain_addr, eaddr - saddr);
}
