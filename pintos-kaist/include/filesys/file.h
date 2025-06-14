#ifndef FILESYS_FILE_H
#define FILESYS_FILE_H

#include "filesys/off_t.h"
#include "include/devices/disk.h"

struct inode;

/* Opening and closing files. */
struct file *file_open(struct inode *);
struct file *file_reopen(struct file *);
struct file *file_duplicate(struct file *file);
void file_close(struct file *);
struct inode *file_get_inode(struct file *);

/* Reading and writing. */
off_t file_read(struct file *, void *, off_t);
off_t file_read_at(struct file *, void *, off_t size, off_t start);
off_t file_write(struct file *, const void *, off_t);
off_t file_write_at(struct file *, const void *, off_t size, off_t start);

/* Preventing writes. */
void file_deny_write(struct file *);
void file_allow_write(struct file *);

/* File position. */
void file_seek(struct file *, off_t);
off_t file_tell(struct file *);
off_t file_length(struct file *);

/* extra2 */
void increase_dup_count(struct file *);
void decrease_dup_count(struct file *);
int check_dup_count(struct file *);

/* project 4 */
int is_file_dir(struct file *file);
disk_sector_t get_file_inode_num(struct file *file);
struct dir *file_to_dir(struct file *file);

#endif /* filesys/file.h */
