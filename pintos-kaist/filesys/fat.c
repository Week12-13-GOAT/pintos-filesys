#include "filesys/fat.h"
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <stdio.h>
#include <string.h>

/* DISK_SECTOR_SIZE보다 작아야 함 */
struct fat_boot
{
	unsigned int magic;				  /* FAT 파일 시스템임을 나타내는 숫자 */
	unsigned int sectors_per_cluster; /* 항상 1로 고정 */
	unsigned int total_sectors;		  /*디스크 전체 섹터 수*/
	unsigned int fat_start;			  /*FAT 영역의 시작 섹터 번호*/
	unsigned int fat_sectors;		  /* FAT 크기(섹터 단위) */
	unsigned int root_dir_cluster;	  /*루트 디렉토리의 시작 클러스터 번호*/
};

/* FAT 파일 시스템 정보 */
struct fat_fs
{
	struct fat_boot bs;		  /* FAT 부트 섹터: FAT 파일 시스템의 메타데이터 (섹터 수, 위치 등) */
	unsigned int *fat;		  /* FAT 테이블을 메모리에 로드한 포인터 (cluster 연결 정보를 담음) */
	unsigned int fat_length;  /* FAT 테이블의 섹터 수 (== bs.fat_sectors) */
	disk_sector_t data_start; /* 실제 파일/디렉터리 데이터가 저장되는 영역의 시작 섹터 번호 */
	cluster_t last_clst;	  /* FAT에서 최근에 할당된 마지막 클러스터 번호 (새 클러스터 할당 시 사용) */
	struct lock write_lock;	  /* FAT 갱신 시 동기화를 위한 락 (write 동시성 제어용) */
};

static struct fat_fs *fat_fs;

void fat_boot_create(void);
void fat_fs_init(void);

void fat_init(void)
{
	fat_fs = calloc(1, sizeof(struct fat_fs));
	if (fat_fs == NULL)
		PANIC("FAT init failed");

	// 디스크에서 부트 섹터를 읽어 온다
	unsigned int *bounce = malloc(DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC("FAT init failed");
	disk_read(filesys_disk, FAT_BOOT_SECTOR, bounce);
	memcpy(&fat_fs->bs, bounce, sizeof(fat_fs->bs));
	free(bounce);

	// FAT 정보 추출
	if (fat_fs->bs.magic != FAT_MAGIC)
		fat_boot_create();
	fat_fs_init();
}

void fat_open(void)
{
	fat_fs->fat = calloc(fat_fs->fat_length, sizeof(cluster_t));
	if (fat_fs->fat == NULL)
		PANIC("FAT load failed");

	// 디스크에서 FAT을 직접 읽어 온다
	uint8_t *buffer = (uint8_t *)fat_fs->fat;
	off_t bytes_read = 0;
	off_t bytes_left = sizeof(fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof(cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++)
	{
		bytes_left = fat_size_in_bytes - bytes_read;
		if (bytes_left >= DISK_SECTOR_SIZE)
		{
			disk_read(filesys_disk, fat_fs->bs.fat_start + i,
					  buffer + bytes_read);
			bytes_read += DISK_SECTOR_SIZE;
		}
		else
		{
			uint8_t *bounce = malloc(DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC("FAT load failed");
			disk_read(filesys_disk, fat_fs->bs.fat_start + i, bounce);
			memcpy(buffer + bytes_read, bounce, bytes_left);
			bytes_read += bytes_left;
			free(bounce);
		}
	}
}

void fat_close(void)
{
	// FAT 부트 섹터 기록
	uint8_t *bounce = calloc(1, DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC("FAT close failed");
	memcpy(bounce, &fat_fs->bs, sizeof(fat_fs->bs));
	disk_write(filesys_disk, FAT_BOOT_SECTOR, bounce);
	free(bounce);

	// FAT을 디스크에 직접 기록
	uint8_t *buffer = (uint8_t *)fat_fs->fat;
	off_t bytes_wrote = 0;
	off_t bytes_left = sizeof(fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof(cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++)
	{
		bytes_left = fat_size_in_bytes - bytes_wrote;
		if (bytes_left >= DISK_SECTOR_SIZE)
		{
			disk_write(filesys_disk, fat_fs->bs.fat_start + i,
					   buffer + bytes_wrote);
			bytes_wrote += DISK_SECTOR_SIZE;
		}
		else
		{
			bounce = calloc(1, DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC("FAT close failed");
			memcpy(bounce, buffer + bytes_wrote, bytes_left);
			disk_write(filesys_disk, fat_fs->bs.fat_start + i, bounce);
			bytes_wrote += bytes_left;
			free(bounce);
		}
	}
}

void fat_create(void)
{
	// FAT 부트 섹터 생성
	fat_boot_create();
	fat_fs_init();

	// FAT 테이블 생성
	fat_fs->fat = calloc(fat_fs->fat_length, sizeof(cluster_t));
	if (fat_fs->fat == NULL)
		PANIC("FAT creation failed");

	// ROOT_DIR_CLUSTER 설정
	fat_put(ROOT_DIR_CLUSTER, EOChain);

	// ROOT_DIR_CLUSTER 영역을 0으로 채움
	uint8_t *buf = calloc(1, DISK_SECTOR_SIZE);
	if (buf == NULL)
		PANIC("FAT create failed due to OOM");
	disk_write(filesys_disk, cluster_to_sector(ROOT_DIR_CLUSTER), buf);
	free(buf);
}

void fat_boot_create(void)
{
	unsigned int fat_sectors =
		(disk_size(filesys_disk) - 1) / (DISK_SECTOR_SIZE / sizeof(cluster_t) * SECTORS_PER_CLUSTER + 1) + 1;
	fat_fs->bs = (struct fat_boot){
		.magic = FAT_MAGIC,
		.sectors_per_cluster = SECTORS_PER_CLUSTER,
		.total_sectors = disk_size(filesys_disk),
		.fat_start = 1,
		.fat_sectors = fat_sectors,
		.root_dir_cluster = ROOT_DIR_CLUSTER,
	};
}

void fat_fs_init(void)
{
	/* TODO: Your code goes here. */
	fat_fs->fat_length = fat_fs->bs.fat_sectors;
	fat_fs->data_start = fat_fs->bs.fat_start + fat_fs->bs.fat_sectors;
	lock_init(&fat_fs->write_lock);
}

/*----------------------------------------------------------------------------*/
/* FAT 처리                                                                    */
/*----------------------------------------------------------------------------*/

/* 클러스터 체인에 새 클러스터를 추가한다.
 * CLST가 0이면 새로운 체인을 시작한다.
 * 새 클러스터 할당에 실패하면 0을 반환한다. */
cluster_t
fat_create_chain(cluster_t clst)
{
	/* TODO: Your code goes here. */
	ASSERT(clst < fat_fs->fat_length);

	unsigned int *fat = fat_fs->fat;

	// 빈 클러스터 탐색
	cluster_t new_clst = find_free_cluster();
	if (new_clst == 0)
		return 0;

	fat_put(new_clst, EOChain);

	// 탐색 성공한 클러스터 번호를 저장(이후 탐색은 last_clst부터(=next_fit))
	fat_fs->last_clst = new_clst;
	if (clst == 0)
		return new_clst;

	// 만약에라도 무한루프가 생기면 이쪽 확인할 것
	// 이 코드가 문제가 아니라 get으로 나온게 EOChain인게 문제
	while (fat_get(clst) != EOChain)
	{
		clst = fat_get(clst);
	}

	fat_put(clst, new_clst);
	return new_clst;
}

/* CLST부터 시작하는 클러스터 체인을 제거한다.
 * PCLST가 0이면 CLST가 체인의 시작이라고 가정한다. */
void fat_remove_chain(cluster_t clst, cluster_t pclst)
{
	ASSERT(clst < fat_fs->fat_length);

	if (pclst != 0)
		fat_put(pclst, EOChain);

	while (clst != EOChain)
	{
		cluster_t next = fat_get(clst);
		fat_put(clst, 0);

		if (fat_fs->last_clst == clst)
			fat_fs->last_clst = 2;

		clst = next;
	}

	return;
}

/* FAT 테이블의 값을 갱신한다. */
void fat_put(cluster_t clst, cluster_t val)
{
	fat_fs->fat[clst] = val;
	return;
}

/* FAT 테이블에서 클러스터 cls가 가리키고 있는 다음 클러스터 번호 반환. */
cluster_t
fat_get(cluster_t clst)
{
	return fat_fs->fat[clst];
}

/* 클러스터 번호를 섹터 번호로 변환한다. */
disk_sector_t
cluster_to_sector(cluster_t clst)
{
	return fat_fs->data_start + (clst - 2);
}

// 빈 클러스터 탐색
cluster_t find_free_cluster(void)
{
	cluster_t find_end = fat_fs->last_clst;
	// last_clst는 할당되어 있으므로, last_clst + 1 위치부터 탐색 시작
	cluster_t finding_clst = find_end + 1;

	// 최대지점(fat_length) 도달 시 처음(2)로 돌아가 last_clst 까지 탐색
	while (finding_clst != find_end)
	{
		if (fat_get(finding_clst) == 0)
			return finding_clst;

		finding_clst++;

		if (finding_clst == fat_fs->fat_length)
			finding_clst = 2;
	}

	// 없을 시 0 반환
	return 0;
}