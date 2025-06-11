#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"
#include "filesys/directory.h"

struct bitmap;

void inode_init(void);
bool inode_create(disk_sector_t, off_t, bool);
struct inode *inode_open(disk_sector_t);
struct inode *inode_reopen(struct inode *);
disk_sector_t inode_get_inumber(const struct inode *);
void inode_close(struct inode *);
void inode_remove(struct inode *);
off_t inode_read_at(struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at(struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write(struct inode *);
void inode_allow_write(struct inode *);
off_t inode_length(const struct inode *);
void inode_flush(struct inode *);
bool is_dir(struct inode *);
disk_sector_t get_dir_sector(struct dir *);
bool is_good_inode(struct inode *);
bool is_root_dir(struct dir *);
bool is_same_dir(struct dir *dir1, struct dir *dir2);
disk_sector_t get_inode_sector(struct inode *inode);
bool is_dir_removed(struct dir *dir);

#endif /* filesys/inode.h */
