#include <os.h>
#include <xmalloc.h>
#include <console.h>
#include <netfront.h>
#include <lwip/api.h>
#include <mini-os/blkfront.h>

#include <stdlib.h>
#include <string.h>

static int disk_fd[1];
static struct blkfront_info blk_info[1];

void
initialize_block_devices(void)
{
  struct blkfront_dev *blk_dev;

  printf("Opening block device\n");

  blk_dev = init_blkfront("device/vbd/769", &(blk_info[0]));
  if (blk_dev) {
    disk_fd[0] = blkfront_open(blk_dev);
  } else {
    disk_fd[0] = -1;
    printf("Block devices #0 not found.\n");
  }
}

int
mocloudos_disk_status(int pdrv)
{
  struct stat buf;

  if (pdrv >= sizeof(disk_fd) / sizeof(disk_fd[0]) || disk_fd[pdrv] < 0) {
    return -1;
  }
  (void) blkfront_posix_fstat(disk_fd[pdrv], &buf);
  return (buf.st_mode & 0200) == 0;
}

static int
mocloudos_disk_rwop(int pdrv, uint8_t *buff, uint32_t sector, uint32_t count, int write)
{
  struct blkfront_dev *blk_dev = files[pdrv].blk.dev;
  off_t offset = blk_info[pdrv].sector_size * sector;
  size_t length = blk_info[pdrv].sector_size * count;

  if (pdrv >= sizeof(disk_fd) / sizeof(disk_fd[0]) || disk_fd[pdrv] < 0) {
    return -1;
  }
  files[pdrv].blk.offset = offset;
  return blkfront_posix_rwop(disk_fd[pdrv], buff, length, write);
}

int
mocloudos_disk_read(int pdrv, uint8_t *buff, uint32_t sector, uint32_t count)
{
  return mocloudos_disk_rwop(pdrv, buff, sector, count, 0);
}

int
mocloudos_disk_write(int pdrv, uint8_t *buff, uint32_t sector, uint32_t count)
{
  return mocloudos_disk_rwop(pdrv, buff, sector, count, 1);
}
