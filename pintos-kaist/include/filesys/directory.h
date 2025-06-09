#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/disk.h"
#include "off_t.h"

/* Maximum length of a file name component.
 * This is the traditional UNIX maximum length.
 * After directories are implemented, this maximum length may be
 * retained, but much longer full path names must be allowed. */
#define NAME_MAX 14

struct inode;

/* Opening and closing directories. */
bool dir_create(disk_sector_t sector, size_t entry_cnt);
struct dir *dir_open(struct inode *);
struct dir *dir_open_root(void);
struct dir *dir_reopen(struct dir *);
void dir_close(struct dir *);
struct inode *dir_get_inode(struct dir *);

/* Reading and writing. */
bool dir_lookup(const struct dir *, const char *name, struct inode **);
bool dir_add(struct dir *, const char *name, disk_sector_t);
bool dir_remove(struct dir *, const char *name);
bool dir_readdir(struct dir *, char name[NAME_MAX + 1]);

/* 디렉터리 구조체. */
struct dir
{
    struct inode *inode; /* 저장소 역할을 하는 inode. */
    off_t pos;           /* 현재 위치. */
};

struct dir_entry
{
    disk_sector_t inode_sector; /* 헤더가 위치한 섹터 번호. */
    char name[NAME_MAX + 1];    /* 널 종료된 파일 이름. */
    bool in_use;                /* 사용 중인지 여부. */
};

#endif /* filesys/directory.h */
