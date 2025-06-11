#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

static struct file *free_map_file; /* 프리맵 파일. */
static struct bitmap *free_map;	   /* 프리맵: 디스크 섹터당 한 비트. */

/* 프리맵을 초기화합니다. */
void free_map_init(void)
{
	free_map = bitmap_create(disk_size(filesys_disk));
	if (free_map == NULL)
		PANIC("bitmap creation failed--disk is too large");
	bitmap_mark(free_map, FREE_MAP_SECTOR);
	bitmap_mark(free_map, ROOT_DIR_SECTOR);
}

/* 프리맵에서 연속된 CNT개의 섹터를 할당하여 첫 번째 섹터 번호를
 * *SECTORP에 저장합니다. 공간이 충분하면 true, 부족하면 false를
 * 반환합니다. */
bool free_map_allocate(size_t cnt, disk_sector_t *sectorp)
{
	disk_sector_t sector = bitmap_scan_and_flip(free_map, 0, cnt, false);
	if (sector != BITMAP_ERROR && free_map_file != NULL && !bitmap_write(free_map, free_map_file))
	{
		bitmap_set_multiple(free_map, sector, cnt, false);
		sector = BITMAP_ERROR;
	}
	if (sector != BITMAP_ERROR)
		*sectorp = sector;
	return sector != BITMAP_ERROR;
}

/* SECTOR부터 CNT개의 섹터를 사용 가능하도록 표시합니다. */
void free_map_release(disk_sector_t sector, size_t cnt)
{
	ASSERT(bitmap_all(free_map, sector, cnt));
	bitmap_set_multiple(free_map, sector, cnt, false);
	bitmap_write(free_map, free_map_file);
}

/* 프리맵 파일을 열어 디스크에서 읽어옵니다. */
void free_map_open(void)
{
	free_map_file = file_open(inode_open(FREE_MAP_SECTOR));
	if (free_map_file == NULL)
		PANIC("can't open free map");
	if (!bitmap_read(free_map, free_map_file))
		PANIC("can't read free map");
}

/* 프리맵을 디스크에 기록한 뒤 프리맵 파일을 닫습니다. */
void free_map_close(void)
{
	file_close(free_map_file);
}

/* 디스크에 새로운 프리맵 파일을 생성하고 그 안에 프리맵을 기록합니다. */
void free_map_create(void)
{
	/* inode 생성. */
	if (!inode_create(FREE_MAP_SECTOR, bitmap_file_size(free_map), false))
		PANIC("free map creation failed");

	/* 비트맵을 파일에 기록. */
	free_map_file = file_open(inode_open(FREE_MAP_SECTOR));
	if (free_map_file == NULL)
		PANIC("can't open free map");
	if (!bitmap_write(free_map, free_map_file))
		PANIC("can't write free map");
}
