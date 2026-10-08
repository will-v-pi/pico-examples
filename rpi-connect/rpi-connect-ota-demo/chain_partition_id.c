
#include <stdio.h>
#include <inttypes.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "hardware/flash.h"
#include "hardware/structs/powman.h"


static volatile uint64_t * chain_partition_id_ptr = (uint64_t*)(powman_hw->scratch);

const uint32_t workarea_size = 0x1000;
static uint32_t * workarea = (uint32_t*)(SRAM_SCRATCH_Y_BASE - workarea_size);

int main() {
    stdio_init_all();

    uint64_t chain_partition_id = *chain_partition_id_ptr;

    printf("Chaining to partition ID %" PRIx64 "\n", chain_partition_id);

    uint32_t buffer[(16 * 4) + 1] = {}; // maximum of 16 partitions, each with maximum of 4 words returned, plus 1
    int ret = rom_get_partition_table_info(buffer, count_of(buffer), PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_PARTITION_ID);

    hard_assert(buffer[0] == (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_PARTITION_ID));

    if (ret > 0) {
        int i = 1;
        int p = 0;
        int picked_p = -1;
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

        if (picked_p >= 0) {
            printf("Chaining to A/B pair %d\n", picked_p);
            #ifdef __riscv
                // Increased bootrom stack is required for this function
                bootrom_stack_t stack = {
                    .base = malloc(0x400),
                    .size = 0x400
                };
                rom_set_bootrom_stack(&stack);
            #endif
            int check_p = rom_pick_ab_partition_during_update(workarea, workarea_size, picked_p);
            if (check_p == BOOTROM_ERROR_INVALID_DATA) {
                // verification will be done it chain_image,
                // so fine to use unverified A partition
            } else {
                picked_p = check_p;
            }
            #ifdef __riscv
                // Reset bootrom stack
                rom_set_bootrom_stack(&stack);
                free(stack.base);
            #endif

            if (picked_p < 0) {
                if (picked_p == BOOTROM_ERROR_NOT_FOUND) {
                    printf("Chosen partition failed verification\n");
                } else if (picked_p == BOOTROM_ERROR_NOT_PERMITTED) {
                    printf("Too many update boots going on at once, so cannot choose partition\n");
                }
                return false;
            }

            printf("Chosen partition %d\n", picked_p);
            int ret = rom_get_partition_table_info(buffer, count_of(buffer), PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | (picked_p << 24));
            hard_assert(buffer[0] == (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION));
            hard_assert(ret == 3);

            uint32_t location_and_permissions = buffer[1];
            uint32_t saddr = ((location_and_permissions >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) & 0x1fffu) * FLASH_SECTOR_SIZE;
            uint32_t eaddr = (((location_and_permissions >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB) & 0x1fffu) + 1) * FLASH_SECTOR_SIZE;

            printf("start %08x, end %08x\n", saddr, eaddr);
            
            // chain
            stdio_deinit_all();
            ret = rom_chain_image((uint8_t*)workarea, workarea_size, saddr + XIP_BASE, eaddr - saddr);
            stdio_init_all();

            printf("rom_chain_image failed %d\n", ret);
            return -1;
        } else {
            printf("No partition found with that ID\n");
            return -1;
        }
    } else {
        printf("No partition table, so cannot chain into partition - get_partition_table_info returned %d\n", ret);
        return -1;
    }

    return 0;
}
