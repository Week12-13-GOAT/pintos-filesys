#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/fat.h"
#include "devices/disk.h"

/* 파일 시스템을 담고 있는 디스크. */
struct disk *filesys_disk;

static void do_format(void);

/* 파일 시스템 모듈을 초기화합니다. FORMAT이 true라면 파일 시스템을 재포맷합니다. */
void filesys_init(bool format)
{
	filesys_disk = disk_get(0, 1);
	if (filesys_disk == NULL)
		PANIC("hd0:1 (hdb) not present, file system initialization failed");

	inode_init();

#ifdef EFILESYS
	fat_init();

	if (format)
		do_format();

	fat_open();
#else
	/* 기존 파일 시스템 */
	free_map_init();

	if (format)
		do_format();

	free_map_open();
#endif
}

/* 파일 시스템 모듈을 종료하며 남아 있는 데이터를 디스크에 기록합니다. */
void filesys_done(void)
{
	/* 기존 파일 시스템 */
#ifdef EFILESYS
	fat_close();
#else
	free_map_close();
#endif
}

/* NAME 이름으로 INITIAL_SIZE 크기의 파일을 생성합니다.
 * 성공하면 true, 실패하면 false를 반환합니다.
 * 같은 이름의 파일이 이미 존재하거나 내부 메모리 할당에 실패하면 실패합니다. */
bool filesys_create(const char *name, off_t initial_size)
{
	// printf("FILE NAME :%s\n", name);
	disk_sector_t inode_sector = 0;
	struct dir *dir = dir_open_root();
	bool success = (dir != NULL && name != NULL && strlen(name) <= 14 && fat_allocate(1, &inode_sector) && inode_create(inode_sector, initial_size) && dir_add(dir, name, inode_sector));
	if (!success && inode_sector != 0)
		printf("fail to filesys_create !!\n");
	// free_map_release(inode_sector, 1);
	dir_close(dir);

	// printf("sucees? :%d\n", success);
	return success;
}

/* 주어진 이름을 가진 파일을 연다.
파일을 여는데 성공하면 새로운 파일을 반환하고, 실패하면 널포인터를 반환.
name인 파일이 존재하지 않는 경우에, 또는 내부 메모리 할당에 실패할 경우 실패한다.
*/
struct file *
filesys_open(const char *name)
{
	struct dir *dir = dir_open_root();
	struct inode *inode = NULL;
	bool exist = 0;

	if (dir != NULL)
		exist = dir_lookup(dir, name, &inode);
	dir_close(dir);

	if (!exist)
	{
		return NULL;
	}

	return file_open(inode);
}

/* NAME 파일을 삭제합니다.
 * 성공하면 true, 실패하면 false를 반환합니다.
 * NAME 파일이 존재하지 않거나 내부 메모리 할당에 실패하면 실패합니다. */
bool filesys_remove(const char *name)
{
	struct dir *dir = dir_open_root();
	bool success = dir != NULL && dir_remove(dir, name);
	dir_close(dir);

	return success;
}

/* 파일 시스템을 포맷합니다. */
static void
do_format(void)
{
	printf("Formatting file system...");

#ifdef EFILESYS
	/* FAT을 생성하여 디스크에 저장합니다. */
	fat_create();
	fat_close();
	create_root_dir_inode();
#else
	free_map_create();
	if (!dir_create(ROOT_DIR_SECTOR, 16))
		PANIC("root directory creation failed");
	free_map_close();
#endif

	printf("done.\n");
}
