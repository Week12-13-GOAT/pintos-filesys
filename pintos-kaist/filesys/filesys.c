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
#include "include/threads/thread.h"

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
	cluster_t inode_clst = 0;
	disk_sector_t inode_sector = 0;

	bool is_root = is_root_path(name);
	char *path_lst[128];
	int path_cnt = parse_path(name, path_lst);
	if (path_cnt == 0)
		return false;
	struct thread *cur = thread_current();
	struct dir *cur_dir;
	if (is_root || cur->cwd == NULL || !is_good_inode(cur->cwd->inode))
		cur_dir = dir_open_root();
	else
		cur_dir = dir_reopen(cur->cwd);

	for (int i = 0; i < path_cnt - 1; i++)
	{
		struct inode *inode = NULL;					   // 더미 inode
		if (!dir_lookup(cur_dir, path_lst[i], &inode)) // 현재 폴더에서 찾기
			return false;
		if (!is_dir(inode))
			return false;
		dir_close(cur_dir);
		cur_dir = dir_open(inode);
	}
	name = path_lst[path_cnt - 1];

	bool success = (cur_dir != NULL && name != NULL && strlen(name) <= 14 && fat_allocate(1, &inode_clst) && inode_create(cluster_to_sector(inode_clst), initial_size, false) && dir_add(cur_dir, name, cluster_to_sector(inode_clst)));
	if (!success && inode_sector != 0)
		dprintf("[%s] fail to filesys_create !!\n", name);

	return success;
}

struct file *
load_file_open(const char *name)
{
	struct dir *dir = thread_current()->cwd;
	if (!dir || !is_good_inode(dir->inode))
	{
		thread_current()->cwd = dir_open_root();
		dir = thread_current()->cwd;
	}
	struct inode *inode = NULL;
	bool exist = 0;

	if (dir != NULL)
		exist = dir_lookup(dir, name, &inode);
	// dir_close(dir);

	if (!exist)
	{
		return NULL;
	}

	return file_open(inode);
}

/* 주어진 이름을 가진 파일을 연다.
파일을 여는데 성공하면 새로운 파일을 반환하고, 실패하면 널포인터를 반환.
name인 파일이 존재하지 않는 경우에, 또는 내부 메모리 할당에 실패할 경우 실패한다.
*/
struct file *
filesys_open(const char *name)
{
	bool is_root = is_root_path(name);
	char cp_name[128];
	memcpy(cp_name, name, strlen(name) + 1);
	char *path_lst[128];
	int path_cnt = parse_path(cp_name, path_lst);
	if (is_root && path_cnt == 0)
		return dir_open_root();

	struct thread *cur = thread_current();
	struct dir *cur_dir;
	if (is_root || cur->cwd == NULL || !is_good_inode(cur->cwd->inode))
		cur_dir = dir_open_root();
	else
		cur_dir = dir_reopen(cur->cwd);

	for (int i = 0; i < path_cnt - 1; i++)
	{
		struct inode *inode = NULL;					   // 더미 inode
		if (!dir_lookup(cur_dir, path_lst[i], &inode)) // 현재 폴더에서 찾기
			return false;
		if (!is_dir(inode))
			return false;
		dir_close(cur_dir);
		cur_dir = dir_open(inode);
		dump_dir(cur_dir);
	}
	struct inode *inode = NULL;
	bool exist = 0;

	if (cur_dir != NULL)
		exist = dir_lookup(cur_dir, path_lst[path_cnt - 1], &inode);
	// dir_close(dir);

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
	bool is_root = is_root_path(name);
	char *path_lst[128];
	int path_cnt = parse_path(name, path_lst);
	if (path_cnt == 0)
		return false;
	struct thread *cur = thread_current();
	struct dir *cur_dir;
	if (is_root || cur->cwd == NULL)
		cur_dir = dir_open_root();
	else
		cur_dir = dir_reopen(cur->cwd);

	for (int i = 0; i < path_cnt - 1; i++)
	{
		struct inode *inode = NULL;					   // 더미 inode
		if (!dir_lookup(cur_dir, path_lst[i], &inode)) // 현재 폴더에서 찾기
			return false;
		if (!is_dir(inode))
			return false;
		dir_close(cur_dir);
		cur_dir = dir_open(inode);
	}

	struct inode *find = NULL;
	char *remove_name = path_lst[path_cnt - 1];
	if (!dir_lookup(cur_dir, remove_name, &find))
		return false;
	struct dir *find_dir = dir_open(find);
	if (is_same_dir(find_dir, cur->cwd))
	{
		dir_close(find_dir);
		dir_close(cur_dir);
		return false;
	}

	dir_close(find_dir);
	bool success = cur_dir != NULL && dir_remove(cur_dir, remove_name);
	dir_close(cur_dir);

	if (is_good_inode(find) && is_dir(find))
		return false;

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

void dump_root_dir(void)
{
#ifdef DEBUG_LOG
	struct dir *dir = dir_open_root();
	if (!dir)
	{
		printf("[DEBUG] Root dir open failed\n");
		return;
	}

	struct dir_entry e;
	off_t ofs = 0;
	int slot = 0;

	printf("======= ROOT DIR DUMP =======\n");
	while (inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e)
	{
		printf("slot %02d : in_use=%d  name='%s'  sector=%u\n",
			   slot, e.in_use, e.name, e.inode_sector);
		ofs += sizeof e;
		slot++;
	}
	printf("======= END DUMP ==========\n");

	dir_close(dir);
#endif
}

int parse_path(char *target, char *argv[])
{
	int argc = 0;
	char *token;
	char *save_ptr; // 파싱 상태를 저장할 변수!

	for (token = strtok_r(target, "/", &save_ptr);
		 token != NULL;
		 token = strtok_r(NULL, "/", &save_ptr))
	{
		argv[argc++] = token; // 각 인자의 포인터 저장
	}
	argv[argc] = NULL; // 마지막에 NULL로 끝맺기(C 관례)

	return argc;
}

bool is_root_path(const char *path)
{
	return path[0] == '/';
}