#include "filesys/fsutil.h"
#include <debug.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "devices/disk.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"

/* 루트 디렉터리의 파일 목록을 출력합니다. */
void
fsutil_ls (char **argv UNUSED) {
	struct dir *dir;
	char name[NAME_MAX + 1];

        printf ("루트 디렉터리의 파일 목록:\n");
	dir = dir_open_root ();
	if (dir == NULL)
		PANIC ("root dir open failed");
	while (dir_readdir (dir, name))
		printf ("%s\n", name);
        printf ("목록 끝.\n");
}

/* 파일 ARGV[1]의 내용을 16진수와 ASCII 형식으로 콘솔에 출력합니다. */
void
fsutil_cat (char **argv) {
	const char *file_name = argv[1];

	struct file *file;
	char *buffer;

        printf ("'%s' 파일을 콘솔에 출력합니다...\n", file_name);
	file = filesys_open (file_name);
	if (file == NULL)
		PANIC ("%s: open failed", file_name);
	buffer = palloc_get_page (PAL_ASSERT);
	for (;;) {
		off_t pos = file_tell (file);
		off_t n = file_read (file, buffer, PGSIZE);
		if (n == 0)
			break;

		hex_dump (pos, buffer, n, true); 
	}
	palloc_free_page (buffer);
	file_close (file);
}

/* 파일 ARGV[1]을 삭제합니다. */
void
fsutil_rm (char **argv) {
	const char *file_name = argv[1];

        printf ("'%s' 파일을 삭제합니다...\n", file_name);
	if (!filesys_remove (file_name))
		PANIC ("%s: delete failed\n", file_name);
}

/* "scratch" 디스크(hdc 또는 hd1:0)에 저장된 데이터를 파일 시스템의
 * ARGV[1] 파일로 복사합니다.
 *
 * 스크래치 디스크의 현재 섹터는 "PUT\0" 문자열과 파일 크기를 나타내는
 * 리틀엔디안 32비트 정수가 먼저 와야 하며, 이후 섹터에는 파일 내용이
 * 저장되어 있어야 합니다.
 *
 * 이 함수를 처음 호출하면 스크래치 디스크의 처음부터 읽기 시작하고,
 * 이후 호출에서는 계속 이어서 읽습니다. 이 위치는 fsutil_get()에서
 * 사용하는 위치와 독립적이므로, 모든 `put`이 `get`보다 먼저 실행되어야
 * 합니다. */
void
fsutil_put (char **argv) {
	static disk_sector_t sector = 0;

	const char *file_name = argv[1];
	struct disk *src;
	struct file *dst;
	off_t size;
	void *buffer;

        printf ("'%s' 파일을 파일 시스템에 복사합니다...\n", file_name);

        /* 버퍼 할당 */
	buffer = malloc (DISK_SECTOR_SIZE);
	if (buffer == NULL)
		PANIC ("couldn't allocate buffer");

        /* 소스 디스크를 열고 파일 크기를 읽어 온다 */
	src = disk_get (1, 0);
	if (src == NULL)
		PANIC ("couldn't open source disk (hdc or hd1:0)");

        /* 파일 크기 읽기 */
	disk_read (src, sector++, buffer);
	if (memcmp (buffer, "PUT", 4))
		PANIC ("%s: missing PUT signature on scratch disk", file_name);
	size = ((int32_t *) buffer)[1];
	if (size < 0)
		PANIC ("%s: invalid file size %d", file_name, size);

        /* 대상 파일 생성 */
	if (!filesys_create (file_name, size))
		PANIC ("%s: create failed", file_name);
	dst = filesys_open (file_name);
	if (dst == NULL)
		PANIC ("%s: open failed", file_name);

        /* 실제 복사 수행 */
	while (size > 0) {
		int chunk_size = size > DISK_SECTOR_SIZE ? DISK_SECTOR_SIZE : size;
		disk_read (src, sector++, buffer);
		if (file_write (dst, buffer, chunk_size) != chunk_size)
			PANIC ("%s: write failed with %"PROTd" bytes unwritten",
					file_name, size);
		size -= chunk_size;
	}

        /* 마무리 작업 */
	file_close (dst);
	free (buffer);
}

/* 파일 시스템의 FILE_NAME 파일을 스크래치 디스크로 복사합니다.
 *
 * 스크래치 디스크의 현재 섹터에는 "GET\0"과 파일 크기(리틀엔디안
 * 32비트 정수)가 기록되고, 이후 섹터에는 파일 데이터가 기록됩니다.
 *
 * 처음 호출하면 스크래치 디스크의 처음부터 쓰기 시작하며, 이후 호출
 * 시에는 계속해서 다음 섹터에 기록합니다. 이 위치는 fsutil_put()과
 * 독립적이므로 모든 `put` 호출이 `get` 호출보다 먼저 이루어져야 합니다. */
void
fsutil_get (char **argv) {
	static disk_sector_t sector = 0;

	const char *file_name = argv[1];
	void *buffer;
	struct file *src;
	struct disk *dst;
	off_t size;

        printf ("파일 시스템에서 '%s' 파일을 가져옵니다...\n", file_name);

        /* 버퍼 할당 */
	buffer = malloc (DISK_SECTOR_SIZE);
	if (buffer == NULL)
		PANIC ("couldn't allocate buffer");

        /* 소스 파일 열기 */
	src = filesys_open (file_name);
	if (src == NULL)
		PANIC ("%s: open failed", file_name);
	size = file_length (src);

        /* 대상 디스크 열기 */
	dst = disk_get (1, 0);
	if (dst == NULL)
		PANIC ("couldn't open target disk (hdc or hd1:0)");

        /* 섹터 0에 크기 기록 */
	memset (buffer, 0, DISK_SECTOR_SIZE);
	memcpy (buffer, "GET", 4);
	((int32_t *) buffer)[1] = size;
	disk_write (dst, sector++, buffer);

        /* 실제 복사 수행 */
	while (size > 0) {
		int chunk_size = size > DISK_SECTOR_SIZE ? DISK_SECTOR_SIZE : size;
		if (sector >= disk_size (dst))
			PANIC ("%s: out of space on scratch disk", file_name);
		if (file_read (src, buffer, chunk_size) != chunk_size)
			PANIC ("%s: read failed with %"PROTd" bytes unread", file_name, size);
		memset (buffer + chunk_size, 0, DISK_SECTOR_SIZE - chunk_size);
		disk_write (dst, sector++, buffer);
		size -= chunk_size;
	}

        /* 마무리 작업 */
	file_close (src);
	free (buffer);
}
