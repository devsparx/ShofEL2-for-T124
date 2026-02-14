#ifndef _EMMC_SERVER_H_
#define _EMMC_SERVER_H_

#if __arm__
    typedef u32 uint32_t;
#else
    #include <stdint.h>
#endif

/* eMMC server protocol commands */
#define EMMC_CMD_READ       0x01
#define EMMC_CMD_WRITE      0x02
#define EMMC_CMD_STATUS     0x03
#define EMMC_CMD_EXIT       0xFF

/* Transfer chunk sizes */
#define EMMC_CHUNK_SECTORS  8
#define EMMC_SECTOR_SIZE    512
#define EMMC_CHUNK_BYTES    (EMMC_CHUNK_SECTORS * EMMC_SECTOR_SIZE)

/* Command structure sent from PC to payload */
struct emmc_cmd_s {
    uint32_t op;
    uint32_t start_sector;
    uint32_t num_sectors;
};

#endif
