#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/fat.h"

/* inode를 식별하는 매직 넘버. */
#define INODE_MAGIC 0x494e4f44

/* 디스크에 기록되는 inode 구조체.
 * 크기는 정확히 DISK_SECTOR_SIZE 바이트여야 한다. */
struct inode_disk
{
	disk_sector_t start; /* 첫 데이터 섹터. */
	off_t length;		 /* 파일 크기(바이트). */
	unsigned magic;		 /* 매직 넘버. */
	bool isdir;
	// uint32_t unused[125];               /* 사용하지 않음. */
	char unused[496];
};

/* 길이가 SIZE 바이트인 inode가 차지할 섹터 수를 반환한다. */
static inline size_t
bytes_to_sectors(off_t size)
{
	return DIV_ROUND_UP(size, DISK_SECTOR_SIZE);
}

/* 메모리 상의 inode. */
/* inode는 index node의 줄임말입니다.
inode는 파일이나 디렉토리에 대한 메타데이터를 갖는 고유 식별자입니다.
*/
struct inode
{
	struct list_elem elem;	/* inode 리스트의 요소. */
	disk_sector_t sector;	/* 디스크 상 위치(섹터 번호). */
	int open_cnt;			/* 열려 있는 횟수. */
	bool removed;			/* 삭제된 경우 true. */
	int deny_write_cnt;		/* 0이면 쓰기 허용, 0보다 크면 금지. */
	struct inode_disk data; /* inode 내용. */
};

void create_root_dir_inode(void)
{
	// 1. 루트 디렉토리용 FAT 클러스터 하나 예약
	cluster_t clst = ROOT_DIR_CLUSTER;
	fat_put(clst, EOChain);

	// 2. 해당 클러스터 섹터를 0으로 초기화
	uint8_t *zero_buf = calloc(1, DISK_SECTOR_SIZE);
	if (zero_buf == NULL)
		PANIC("create_root_dir_inode: OOM during zero padding");
	disk_write(filesys_disk, cluster_to_sector(ROOT_DIR_CLUSTER), zero_buf);
	free(zero_buf);

	// 3. inode_disk 생성 및 설정
	struct inode_disk root_inode;
	ASSERT(sizeof(root_inode) == DISK_SECTOR_SIZE);
	memset(&root_inode, 0, sizeof root_inode);
	root_inode.start = ROOT_DIR_CLUSTER; // 루트 디렉토리의 데이터 시작 위치
	root_inode.length = 0;				 // 초기에는 파일 크기 0
	root_inode.magic = INODE_MAGIC;
	root_inode.isdir = true;

	// 4. 루트 inode를 디스크의 ROOT_DIR_SECTOR에 저장 (보통 sector 1)
	disk_write(filesys_disk, ROOT_DIR_SECTOR, &root_inode);
}

/* INODE의 바이트 오프셋 POS가 위치한 디스크 섹터를 반환한다.
 * POS 위치에 데이터가 없으면 -1을 반환한다. */
static disk_sector_t
byte_to_sector(const struct inode *inode, off_t pos)
{
	ASSERT(inode != NULL);

	if (pos > inode->data.length)
		return -1;

	// pos 오프셋이 위치한 섹터 서치
	off_t sectors = pos / DISK_SECTOR_SIZE;
	// 체인의 시작점 확보
	/**
	 * inode->data.start = 이 파일의 첫번째 섹터 번호
	 */
	cluster_t clst_idx = inode->data.start;
	if (clst_idx == 0 || clst_idx == EOChain)
		return -1;

	// pos가 위치한 섹터의 fat인덱스 찾기
	/* 체인을 따라가면서 저장될 위치를 찾음 */
	while (sectors > 0)
	{
		clst_idx = fat_get(clst_idx);
		if (clst_idx == EOChain)
			return -1;
		sectors--;
	}

	return cluster_to_sector(clst_idx);
}

/* 동일한 inode를 두 번 열 때 같은 `struct inode'를 반환하기 위한
 * 열린 inode 목록. */
static struct list open_inodes;

/* inode 모듈을 초기화한다. */
void inode_init(void)
{
	list_init(&open_inodes);
}

/* 길이가 LENGTH 바이트인 데이터를 갖는 inode를 초기화하여
 * 파일 시스템 디스크의 SECTOR 섹터에 기록한다.
 * 성공하면 true를, 메모리나 디스크 할당이 실패하면 false를 반환한다. */
bool inode_create(disk_sector_t sector, off_t length)
{
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	ASSERT(length >= 0);

	/* 이 어서션이 실패한다면 inode 구조체의 크기가 한 섹터와
	 * 정확히 일치하지 않는 것이므로 수정해야 한다. */
	ASSERT(sizeof *disk_inode == DISK_SECTOR_SIZE);

	disk_inode = calloc(1, sizeof *disk_inode);
	if (disk_inode != NULL)
	{
		size_t sectors = bytes_to_sectors(length);
		disk_inode->length = length;
		disk_inode->magic = INODE_MAGIC;
		if (fat_allocate(sectors, &disk_inode->start))
		{
			disk_write(filesys_disk, sector, disk_inode);
			if (sectors > 0)
			{
				static char zeros[DISK_SECTOR_SIZE];
				size_t i;

				for (i = 0; i < sectors; i++)
					disk_write(filesys_disk, disk_inode->start + i, zeros);
			}
			success = true;
		}
		free(disk_inode);
	}
	return success;
}

/* SECTOR에서 inode를 읽어 그 내용을 담은 `struct inode`를 반환한다.
 * 메모리 할당에 실패하면 NULL 포인터를 반환한다. */
struct inode *
inode_open(disk_sector_t sector)
{
	struct list_elem *e;
	struct inode *inode;

	/* 이미 열려 있는 inode인지 확인한다. */
	for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
		 e = list_next(e))
	{
		inode = list_entry(e, struct inode, elem);
		if (inode->sector == sector)
		{
			inode_reopen(inode);
			return inode;
		}
	}

	/* 메모리를 할당한다. */
	inode = malloc(sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* 초기화. */
	list_push_front(&open_inodes, &inode->elem);
	inode->sector = sector;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false;
	disk_read(filesys_disk, inode->sector, &inode->data);
	return inode;
}

/* INODE를 다시 열어 반환한다. */
struct inode *
inode_reopen(struct inode *inode)
{
	if (inode != NULL)
		inode->open_cnt++;
	return inode;
}

/* INODE의 번호(섹터 번호)를 반환한다. */
disk_sector_t
inode_get_inumber(const struct inode *inode)
{
	return inode->sector;
}

/* INODE을 닫고 디스크에 기록한다.
 * 마지막 참조라면 메모리를 해제하고,
 * 삭제 표시된 inode라면 그 블록들도 해제한다. */
void inode_close(struct inode *inode)
{
	/* NULL 포인터라면 무시한다. */
	if (inode == NULL)
		return;

	/* 마지막으로 열려 있다면 자원을 해제한다. */
	if (--inode->open_cnt == 0)
	{
		/* 리스트에서 제거하고 잠금을 해제한다. */
		list_remove(&inode->elem);

		/* 삭제된 경우 블록을 반환한다. */
		if (inode->removed)
		{
			free_map_release(inode->sector, 1);
			free_map_release(inode->data.start,
							 bytes_to_sectors(inode->data.length));
		}

		free(inode);
	}
}

/* 마지막으로 열고 있는 스레드가 닫을 때 삭제되도록 INODE에 표시한다. */
void inode_remove(struct inode *inode)
{
	ASSERT(inode != NULL);
	inode->removed = true;
}

/* OFFSET 위치부터 INODE에서 SIZE 바이트를 BUFFER로 읽어 들인다.
 * 오류가 발생하거나 파일 끝에 도달하면 SIZE보다 적게 읽을 수 있으며,
 * 실제로 읽은 바이트 수를 반환한다. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset)
{
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;

	while (size > 0)
	{
		/* 읽을 디스크 섹터와 섹터 내 시작 오프셋. */
		disk_sector_t sector_idx = byte_to_sector(inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* inode와 섹터에 남은 바이트 중 더 작은 값. */
		off_t inode_left = inode_length(inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* 이번에 실제로 복사할 바이트 수. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE)
		{
			/* 섹터 전체를 호출자의 버퍼로 직접 읽는다. */
			disk_read(filesys_disk, sector_idx, buffer + bytes_read);
		}
		else
		{
			/* 섹터를 bounce 버퍼에 읽은 뒤 일부를 호출자의 버퍼에 복사한다. */
			if (bounce == NULL)
			{
				bounce = malloc(DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read(filesys_disk, sector_idx, bounce);
			memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}

		/* 진행. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}
	free(bounce);

	return bytes_read;
}

/* OFFSET 위치부터 BUFFER의 데이터를 SIZE 바이트 만큼 INODE에 기록한다.
 * 파일 끝에 도달하거나 오류가 발생하면 SIZE보다 적게 쓸 수 있으며,
 * 실제로 기록한 바이트 수를 반환한다.
 * (보통 파일 끝에서의 쓰기는 inode를 확장해야 하지만, 아직 구현되지 않았다.) */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size,
					 off_t offset)
{
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	uint8_t *bounce = NULL;

	if (inode->deny_write_cnt)
		return 0;

	/** TODO: 여기서 파일 확장을 구현해야 함
	 * 오프셋 + 사이즈가 파일 끝보다 크다면 그만큼 섹터를 확장
	 * 클러스터 체인에도 붙여야함
	 * 확장 이후엔 while 루프에 진입해서 확장된 섹터에 write
	 */
	off_t cur_len = inode_length(inode);
	off_t zero_pad_len = offset - cur_len;
	bool first_padding = true;
	if (offset + size > inode_length(inode)) // 파일 끝보다 크게 써야 한다면
	{
		off_t last_sector_remain_size = inode_length(inode) % DISK_SECTOR_SIZE;				   // 마지막 섹터의 남은 공간
		off_t remain_length = (size + offset) - inode_length(inode) - last_sector_remain_size; // 확장해야할 크기 찾기
		while (remain_length > 0)
		{
			fat_create_chain(inode->data.start); // 클러스터 확장
			off_t add_size = remain_length < DISK_SECTOR_SIZE ? remain_length : DISK_SECTOR_SIZE;
			inode->data.length += add_size;	   // 이 파일의 길이를 확장
			remain_length -= DISK_SECTOR_SIZE; // 남은 길이 - 512
			if (zero_pad_len > 0)			   // 0으로 패딩해야 하면
			{
				uint8_t zero_pad_buf[DISK_SECTOR_SIZE] = {0};			   // 512 바이트 0 패딩 버퍼
				disk_sector_t sector_idx = byte_to_sector(inode, cur_len); // 패딩 해줘야 하는 섹터 찾기
				if (first_padding)										   // 첫번째 섹터면
				{
					bounce = malloc(DISK_SECTOR_SIZE);			 // 바운스 버퍼 만들기
					disk_read(filesys_disk, sector_idx, bounce); // 섹터의 기존 내용 복사
					memset(bounce + last_sector_remain_size, 0, DISK_SECTOR_SIZE - last_sector_remain_size);
					//  바운스 버퍼에 기존 내용 뒤에는 0으로 패딩

					disk_write(filesys_disk, sector_idx, bounce); // 패딩까지 한 후에 disk에 쓰기
					zero_pad_len -= last_sector_remain_size;	  // 0으로 패딩할 길이 감소
					cur_len += last_sector_remain_size;			  // 다음 섹터를 찾기 위해 추가
					first_padding = false;						  // 첫번째 패딩 끝남
					free(bounce);
				}
				else // 첫번째 섹터가 아니면
				{
					disk_write(filesys_disk, sector_idx, zero_pad_buf); // 디스크에 0으로 채우기
					zero_pad_len -= DISK_SECTOR_SIZE;					// 0으로 패딩할 길이 감소
					cur_len += DISK_SECTOR_SIZE;						// 다음 섹터를 찾기 위해 추가
				}
			}
		}
	}

	while (size > 0)
	{
		/* 기록할 섹터와 섹터 내 시작 오프셋. */
		disk_sector_t sector_idx = byte_to_sector(inode, offset);
		/** sector_ofs가 대체 뭔가??
		 * 디스크는 데이터를 512바이트 단위로 읽고 씀
		 * offset(파일 오프셋)은 파일 전체에서의 바이트 위치임
		 * offset이 1300일 경우, byte_to_sector로 섹터 번호를 가져오고,
		 * offset % 512로 섹터의 시작 위치를 가져와야 함 (어디서부터 쓸 건지 알기 위해)
		 */
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* inode와 섹터에 남은 바이트 중 더 작은 값. */
		off_t inode_left = inode_length(inode) - offset;					// 파일 끝까지 남은 바이트 수
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;					// 현재 섹터에 쓸 수 있는 바이트 수
		int min_left = inode_left < sector_left ? inode_left : sector_left; // 파일 끝과 섹터 끝 중 더 작은 값

		/* 이번에 실제로 이 섹터에 쓸 바이트 수. */
		int chunk_size = size < min_left ? size : min_left; // 실제로 이번에 쓸 바이트 수
		if (chunk_size <= 0)								// 이번에 쓸 바이트가 0 이하라면 탈출
			break;

		/* 섹터 전체 (512B)를 쓸 수 있는 경우라면 */
		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE)
		{
			/* 섹터 전체를 디스크에 바로 기록한다. */
			/**
			 * filesys_disk = 파일 시스템 디스크
			 * sector_idx = 기록될 섹터 번호
			 * buffer + bytes_written = 데이터를 가져올 버퍼 주소
			 */
			disk_write(filesys_disk, sector_idx, buffer + bytes_written);
		}
		else // 섹터 전체를 사용하지 못하고 일부만 쓸 수 있는 경우
		{
			/**
			 * bounce 버퍼는 기존의 섹터 데이터를 백업해두기 위한 버퍼
			 */
			if (bounce == NULL)
			{
				bounce = malloc(DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}

			/* 현재 쓰려는 범위 앞뒤에 데이터가 있는 경우
			   먼저 섹터를 읽어야 한다. 그렇지 않으면 0으로 채워진
			   섹터에서 시작한다. */
			if (sector_ofs > 0 || chunk_size < sector_left)
				/* 기존 섹터 데이터를 bounce 버퍼에 저장 */
				disk_read(filesys_disk, sector_idx, bounce);
			else
				memset(bounce, 0, DISK_SECTOR_SIZE);
			memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size); // 덮어쓰려는 부분만 바꾸기
			disk_write(filesys_disk, sector_idx, bounce);					 // 다시 전체 섹터를 디스크에 쓰기
		}

		/* 진행. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}
	free(bounce);
	inode_flush(inode);

	return bytes_written;
}

/* INODE에 대한 쓰기를 금지한다.
   각 inode 오픈마다 한 번만 호출될 수 있다. */
void inode_deny_write(struct inode *inode)
{
	inode->deny_write_cnt++;
	ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* INODE에 대한 쓰기를 다시 허용한다.
 * inode_deny_write()를 호출한 각 오픈자는 inode를 닫기 전에
 * 반드시 한 번 이 함수를 호출해야 한다. */
void inode_allow_write(struct inode *inode)
{
	ASSERT(inode->deny_write_cnt > 0);
	ASSERT(inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
}

/* INODE 데이터의 길이(바이트)를 반환한다. */
off_t inode_length(const struct inode *inode)
{
	return inode->data.length;
}

void inode_flush(struct inode *inode)
{
	disk_write(filesys_disk, inode->sector, &inode->data);
}
