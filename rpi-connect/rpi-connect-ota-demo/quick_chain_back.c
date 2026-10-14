
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "hardware/structs/powman.h"
#include "hardware/flash.h"

int main() {
    stdio_init_all();

    printf("Hello from version 1 of quick chain back\n");

    uint64_t chain_id = 0xbace;
    printf("Chaining ID %" PRIx64 "\n", chain_id);
    uint64_t * chain_id_ptr = (uint64_t*)(&powman_hw->scratch[0]);
    *chain_id_ptr = chain_id;
    printf("Written %" PRIx64 "\n", *chain_id_ptr);
    powman_hw->scratch[2] = USBCTRL_DPRAM_BASE;
    printf("Using workarea %" PRIx32 "\n", powman_hw->scratch[2]);
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
    ret = rom_chain_image(workarea, 0x1000, XIP_BASE + saddr, eaddr - saddr);
    stdio_init_all();
    printf("rom_chain_image returned %d\n", ret);
    free(workarea);

    return ret;
}
