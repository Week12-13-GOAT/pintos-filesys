#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"

/* 디렉터리 구조체. */
struct dir
{
	struct inode *inode; /* 저장소 역할을 하는 inode. */
	off_t pos;			 /* 현재 위치. */
};

/* 하나의 디렉터리 엔트리. */
struct dir_entry
{
	disk_sector_t inode_sector; /* 헤더가 위치한 섹터 번호. */
	char name[NAME_MAX + 1];	/* 널 종료된 파일 이름. */
	bool in_use;				/* 사용 중인지 여부. */
};

/* 주어진 SECTOR에 ENTRY_CNT개의 엔트리를 저장할 공간을 갖는
 * 디렉터리를 생성합니다. 성공하면 true, 실패하면 false를 반환합니다. */
bool dir_create(disk_sector_t sector, size_t entry_cnt)
{
	return inode_create(sector, entry_cnt * sizeof(struct dir_entry));
}

/* 주어진 INODE에 대한 디렉터리를 열어 반환하며 소유권을 가져옵니다.
 * 실패하면 NULL 포인터를 반환합니다. */
struct dir *
dir_open(struct inode *inode)
{
	struct dir *dir = calloc(1, sizeof *dir);
	if (inode != NULL && dir != NULL)
	{
		dir->inode = inode;
		dir->pos = 0;
		return dir;
	}
	else
	{
		inode_close(inode);
		free(dir);
		return NULL;
	}
}

/* 루트 디렉터리를 열어 그 디렉터리 객체를 반환합니다.
 * 성공하면 true, 실패하면 false를 반환합니다. */
struct dir *
dir_open_root(void)
{
	return dir_open(inode_open(ROOT_DIR_SECTOR));
}

/* DIR과 같은 inode를 사용하는 새 디렉터리를 열어 반환합니다.
 * 실패하면 NULL 포인터를 반환합니다. */
struct dir *
dir_reopen(struct dir *dir)
{
	return dir_open(inode_reopen(dir->inode));
}

/* DIR을 파괴하고 관련 자원을 해제합니다. */
void dir_close(struct dir *dir)
{
	if (dir != NULL)
	{
		inode_close(dir->inode);
		free(dir);
	}
}

/* DIR이 감싸고 있는 inode를 반환합니다. */
struct inode *
dir_get_inode(struct dir *dir)
{
	return dir->inode;
}

/* DIR에서 주어진 NAME을 가진 파일을 검색합니다.
 * 성공하면 true를 반환하며, EP가 NULL이 아니면 해당 엔트리를 *EP에,
 * OFSP가 NULL이 아니면 엔트리의 바이트 오프셋을 *OFSP에 저장합니다.
 * 실패하면 false를 반환하고 EP와 OFSP는 무시됩니다. */
static bool
lookup(const struct dir *dir, const char *name,
	   struct dir_entry *ep, off_t *ofsp)
{
	struct dir_entry e;
	size_t ofs;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

	for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
		 ofs += sizeof e)
		if (e.in_use && !strcmp(name, e.name))
		{
			if (ep != NULL)
				*ep = e;
			if (ofsp != NULL)
				*ofsp = ofs;
			return true;
		}
	return false;
}

/* DIR에서 주어진 NAME을 가진 파일을 검색하여 존재하면 true,
 * 그렇지 않으면 false를 반환합니다. 성공하면 *INODE에 해당 파일의
 * inode를 설정하고, 실패하면 NULL 포인터로 설정합니다.
 * 호출자는 반드시 *INODE를 닫아야 합니다. */
/* 주어진 name 으로 파일을 검색하고, 존재하면 true를, 실패하면 false를 반환합니다.
성공시에, INODE를 파일의 inode로 설정하고, 실패하면 null pointer로 설정합니다.
호출자는 반드시 INODE를 닫아야 합니다. */
bool dir_lookup(const struct dir *dir, const char *name,
				struct inode **inode)
{
	struct dir_entry e;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

	if (lookup(dir, name, &e, NULL))		 // 디렉터리에 name이라는 엔트리가 있는지 검색
		*inode = inode_open(e.inode_sector); // 있다
	else
		*inode = NULL; // 없다.

	return *inode != NULL;
}

/* DIR에 NAME이라는 이름의 파일을 추가합니다. DIR에는 이미 같은 이름의 파일이 없어야 합니다.
 * 파일의 inode는 INODE_SECTOR 섹터에 있습니다.
 * 성공하면 true를, 실패하면 false를 반환합니다.
 * NAME이 유효하지 않거나(즉, 너무 길거나), 디스크 또는 메모리 오류가 발생하면 실패합니다. */
bool dir_add(struct dir *dir, const char *name, disk_sector_t inode_sector)
{
	struct dir_entry e;
	off_t ofs;
	bool success = false;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

	/* NAME의 유효성을 검사한다. */
	if (*name == '\0' || strlen(name) > NAME_MAX)
		return false;

	/* NAME이 이미 사용 중인지 확인한다. */
	if (lookup(dir, name, NULL, NULL))
		goto done;

	/* 빈 슬롯의 오프셋을 OFS에 설정한다.
	 * 빈 슬롯이 없다면 현재 파일 끝 위치가 사용된다.

	 * inode_read_at()은 파일 끝에서만 짧게 읽기를 반환하므로
	 * 그 외의 경우라면 메모리 부족 등 일시적인 이유로 짧게
	 * 읽은 것이 아님을 확인해야 한다. */
	for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
		 ofs += sizeof e)
		if (!e.in_use)
			break;

	/* Write slot. */
	e.in_use = true;
	strlcpy(e.name, name, sizeof e.name);
	e.inode_sector = inode_sector;
	success = inode_write_at(dir->inode, &e, sizeof e, ofs) == sizeof e;

#ifdef DEBUG_LOG // 디버그 용으로 조건부 컴파일 가능
	// 확인용 읽기
	if (success)
	{
		struct dir_entry verify;
		off_t check = inode_read_at(dir->inode, &verify, sizeof verify, ofs);
		if (check != sizeof verify)
		{
			printf("Failed to re-read dir_entry at ofs %d\n", (int)ofs);
		}
		else
		{
			printf("[VERIFY] name: %s, sector: %u, in_use: %d\n",
				   verify.name, verify.inode_sector, verify.in_use);
		}
	}
#endif
done:
	return success;
}

/* DIR에서 NAME에 해당하는 엔트리를 제거한다.
 * 성공하면 true, 실패하면 false를 반환하며,
 * NAME 이름의 파일이 없을 때만 실패한다. */
bool dir_remove(struct dir *dir, const char *name)
{
	struct dir_entry e;
	struct inode *inode = NULL;
	bool success = false;
	off_t ofs;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

	/* Find directory entry. */
	if (!lookup(dir, name, &e, &ofs))
		goto done;

	/* Open inode. */
	inode = inode_open(e.inode_sector);
	if (inode == NULL)
		goto done;

	/* Erase directory entry. */
	e.in_use = false;
	if (inode_write_at(dir->inode, &e, sizeof e, ofs) != sizeof e)
		goto done;

	/* Remove inode. */
	inode_remove(inode);
	success = true;

done:
	inode_close(inode);
	return success;
}

/* DIR에서 다음 엔트리를 읽어 NAME에 저장한다.
 * 성공하면 true를 반환하고, 더 이상 읽을 엔트리가 없으면 false를 반환한다. */
bool dir_readdir(struct dir *dir, char name[NAME_MAX + 1])
{
	struct dir_entry e;

	while (inode_read_at(dir->inode, &e, sizeof e, dir->pos) == sizeof e)
	{
		dir->pos += sizeof e;
		if (e.in_use)
		{
			strlcpy(name, e.name, NAME_MAX + 1);
			return true;
		}
	}
	return false;
}
