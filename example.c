/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include <stdio.h>
#include <assert.h>

#include "ringfs.h"
#include "flashsim.h"


/* Реализация и операции с flash. */

#define FLASH_SECTOR_SIZE       1024
#define FLASH_TOTAL_SECTORS     16
#define FLASH_PARTITION_OFFSET  8
#define FLASH_PARTITION_SIZE    4

static struct flashsim *sim;

static void init_flash_driver(void)
{
    sim = flashsim_open("example.sim",
            FLASH_TOTAL_SECTORS*FLASH_SECTOR_SIZE,
            FLASH_SECTOR_SIZE);
}

static int op_sector_erase(uint32_t address)
{
    flashsim_sector_erase(sim, address);
    return 0;
}

static ssize_t op_program(uint32_t address, const void *data, size_t size)
{
    flashsim_program(sim, address, data, size);
    return size;
}

static ssize_t op_read(uint32_t address, void *data, size_t size)
{
    flashsim_read(sim, address, data, size);
    return size;
}

static struct ringfs_flash_partition flash = {
    .sector_size = FLASH_SECTOR_SIZE,
    .sector_offset = FLASH_PARTITION_OFFSET,
    .sector_count = FLASH_PARTITION_SIZE,

    .sector_erase = op_sector_erase,
    .program = op_program,
    .read = op_read,
};


/* Формат записи данных. */
struct log_entry {
    int level;
    char message[16];
};
#define LOG_ENTRY_VERSION 1


/* Поехали! */
int main()
{
    /* Инициализируйте драйвер flash до использования файловой системы. */
    init_flash_driver();

    /* Всегда вызывайте ringfs_init первым. */
    struct ringfs fs;
    ringfs_init(&fs, &flash, LOG_ENTRY_VERSION, sizeof(struct log_entry));

    /* Просканируйте и/или отформатируйте до любых операций с данными. */
    printf("# scanning for filesystem...\n");
    if (ringfs_scan(&fs) == 0) {
        printf("# found existing filesystem, usage: %d/%d\n",
                ringfs_count_estimate(&fs),
                ringfs_capacity(&fs));
    } else {
        printf("# no valid filesystem found, formatting.\n");
        ringfs_format(&fs);
    }

    /* Добавление данных через ringfs_append. Самые старые данные удаляются
     * по мере необходимости. */
    printf("# inserting some objects\n");
    ringfs_append(&fs, &(struct log_entry) { 1, "foo" });
    ringfs_append(&fs, &(struct log_entry) { 2, "bar" });
    ringfs_append(&fs, &(struct log_entry) { 3, "baz" });
    ringfs_append(&fs, &(struct log_entry) { 4, "xyzzy" });
    ringfs_append(&fs, &(struct log_entry) { 5, "test" });
    ringfs_append(&fs, &(struct log_entry) { 6, "hello" });

    /* Объекты извлекаются через ringfs_fetch. Физически они не удаляются, пока
     * не будет вызван ringfs_discard. Это полезно, например, при передаче
     * объектов по сети: они не удаляются из кольцевого буфера, пока не получено
     * подтверждение (ACK). */
    printf("# reading 2 objects\n");
    for (int i=0; i<2; i++) {
        struct log_entry entry;
        assert(ringfs_fetch(&fs, &entry) == 0);
        printf("## level=%d message=%.16s\n", entry.level, entry.message);
    }
    printf("# discarding read objects\n");
    ringfs_discard(&fs);

    /* Если вы решили, что пока не можете удалить объекты, просто вызовите
     * ringfs_rewind() — они снова станут доступны для последующих чтений. */
    printf("# reading 2 objects\n");
    for (int i=0; i<2; i++) {
        struct log_entry entry;
        assert(ringfs_fetch(&fs, &entry) == 0);
        printf("## level=%d message=%.16s\n", entry.level, entry.message);
    }
    printf("# rewinding read head back\n");
    ringfs_rewind(&fs);

    /* ...и вот они снова. */
    printf("# reading 2 objects\n");
    for (int i=0; i<2; i++) {
        struct log_entry entry;
        assert(ringfs_fetch(&fs, &entry) == 0);
        printf("## level=%d message=%.16s\n", entry.level, entry.message);
    }

    return 0;
}

/* vim: set ts=4 sw=4 et: */
