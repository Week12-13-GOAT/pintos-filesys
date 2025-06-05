#include "filesys/file.h"
#include <debug.h>
#include "filesys/inode.h"
#include "threads/malloc.h"

/* 열린 파일을 나타내는 구조체. */
struct file
{
	struct inode *inode; /* 파일의 인노드. */
	off_t pos;			 /* 현재 위치. */
	bool deny_write;	 /* file_deny_write()가 호출되었는지 여부. */
	int dup_count;		 /* extra2 */
};

/* 주어진 INODE를 사용하여 파일을 열고 해당 소유권을 가져온 후
 * 새 파일을 반환합니다. 메모리 할당이 실패하거나 INODE가 NULL이면
 * NULL 포인터를 반환합니다. */

void increase_dup_count(struct file *file)
{
	file->dup_count++;
}

void decrease_dup_count(struct file *file)
{
	file->dup_count--;
}

int check_dup_count(struct file *file)
{
	return file->dup_count;
}

/*
주어진 inode를 가진 file 구조체를 만들어 반환 .
*/
struct file *
file_open(struct inode *inode)
{
	struct file *file = calloc(1, sizeof *file);
	if (inode != NULL && file != NULL)
	{
		file->inode = inode;
		file->pos = 0;
		/* extra 2 */
		file->dup_count = 1;
		file->deny_write = false;
		return file;
	}
	else
	{
		inode_close(inode);
		free(file);
		return NULL;
	}
}

/* FILE과 같은 inode를 사용하는 새 파일을 열어 반환합니다.
 * 실패하면 NULL 포인터를 반환합니다. */
struct file *
file_reopen(struct file *file)
{
	return file_open(inode_reopen(file->inode));
}

/* 파일 객체의 속성을 복제하여 FILE과 같은 inode를 가지는 새 파일을 반환합니다.
 * 실패하면 NULL 포인터를 반환합니다. */
struct file *
file_duplicate(struct file *file)
{
	struct file *nfile = file_open(inode_reopen(file->inode));
	if (nfile)
	{
		nfile->pos = file->pos;
		if (file->deny_write)
			file_deny_write(nfile);
	}
	return nfile;
}

/* FILE을 닫습니다. */
void file_close(struct file *file)
{
	if (file != NULL)
	{
		file_allow_write(file);
		inode_close(file->inode);
		free(file);
	}
}

/* FILE이 감싸고 있는 inode를 반환합니다. */
struct inode *
file_get_inode(struct file *file)
{
	return file->inode;
}

/* 파일(FILE)에서 현재 위치부터 SIZE 바이트를 버퍼(BUFFER)로 읽어옵니다.
 * 실제로 읽은 바이트 수를 반환하며, 파일 끝에 도달하면 SIZE보다 적을 수 있습니다.
 * 파일의 현재 위치(FILE->pos)는 읽은 바이트 수만큼 앞으로 이동합니다. */
off_t file_read(struct file *file, void *buffer, off_t size)
{
	off_t bytes_read = inode_read_at(file->inode, buffer, size, file->pos);
	file->pos += bytes_read;
	return bytes_read;
}

/* 파일(FILE)에서 오프셋(FILE_OFS)부터 SIZE 바이트를 버퍼(BUFFER)로 읽어옵니다.
 * 실제로 읽은 바이트 수를 반환하며, 파일 끝에 도달하면 SIZE보다 적을 수 있습니다.
 * 파일의 현재 위치는 영향을 받지 않습니다. */
off_t file_read_at(struct file *file, void *buffer, off_t size, off_t file_ofs)
{
	return inode_read_at(file->inode, buffer, size, file_ofs);
}

/* 버퍼(BUFFER)의 데이터를 파일(FILE)에 현재 위치부터 SIZE 바이트만큼 기록합니다.
 * 실제로 기록된 바이트 수를 반환하며, 파일 끝에 도달하면 SIZE보다 적을 수 있습니다.
 * (일반적으로 이 경우 파일을 확장해야 하지만, 파일 확장은 아직 구현되지 않았습니다.)
 * 파일의 현재 위치(FILE->pos)는 기록된 바이트 수만큼 앞으로 이동합니다. */
off_t file_write(struct file *file, const void *buffer, off_t size)
{
	off_t bytes_written = inode_write_at(file->inode, buffer, size, file->pos);
	file->pos += bytes_written;
	return bytes_written;
}

/* 버퍼(BUFFER)의 데이터를 파일(FILE)에 파일 오프셋(FILE_OFS)부터 SIZE 바이트만큼 기록합니다.
 * 실제로 기록된 바이트 수를 반환하며, 파일 끝에 도달하면 SIZE보다 적을 수 있습니다.
 * (일반적으로 이 경우 파일을 확장해야 하지만, 파일 확장은 아직 구현되지 않았습니다.)
 * 파일의 현재 위치는 영향을 받지 않습니다. */
off_t file_write_at(struct file *file, const void *buffer, off_t size,
					off_t file_ofs)
{
	return inode_write_at(file->inode, buffer, size, file_ofs);
}

/* file_allow_write()가 호출되거나 FILE이 닫힐 때까지
 * FILE의 inode에 대한 쓰기 작업을 금지합니다. */
void file_deny_write(struct file *file)
{
	ASSERT(file != NULL);
	if (!file->deny_write)
	{
		file->deny_write = true;
		inode_deny_write(file->inode);
	}
}

/* FILE의 inode에 대한 쓰기 작업을 다시 허용합니다.
 * (같은 inode를 열어 놓은 다른 파일에 의해 여전히 거부될 수 있습니다.) */
void file_allow_write(struct file *file)
{
	ASSERT(file != NULL);
	if (file->deny_write)
	{
		file->deny_write = false;
		inode_allow_write(file->inode);
	}
}

/* FILE의 크기를 바이트 단위로 반환합니다. */
off_t file_length(struct file *file)
{
	ASSERT(file != NULL);
	return inode_length(file->inode);
}

/* 파일의 처음부터 NEW_POS 바이트 위치로 현재 위치를 설정합니다. */
void file_seek(struct file *file, off_t new_pos)
{
	ASSERT(file != NULL);
	ASSERT(new_pos >= 0);
	file->pos = new_pos;
}

/* 파일의 처음으로부터의 바이트 오프셋 형태로 FILE의 현재 위치를 반환합니다. */
off_t file_tell(struct file *file)
{
	ASSERT(file != NULL);
	return file->pos;
}
