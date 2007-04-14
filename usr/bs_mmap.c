/*
 * mmap file backing store routine
 *
 * Copyright (C) 2006-2007 FUJITA Tomonori <tomof@acm.org>
 * Copyright (C) 2006-2007 Mike Christie <michaelc@cs.wisc.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "list.h"
#include "util.h"
#include "tgtd.h"
#include "scsi.h"

static int bs_mmap_open(struct scsi_lu *lu, char *path, int *fd, uint64_t *size)
{
	*fd = backed_file_open(path, O_RDWR| O_LARGEFILE, size);

	return *fd >= 0 ? 0 : *fd;
}

static void bs_mmap_close(struct scsi_lu *lu)
{
	close(lu->fd);
}

#define pgcnt(size, offset)	((((size) + ((offset) & (pagesize - 1))) + (pagesize - 1)) >> pageshift)

static int bs_mmap_cmd_submit(struct scsi_cmd *cmd)
{
	int fd = cmd->dev->fd, ret = 0;
	void *p;

	if (cmd->scb[0] == SYNCHRONIZE_CACHE ||
	    cmd->scb[0] == SYNCHRONIZE_CACHE_16)
		return fsync(fd);

	if (cmd->uaddr)
		cmd->uaddr += cmd->offset;
	else {
		p = mmap64(NULL, pgcnt(cmd->len, cmd->offset) << pageshift,
			   PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			   cmd->offset & ~((1ULL << pageshift) - 1));

		cmd->uaddr = (unsigned long) p + (cmd->offset & (pagesize - 1));
		if (p == MAP_FAILED) {
			ret = -EINVAL;
			eprintf("%" PRIx64 " %u %" PRIu64 "\n", cmd->uaddr,
				cmd->len, cmd->offset);
		}
	}

	dprintf("%" PRIx64 " %u %" PRIu64 "\n", cmd->uaddr, cmd->len, cmd->offset);

	return ret;
}

static int bs_mmap_cmd_done(struct scsi_cmd *cmd)
{
	int err = 0;
	uint64_t uaddr = cmd->uaddr;
	int len = cmd->len;

	dprintf("%d %" PRIx64 " %d\n", cmd->mmapped, uaddr, len);

	if (cmd->mmapped) {
		len = pgcnt(len, (uaddr & (pagesize - 1))) << pageshift;
		uaddr &= ~(pagesize - 1);
		err = munmap((void *) (unsigned long) uaddr, len);
		if (err)
			eprintf("%" PRIx64 " %d\n", uaddr, len);
	} else
		free((void *) (unsigned long) uaddr);

	return err;
}

struct backingstore_template mmap_bst = {
	.bs_open		= bs_mmap_open,
	.bs_close		= bs_mmap_close,
	.bs_cmd_submit		= bs_mmap_cmd_submit,
	.bs_cmd_done		= bs_mmap_cmd_done,
};