/*
 * Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "plat_vfio.h"
#include "plat_dmabuf.h"
#include "plat_smmu.h"

#include <linux/hyp_udmabuf.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>


#define HYP_UDMABUF_DEVICE "/dev/hyp_udmabuf"
#define CONFIG_FILE_PATH "hyp_udmabuf_test.config"
#define MAX_SUPPORTED_TIME_OUT 600
#define MAX_SUPPORTED_THREADS_NUM 64
#define MAX_SUPPORTED_SMMU_MAP_COUNT 64
#define MAX_SUPPORTED_CPU_MAP_COUNT 64
#define MAX_DEVICE_NAME_LEN 32


static int timeout = -1;
static int num_threads = 1;
static int smmu_map_count = 10;
static int cpu_map_count = 10;
static int fd = -1;

static int page_sgl_nents = 3;
static struct plat_vfio pvfio;
static struct guest_mem_region *page_sgl_table = NULL;


static int open_device(void)
{
	int fd = -1;

	fd = open(HYP_UDMABUF_DEVICE, O_RDWR);
	if (fd < 0) {
		printf("open device failed\n");
		return -1;
	}

	return fd;
}

static void close_device(int fd)
{
	if (fd >= 0)
		close(fd);
}

static int hyp_udmabuf_test_set_default_config()
{
	page_sgl_table = malloc(sizeof(struct guest_mem_region) * page_sgl_nents);
	if (!page_sgl_table) {
		printf("malloc page_sgl_table failed\n");
		return -ENOMEM;
	}

	page_sgl_table[0].addr  = 0xf3640000;
	page_sgl_table[0].len = 0x10000;
	page_sgl_table[1].addr  = 0x1acdf1000;
	page_sgl_table[1].len = 0x1000;
	page_sgl_table[2].addr  = 0x20d98c000;
	page_sgl_table[2].len = 0x1000;

	return 0;
}

static int hyp_udmabuf_test_config_init()
{
	static const char *config_file = CONFIG_FILE_PATH;
	struct stat st;
	int nErr = 0;
	size_t len = 0;
	FILE *fp = NULL;
	char *saveptr = NULL, *buf = NULL;
	char *token, *line;
	char delim[] = " =,";
	int dmabuf_fd, ret;

	nErr = stat(config_file, &st);
	if (nErr)
		return hyp_udmabuf_test_set_default_config();

	len = st.st_size + 1;
	fp = fopen(config_file, "r");
	if (!fp)
		goto bail;

	buf = calloc(1, len);
	if (!buf)
		goto bail;

	line = fgets(buf, len, fp);
	if (!line)
		goto bail;

	token = strtok_r(line, delim, &saveptr);
	if (token == NULL || strcmp(token, "page_sgl_nents") != 0) {
		printf("Invalid format for num\n");
		goto bail;
	}
	token = strtok_r(NULL, delim, &saveptr);
	if (token == NULL) {
		printf("Invalid format for num value\n");
		goto bail;
	}
	page_sgl_nents = atoi(token);
	printf("page_sgl_nents is %d\n", page_sgl_nents);

	page_sgl_table = malloc(sizeof(struct guest_mem_region) * page_sgl_nents);
	if (!page_sgl_table) {
		printf("malloc page_sgl_table failed\n");
		goto bail;
	}

	for (int i = 0; i < page_sgl_nents; i++) {
		if (fgets(line, len, fp) == NULL) {
			printf("Failed to read config line\n");
			goto bail;
		}
		token = strtok_r(line, delim, &saveptr);

		while (token != NULL) {
			if (strcmp(token, "pa_start") == 0) {
				token = strtok_r(NULL, delim, &saveptr);
				if (token != NULL) {
					page_sgl_table[i].addr  = strtoull(token, NULL, 16);
				}
			} else if (strcmp(token, "len") == 0) {
				token = strtok_r(NULL, delim, &saveptr);
				if (token != NULL) {
					page_sgl_table[i].len = strtoull(token, NULL, 16);
				}
			}
			token = strtok_r(NULL, delim, &saveptr);
		}
		printf("page_sgl_table%d.addr = 0x%llx, page_sgl_table%d.len = 0x%llx\n", i, page_sgl_table[i].addr, i, page_sgl_table[i].len);
	}

	free(buf);
	fclose(fp);
	return 0;
bail:
	if (buf) {
		free(buf);
		buf = NULL;
	}
	if (fp)
		fclose(fp);
	if (page_sgl_table)
		free(page_sgl_table);
	return -1;
}


static int create_dmabuf_fd(int device, int *fd, uint64_t *size)
{
	int dmabuf_fd, ret;
	struct hyp_udmabuf_create *create;

	create = malloc(sizeof(struct hyp_udmabuf_create) + sizeof(struct guest_mem_region) * page_sgl_nents);
	if (!create) {
		printf("malloc create failed\n");
		return -ENOMEM;
	}

	memcpy(create->list, page_sgl_table, sizeof(struct guest_mem_region) * page_sgl_nents);
	create->flags = O_RDWR;
	create->count = page_sgl_nents;
	create->fd = -1;
	create->size = 0;

	ret = ioctl(device, HYP_UDMABUF_CREATE, create);
	if (ret) {
		printf("create list failed\n");
		free(create);
		return ret;
	}
	printf("Create new dmabuf %d with size 0x%llx\n",
			create->fd, create->size);
	*fd = create->fd;
	*size = create->size;

	free(create);
	return 0;
}

static int get_vfio_fd(const char *device_name, struct plat_vfio *vfio, int iommu_grp)
{
	int ret = 0;

	/* "17c23000.vfio_timer" is from smmulib's unit_tests */
	ret = plat_vfio_device_init(device_name, vfio);
	if (ret < 0) {
		printf("\n Failed to initalize the vfio device\n");
		return -ENODEV;
	}
	printf("plat vfio success:%d\n", ret);
	return ret;
}

void smmu_map_test(struct plat_smmu *smmu_ctx, int thread_id)
{
	int count = smmu_map_count;
	do {
		vfio_dma_buf_map(&pvfio, smmu_ctx);
		printf("Thread ID: %d, %d.DMA Address 0x%lx \n", thread_id, smmu_ctx->dma_direction_usr + 1, smmu_ctx->dma_addr);
		vfio_dma_buf_unmap(&pvfio, smmu_ctx);
		count--;
	} while (count > 0);

	return;
}

void cpu_map_test(struct plat_smmu *smmu_ctx, int thread_id, uint64_t dmabuf_size)
{
	void *va;
	int buf[10];
	int count = cpu_map_count;
	do {
		va = mmap(0, dmabuf_size, PROT_READ, MAP_SHARED,
				smmu_ctx->dmabuf_fd, 0);
		if (va == MAP_FAILED) {
			printf("Thread ID: %d, Mmap failed\n", thread_id);
			break;
		}
		printf("Thread ID: %d, VA Address %p\n", thread_id, va);
		memcpy(buf, va, sizeof(buf));
		munmap(va, dmabuf_size);
		count--;
	} while (count > 0);

	return;
}

void *thread_muti_threads_case(void *arg)
{
	int ret = 0;
	uint64_t dmabuf_size;
	struct plat_smmu smmu_ctx;
	int thread_id = *(int *)arg;

	/* call hyp_dmabuf to get FD */
	ret = create_dmabuf_fd(fd, &smmu_ctx.dmabuf_fd,
			&dmabuf_size);
	if (ret) {
		printf("Thread ID: %d, create dmabuf fd failed\n", thread_id);
		return (void *)(intptr_t)ret;
	}

	smmu_ctx.dma_direction_usr = 0;
	printf("Thread ID: %d, Custom IOVA start\n", thread_id);
	smmu_map_test(&smmu_ctx, thread_id);
	printf("Thread ID: %d, Custom IOVA end\n", thread_id);

	smmu_ctx.dma_direction_usr = 1;
	printf("Thread ID: %d, Custom IOVA range-2 start\n", thread_id);
	smmu_map_test(&smmu_ctx, thread_id);
	printf("Thread ID: %d, Custom IOVA range-2 end\n", thread_id);

	printf("Thread ID: %d, Custom VA start\n", thread_id);
	cpu_map_test(&smmu_ctx, thread_id, dmabuf_size);
	printf("Thread ID: %d, Custom VA end\n", thread_id);

	close(smmu_ctx.dmabuf_fd);
	return (void *)(intptr_t)ret;
}

static int T32_test_case(void)
{
	struct plat_smmu smmu_ctx;
	uint64_t dmabuf_size;
	int ret;

	if (timeout <= 0)
		return 0;

	/* Allocacate dmabuf and get FD */
	ret = create_dmabuf_fd(fd, &smmu_ctx.dmabuf_fd,
			&dmabuf_size);
	if (ret)
		return ret;

	smmu_ctx.dma_direction_usr = 0;
	vfio_dma_buf_map(&pvfio, &smmu_ctx);
	printf("BREAK in debugger now to see if CB is populated with the appropriate PGT\n");
	sleep(timeout);
	vfio_dma_buf_unmap(&pvfio, &smmu_ctx);

	return 0;
}

void print_usage(void) {
	printf("usage:\n\n"
			"  Recommended:\n"
			"     ./hyp_udmabuf_test -D 17c23000.vfio_timer -t 10 -n 10 -s 20 -c 20\n\n"
			"  Options:\n\n"
			"  -D: device to use (default 17c23000.vfio_timer)\n\n"
			"  -t: Time (s) break in debugger now to see if CB is populated with the appropriate PGT.\n\n"
			"  -n: Number of threads to create.\n\n"
			"  -s: Test times for smmu map.\n\n"
			"  -c: Test times for cpu map.\n\n"
			"  Example:\n"
			"  ./hyp_udmabuf_test -t 10\n\n"
			"  ./hyp_udmabuf_test -D 17c23000.vfio_timer -t 10 -n 10 -s 20 -c 20\n\n"
			);
}

int main(int argc, char *argv[])
{
	int opt;
	int nErr = 0;
	pthread_t *threads = NULL;
	int *thread_ids = NULL;
	void *thread_ret;
	char device_name[MAX_DEVICE_NAME_LEN] = {0};
	/* set defualt device name to 17c23000.vfio_timer */
	snprintf(device_name, MAX_DEVICE_NAME_LEN, "17c23000.vfio_timer");

	if(argc == 1) {
		printf("Hyp-udmabuf test start with default parameters\n");
	} else {
		while ((opt = getopt(argc, argv, "D:t:n:s:c:")) != -1) {
			switch(opt) {
				case 'D':
					snprintf(device_name, MAX_DEVICE_NAME_LEN, "%s", optarg);
					break;
				case 't':
					timeout = atoi(optarg);
					if(timeout <= 0 || timeout > MAX_SUPPORTED_TIME_OUT) {
						printf("Error: Invalid timeout %d, should be between 1 and 60\n\n", timeout);
						goto bail;
					}
					break;
				case 'n':
				    num_threads = atoi(optarg);
					if(num_threads <= 0 || num_threads > MAX_SUPPORTED_THREADS_NUM) {
						printf("Error: Invalid num_threads %d, should be between 1 and 100\n\n", num_threads);
						goto bail;
					}
					break;
				case 's':
					smmu_map_count = atoi(optarg);
					if(smmu_map_count <= 0 || smmu_map_count > MAX_SUPPORTED_SMMU_MAP_COUNT) {
						printf("Error: Invalid smmu_map_count %d, should be between 1 and 100\n\n", smmu_map_count);
						goto bail;
					}
					break;
				case 'c':
					cpu_map_count = atoi(optarg);
					if(cpu_map_count <= 0 || cpu_map_count > MAX_SUPPORTED_CPU_MAP_COUNT) {
						printf("Error: Invalid cpu_map_count %d, should be between 1 and 100\n\n", cpu_map_count);
						goto bail;
					}
					break;
				case ':':
					printf("Missing argument for option '%c'\n", optopt);
					break;
				case '?':
					print_usage();
					goto bail;
				default:
					print_usage();
					goto bail;
			}
		}
		printf("Hyp-udmabuf test start with device_name=%s, timeout=%d, num_threads=%d, smmu_map_count=%d, cpu_map_count=%d\n",
				device_name, timeout, num_threads, smmu_map_count, cpu_map_count);
	}

	threads = malloc(num_threads * sizeof(pthread_t));
	if (!threads) {
		printf("malloc threads failed\n");
		nErr = -ENOMEM;
		goto bail;
	}

	thread_ids = malloc(num_threads * sizeof(int));
	if (!thread_ids) {
		printf("malloc thread_ids failed\n");
		nErr = -ENOMEM;
		goto bail;
	}

	fd = open_device();
	if (fd < 0)	{
		nErr = -ENODEV;
		goto bail;
	}

	/* Get plat_vfio struct for device for known IOMMU group ID */
	nErr = get_vfio_fd(device_name, &pvfio, 0);
	if (nErr < 0) {
		goto bail;
	}

	nErr = hyp_udmabuf_test_config_init();
	if (nErr < 0) {
		goto bail;
	}

	nErr = T32_test_case();
	if (nErr != 0) {
		goto bail;
	}

	for (int i = 0; i < num_threads; i++) {
		thread_ids[i] = i + 1;
		printf("pthread_create, %d\n", i + 1);
		if (pthread_create(&threads[i], NULL, thread_muti_threads_case, &thread_ids[i]) != 0) {
			printf("Failed to create thread\n");
			nErr = -1;
			goto bail;
		}
	}

	for (int i = 0; i < num_threads; i++) {
		if (pthread_join(threads[i], &thread_ret) != 0) {
			printf("Thread id: %d, failed to join thread, %d\n", thread_ids[i], (int)(intptr_t)thread_ret);
			nErr = (int)(intptr_t)thread_ret;
			goto bail;
		}
	}

	free(threads);
	free(thread_ids);
	free(page_sgl_table);
	plat_vfio_device_deinit(&pvfio);
	close_device(fd);
	printf("Device Deinit......      Done\n");
	return 0;

bail:
	if (threads) {
		free(threads);
	}
	if (thread_ids) {
		free(thread_ids);
	}
	if (page_sgl_table) {
		free(page_sgl_table);
	}
	plat_vfio_device_deinit(&pvfio);
	close_device(fd);
	return nErr;
}
