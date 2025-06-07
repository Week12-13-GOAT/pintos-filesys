#include "filesys/fat.h"
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
// Project 4
#include "filesys/fat.h"

/* DISK_SECTOR_SIZE보다 작아야 함 */
struct fat_boot {
	unsigned int magic;					//	FAT 파일 시스템인지 확인하기 위한 매직 넘버
    unsigned int sectors_per_cluster; 	//	하나의 클러스터가 차지하는 섹터 수 PintOS에서는 1로 고정 → 클러스터 == 섹터
	unsigned int total_sectors;			//	파일 시스템 전체의 섹터 수. 디스크 크기를 의미
	unsigned int fat_start;				//	FAT 테이블이 시작되는 섹터 번호
    unsigned int fat_sectors; 			//	FAT 테이블이 차지하는 섹터 수
	unsigned int root_dir_cluster;		//	루트 디렉터리가 위치한 시작 클러스터 번호
};

/* FAT 파일 시스템 정보 */
struct fat_fs {
    struct fat_boot bs;        // 부트 섹터 정보
    unsigned int *fat;         // FAT 배열(메모리에 로드된 FAT 테이블)
    unsigned int fat_length;   // FAT 엔트리 개수
    disk_sector_t data_start;  // 데이터 시작 섹터
    cluster_t last_clst;       // 마지막 클러스터 번호
    struct lock write_lock;    // FAT 수정 시 동기화용 락
};

static struct fat_fs *fat_fs;

void fat_boot_create (void);
void fat_fs_init (void);

void
fat_init (void) {
	fat_fs = calloc (1, sizeof (struct fat_fs));
	if (fat_fs == NULL)
		PANIC ("FAT init failed");

        // 디스크에서 부트 섹터를 읽어 온다
	unsigned int *bounce = malloc (DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT init failed");
	disk_read (filesys_disk, FAT_BOOT_SECTOR, bounce);
	memcpy (&fat_fs->bs, bounce, sizeof (fat_fs->bs));
	free (bounce);

        // FAT 정보 추출
	if (fat_fs->bs.magic != FAT_MAGIC)
		fat_boot_create ();
	fat_fs_init ();
}

void
fat_open (void) {
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT load failed");

        // 디스크에서 FAT을 직접 읽어 온다
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_read = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_read;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_read (filesys_disk, fat_fs->bs.fat_start + i,
			           buffer + bytes_read);
			bytes_read += DISK_SECTOR_SIZE;
		} else {
			uint8_t *bounce = malloc (DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT load failed");
			disk_read (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			memcpy (buffer + bytes_read, bounce, bytes_left);
			bytes_read += bytes_left;
			free (bounce);
		}
	}
}

void
fat_close (void) {
        // FAT 부트 섹터 기록
	uint8_t *bounce = calloc (1, DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT close failed");
	memcpy (bounce, &fat_fs->bs, sizeof (fat_fs->bs));
	disk_write (filesys_disk, FAT_BOOT_SECTOR, bounce);
	free (bounce);

        // FAT을 디스크에 직접 기록
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_wrote = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_wrote;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_write (filesys_disk, fat_fs->bs.fat_start + i,
			            buffer + bytes_wrote);
			bytes_wrote += DISK_SECTOR_SIZE;
		} else {
			bounce = calloc (1, DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT close failed");
			memcpy (bounce, buffer + bytes_wrote, bytes_left);
			disk_write (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			bytes_wrote += bytes_left;
			free (bounce);
		}
	}
}

void
fat_create (void) {
        // FAT 부트 섹터 생성
	fat_boot_create ();
	fat_fs_init ();

        // FAT 테이블 생성
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT creation failed");

        // ROOT_DIR_CLUSTER 설정
	fat_put (ROOT_DIR_CLUSTER, EOChain);

        // ROOT_DIR_CLUSTER 영역을 0으로 채움
	uint8_t *buf = calloc (1, DISK_SECTOR_SIZE);
	if (buf == NULL)
		PANIC ("FAT create failed due to OOM");
	disk_write (filesys_disk, cluster_to_sector (ROOT_DIR_CLUSTER), buf);
	free (buf);
}

void
fat_boot_create (void) {
	unsigned int fat_sectors =
	    (disk_size (filesys_disk) - 1)
	    / (DISK_SECTOR_SIZE / sizeof (cluster_t) * SECTORS_PER_CLUSTER + 1) + 1;
	fat_fs->bs = (struct fat_boot){
	    .magic = FAT_MAGIC,
	    .sectors_per_cluster = SECTORS_PER_CLUSTER,
	    .total_sectors = disk_size (filesys_disk),
	    .fat_start = 1,
	    .fat_sectors = fat_sectors,
	    .root_dir_cluster = ROOT_DIR_CLUSTER,
	};
}

void
fat_fs_init (void) {
	/* TODO: Your code goes here. */
	/*	FAT 파일 시스템은 여러 쓰레드가 동시에 접근할 수 도 있음 
	 *	→ 따라서 쓰기 작업을 보호하기 위해 락 초기화
	 */ 
	lock_init(&fat_fs->write_lock);

	/*	이건 뭐 공식임 외워. 
	 *	FAT 테이블 항목 개수 = FAT 테이블 총 섹터 수 × 섹터 크기 ÷ 클러스터 항목 크기
	 */
	fat_fs->fat_length = fat_fs->bs.fat_sectors * DISK_SECTOR_SIZE / sizeof(cluster_t);

	// 파일 시작 위치는 이전 시작위치 + 이전 섹터를 더하면 됩니다. 
	fat_fs->data_start = fat_fs->bs.fat_start + fat_fs->bs.fat_sectors;

	/*	클러스터 번호 0번과 1번은 예약되어 있으므로
	 *	→ 새로운 클러스터 할당 시 2번부터 시작!
	 */
	fat_fs->last_clst =2;
}

/*----------------------------------------------------------------------------*/
/* FAT 처리                                                                    */
/*----------------------------------------------------------------------------*/

/* 클러스터 체인에 새 클러스터를 추가한다.
 * CLST가 0이면 새로운 체인을 시작한다.
 * 새 클러스터 할당에 실패하면 0을 반환한다. */
cluster_t
fat_create_chain (cluster_t clst) {
	/* TODO: Your code goes here. */
	for (int i = 2; i < fat_fs->fat_length; i++){
		if (fat_fs->fat[i] == 0) {
			if (clst == 0) {
				fat_fs->fat[i] = EOChain;
				return i;
			}
			else {
				if (clst >= fat_fs->fat_length) {
					return 0;
				}
				cluster_t curr = clst;
				while (fat_fs->fat[curr] != EOChain) {
					curr = fat_fs->fat[curr];
					if (curr >= fat_fs->fat_length) {
						return 0;
					}
				}
				fat_fs->fat[curr] = i;
				fat_fs->fat[i] = EOChain;
				return i;
			}
		}
	}

	return 0;
}

/* CLST부터 시작하는 클러스터 체인을 제거한다.
 * PCLST가 0이면 CLST가 체인의 시작이라고 가정한다. */
void
fat_remove_chain (cluster_t clst, cluster_t pclst) {
	/* TODO: Your code goes here. */
	ASSERT(clst >= 2 && clst < fat_fs->fat_length);

	if (pclst != 0) {
		ASSERT(pclst >= 2 && pclst < fat_fs->fat_length);
		fat_put(pclst, EOChain);
	}

	cluster_t curr = clst;
	while (curr != EOChain) {
		cluster_t next = fat_get(curr);
		fat_put(curr, 0);
		curr = next;
	}
}

/* FAT 테이블의 값을 갱신한다. */
void
fat_put (cluster_t clst, cluster_t val) {
	/* TODO: Your code goes here. */
	if (clst < 2 || clst >= fat_fs->fat_length) {
		PANIC("fat put : Invalid cluster number\n");
	}
	fat_fs->fat[clst] = val;
}

/* FAT 테이블에서 값을 가져온다. */
cluster_t
fat_get (cluster_t clst) {
	/* TODO: Your code goes here. */
	if (clst < 2 || clst >= fat_fs->fat_length) {
		PANIC("fat get : Invalid cluster number\n");
	}
	return fat_fs->fat[clst];
}

/* 클러스터 번호를 섹터 번호로 변환한다. */
disk_sector_t
cluster_to_sector (cluster_t clst) {
	/* TODO: Your code goes here. */
	return fat_fs->data_start + (clst - 2);
}
