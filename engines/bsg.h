/*
 * bsg structure declarations and helper functions for the
 * io_uring_cmd engine.
 */

#include <linux/bsg.h>
#include <poll.h>
#include "../fio.h"

#define MAX_SB 64
#define MAX_10CDB_LBA  0xFFFFFFFFULL
#define MAX_10CDB_NLB  0xFFFFU

enum bsg_io_opcode {
	bsg_cmd_read_10			= 0x28,
	bsg_cmd_read_capacity_10	= 0x25,
	bsg_cmd_sync_cache_10		= 0x35,
	bsg_cmd_unmap			= 0x42,
	bsg_cmd_write_10		= 0x2A,
};

struct bsg_cmd {
	unsigned char cdb[16];
	unsigned char sb[MAX_SB];
	uint8_t unmap_param[24];
};

struct bsg_data {
	unsigned int bs;
};

int fio_bsg_uring_cmd_read_capacity(struct thread_data *td, unsigned int *bs,
				    unsigned long long *max_lba);

int fio_bsg_uring_cmd_get_file_size(struct thread_data *td, struct fio_file *f);

int fio_bsg_uring_cmd_prep(struct bsg_uring_cmd *cmd, struct io_u *io_u,
			   struct bsg_cmd *bc, bool fua);
