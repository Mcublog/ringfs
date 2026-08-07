/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <check.h>

#include "ringfs.h"
#include "flashsim.h"

/* Flashsim tests. */

START_TEST(test_flashsim)
{
    printf("# test_flashsim\n");

    struct flashsim *smallsim = flashsim_open("tests/test.sim", 1024, 16);
    uint8_t buf[48];
    uint8_t data[16];

    flashsim_sector_erase(smallsim, 0);
    flashsim_sector_erase(smallsim, 16);
    flashsim_sector_erase(smallsim, 32);

    memset(data, 0x5a, 16);
    flashsim_program(smallsim, 16, data, 16);

    flashsim_read(smallsim, 0, buf, 48);
    for (int i=0; i<16; i++)
        ck_assert_int_eq(buf[i], 0xff);
    for (int i=16; i<32; i++)
        ck_assert_int_eq(buf[i], 0x5a);
    for (int i=32; i<48; i++)
        ck_assert_int_eq(buf[i], 0xff);

    memset(data, 0x01, 16);
    flashsim_program(smallsim, 0, data, 16);
    memset(data, 0x10, 16);
    flashsim_program(smallsim, 32, data, 16);
    flashsim_sector_erase(smallsim, 16);

    flashsim_read(smallsim, 0, buf, 48);
    for (int i=0; i<16; i++)
        ck_assert_int_eq(buf[i], 0x01);
    for (int i=16; i<32; i++)
        ck_assert_int_eq(buf[i], 0xff);
    for (int i=32; i<48; i++)
        ck_assert_int_eq(buf[i], 0x10);

    free(smallsim);
}
END_TEST

/* Flash simulator + MTD partition fixture. */

static struct flashsim *sim;

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

/*
 * Очень маленькая файловая система: 3 слота на сектор, всего 15 слотов.
 * Преимущество — частые переносы кольца, что потенциально выявляет больше
 * ошибок.
 */
static const struct ringfs_flash_partition flash = {
    .sector_size = 32,
    .sector_offset = 4,
    .sector_count = 6,

    .sector_erase = op_sector_erase,
    .program = op_program,
    .read = op_read,
};

static void fixture_flashsim_setup(void)
{
    sim = flashsim_open("tests/ringfs.sim",
            flash.sector_size * (flash.sector_offset + flash.sector_count),
            flash.sector_size);
}

static void fixture_flashsim_teardown(void)
{
    flashsim_close(sim);
    sim = NULL;
}

/* RingFS tests. */

#define DEFAULT_VERSION 0x000000042
typedef int object_t;
#define SECTOR_HEADER_SIZE 8
#define SLOT_HEADER_SIZE 4

static void assert_loc_equiv_to_offset(const struct ringfs *fs, const struct ringfs_loc *loc, int offset)
{
    int loc_offset = loc->sector * fs->slots_per_sector + loc->slot;
    ck_assert_int_eq(offset, loc_offset);
}

static void assert_scan_integrity(const struct ringfs *fs)
{
    struct ringfs newfs;
    ringfs_init(&newfs, fs->flash, fs->version, fs->object_size);
    ck_assert(ringfs_scan(&newfs) == 0);
    ck_assert_int_eq(newfs.read.sector, fs->read.sector);
    ck_assert_int_eq(newfs.read.slot, fs->read.slot);
    ck_assert_int_eq(newfs.write.sector, fs->write.sector);
    ck_assert_int_eq(newfs.write.slot, fs->write.slot);
}

START_TEST(test_ringfs_format)
{
    printf("# test_ringfs_format\n");

    struct ringfs fs1;
    printf("## ringfs_init()\n");
    ringfs_init(&fs1, &flash, DEFAULT_VERSION, sizeof(object_t));
    printf("## ringfs_format()\n");
    ringfs_format(&fs1);
}
END_TEST

START_TEST(test_ringfs_scan)
{
    printf("# test_ringfs_scan\n");

    /* сначала отформатируем файловую систему */
    struct ringfs fs1;
    printf("## ringfs_init()\n");
    ringfs_init(&fs1, &flash, DEFAULT_VERSION, sizeof(object_t));
    printf("## ringfs_format()\n");
    ringfs_format(&fs1);

    /* теперь попробуем просканировать её */
    struct ringfs fs2;
    printf("## ringfs_init()\n");
    ringfs_init(&fs2, &flash, DEFAULT_VERSION, sizeof(object_t));
    printf("## ringfs_scan()\n");
    ck_assert(ringfs_scan(&fs2) == 0);

    /* это пустая ФС, должна начинаться так: */
    ck_assert_int_eq(fs2.slots_per_sector, (flash.sector_size-SECTOR_HEADER_SIZE)/(SLOT_HEADER_SIZE+sizeof(object_t)));
    assert_loc_equiv_to_offset(&fs2, &fs2.read, 0);
    assert_loc_equiv_to_offset(&fs2, &fs2.cursor, 0);
    assert_loc_equiv_to_offset(&fs2, &fs2.write, 0);

    /* теперь добавим несколько объектов */
    ck_assert(ringfs_append(&fs2, (int[]) { 0x11 }) == 0);
    ck_assert(ringfs_append(&fs2, (int[]) { 0x22 }) == 0);
    ck_assert(ringfs_append(&fs2, (int[]) { 0x33 }) == 0);

    /* повторное сканирование */
    printf("## ringfs_scan()\n");
    ck_assert(ringfs_scan(&fs2) == 0);

    /* убедимся, что объекты на месте */
    ck_assert(ringfs_count_exact(&fs2) == 3);

    /* scan должен завершиться ошибкой при другой версии */
    struct ringfs fs3;
    printf("## ringfs_init()\n");
    ringfs_init(&fs3, &flash, DEFAULT_VERSION+1, sizeof(object_t));
    printf("## ringfs_scan()\n");
    ck_assert(ringfs_scan(&fs3) != 0);
}
END_TEST

START_TEST(test_ringfs_append)
{
    printf("# test_ringfs_append\n");

    /* сначала отформатируем файловую систему */
    int obj;
    struct ringfs fs;
    printf("## ringfs_init()\n");
    ringfs_init(&fs, &flash, DEFAULT_VERSION, sizeof(object_t));
    printf("## ringfs_format()\n");
    ringfs_format(&fs);

    /* fetch до append не должен ничего менять */
    for (int i=0; i<3; i++) {
        printf("## ringfs_fetch()\n");
        ck_assert(ringfs_fetch(&fs, &obj) < 0);
    }
    assert_loc_equiv_to_offset(&fs, &fs.read, 0);
    assert_loc_equiv_to_offset(&fs, &fs.write, 0);
    assert_loc_equiv_to_offset(&fs, &fs.cursor, 0);
    assert_scan_integrity(&fs);

    /* теперь мы осмелели и пишем данные */
    for (int i=0; i<3; i++) {
        printf("## ringfs_append()\n");
        ringfs_append(&fs, (int[]) { 0x11*(i+1) });

        /* убедимся, что головка записи продвинулась */
        assert_loc_equiv_to_offset(&fs, &fs.write, i+1);
        assert_scan_integrity(&fs);
    }

    /* теперь извлекаем объекты. */
    for (int i=0; i<3; i++) {
        printf("## ringfs_fetch()\n");
        ck_assert(ringfs_fetch(&fs, &obj) == 0);
        ck_assert_int_eq(obj, 0x11*(i+1));

        /* убедимся, что курсор продвинулся */
        assert_loc_equiv_to_offset(&fs, &fs.cursor, i+1);
    }
    /* данных остаться не должно */
    ck_assert(ringfs_fetch(&fs, &obj) < 0);

    /* проверяем перемотку. */
    ck_assert(ringfs_rewind(&fs) == 0);
    assert_loc_equiv_to_offset(&fs, &fs.cursor, 0);

    /* попробуем прочитать объекты ещё раз. */
    for (int i=0; i<3; i++) {
        printf("## ringfs_fetch()\n");
        ck_assert(ringfs_fetch(&fs, &obj) == 0);
        ck_assert_int_eq(obj, 0x11*(i+1));
    }
}
END_TEST

START_TEST(test_ringfs_discard)
{
    printf("# test_ringfs_discard\n");

    struct ringfs fs;
    printf("## ringfs_init()\n");
    ringfs_init(&fs, &flash, DEFAULT_VERSION, sizeof(object_t));
    printf("## ringfs_format()\n");
    ringfs_format(&fs);

    /* запишем несколько записей */
    for (int i=0; i<4; i++) {
        printf("## ringfs_append()\n");
        ringfs_append(&fs, (int[]) { 0x11*(i+1) });
        assert_scan_integrity(&fs);
    }
    /* прочитаем некоторые из них */
    int obj;
    for (int i=0; i<2; i++) {
        printf("## ringfs_fetch()\n");
        ck_assert(ringfs_fetch(&fs, &obj) == 0);
        ck_assert_int_eq(obj, 0x11*(i+1));
    }
    /* сбросить прочитанное */
    ck_assert(ringfs_discard(&fs) == 0);
    assert_scan_integrity(&fs);
    /* убедимся в согласованности */
    assert_loc_equiv_to_offset(&fs, &fs.read, 2);
    assert_loc_equiv_to_offset(&fs, &fs.cursor, 2);
    assert_loc_equiv_to_offset(&fs, &fs.write, 4);

    /* прочитаем остальные записи */
    for (int i=2; i<4; i++) {
        printf("## ringfs_fetch()\n");
        ck_assert(ringfs_fetch(&fs, &obj) == 0);
        ck_assert_int_eq(obj, 0x11*(i+1));
    }
    /* сбросим их */
    ck_assert(ringfs_discard(&fs) == 0);
    assert_loc_equiv_to_offset(&fs, &fs.read, 4);
    assert_loc_equiv_to_offset(&fs, &fs.cursor, 4);
    assert_loc_equiv_to_offset(&fs, &fs.write, 4);
    assert_scan_integrity(&fs);
}
END_TEST

START_TEST(test_ringfs_capacity)
{
    printf("# test_ringfs_capacity\n");

    struct ringfs fs;
    ringfs_init(&fs, &flash, DEFAULT_VERSION, sizeof(object_t));

    int slots_per_sector = (flash.sector_size-SECTOR_HEADER_SIZE)/(SLOT_HEADER_SIZE+sizeof(object_t));
    int sectors = flash.sector_count;
    ck_assert_int_eq(ringfs_capacity(&fs), (sectors-2) * slots_per_sector);
}
END_TEST

START_TEST(test_ringfs_count)
{
    printf("# test_ringfs_count\n");

    int obj;
    struct ringfs fs;
    ringfs_init(&fs, &flash, DEFAULT_VERSION, sizeof(object_t));
    ringfs_format(&fs);
    ck_assert(ringfs_count_exact(&fs) == 0);

    printf("## write some records\n");
    for (int i=0; i<10; i++)
        ringfs_append(&fs, (int[]) { 0x11*(i+1) });
    ck_assert_int_eq(ringfs_count_exact(&fs), 10);
    ck_assert_int_eq(ringfs_count_estimate(&fs), 10);

    printf("## rescan\n");
    ck_assert(ringfs_scan(&fs) == 0);
    ck_assert_int_eq(ringfs_count_exact(&fs), 10);
    ck_assert_int_eq(ringfs_count_estimate(&fs), 10);

    printf("## append more records\n");
    for (int i=10; i<13; i++)
        ringfs_append(&fs, (int[]) { 0x11*(i+1) });
    ck_assert_int_eq(ringfs_count_exact(&fs), 13);
    ck_assert_int_eq(ringfs_count_estimate(&fs), 13);

    printf("## fetch some objects without discard\n");
    for (int i=0; i<4; i++) {
        ck_assert(ringfs_fetch(&fs, &obj) == 0);
        ck_assert_int_eq(obj, 0x11*(i+1));
    }
    ck_assert_int_eq(ringfs_count_exact(&fs), 13);
    ck_assert_int_eq(ringfs_count_estimate(&fs), 13);

    printf("## rescan\n");
    ck_assert(ringfs_scan(&fs) == 0);
    ck_assert_int_eq(ringfs_count_exact(&fs), 13);
    ck_assert_int_eq(ringfs_count_estimate(&fs), 13);

    printf("## fetch some objects with discard\n");
    for (int i=0; i<4; i++) {
        ck_assert(ringfs_fetch(&fs, &obj) == 0);
        ck_assert_int_eq(obj, 0x11*(i+1));
    }
    ck_assert_int_eq(ringfs_count_exact(&fs), 13);
    ck_assert_int_eq(ringfs_count_estimate(&fs), 13);
    ck_assert(ringfs_discard(&fs) == 0);
    ck_assert_int_eq(ringfs_count_exact(&fs), 9);
    ck_assert_int_eq(ringfs_count_estimate(&fs), 9);

    printf("## fill the segment\n");
    int count = fs.slots_per_sector - 1;
    for (int i=0; i<count; i++)
        ringfs_append(&fs, (int[]) { 0x42 });
    ck_assert_int_eq(ringfs_count_exact(&fs), 9+count);
    ck_assert_int_eq(ringfs_count_estimate(&fs), 9+count);

    printf("## extra synthetic tests for estimation\n");
    /* wrapping around */
    fs.read = (struct ringfs_loc) { fs.flash->sector_count - 1, fs.slots_per_sector - 1 };
    fs.write = (struct ringfs_loc) { 0, 0 };
    ck_assert_int_eq(ringfs_count_estimate(&fs), 1);
}
END_TEST

START_TEST(test_ringfs_overflow)
{
    printf("# test_ringfs_overflow\n");

    printf("## format\n");
    struct ringfs fs;
    ringfs_init(&fs, &flash, DEFAULT_VERSION, sizeof(object_t));
    ringfs_format(&fs);

    /* ringfs_capacity() — это объявленная безопасная ёмкость; физический
     * жёсткий предел на один сектор больше. Проверяем вытеснение на жёстком
     * пределе. */
    int capacity = ringfs_capacity(&fs) + fs.slots_per_sector;

    printf("## fill filesystem to the brim\n");
    for (int i=0; i<capacity; i++)
        ringfs_append(&fs, (int[]) { i });
    ck_assert_int_eq(ringfs_count_exact(&fs), capacity);
    assert_scan_integrity(&fs);

    /* не помешает немного нагрузить его! */
    for (int round=0; round<3; round++) {
        printf("## add one more object\n");
        ringfs_append(&fs, (int[]) { 0x42 });
        /* должен освободить целый сектор, чтобы освободить место */
        ck_assert_int_eq(ringfs_count_exact(&fs), capacity - fs.slots_per_sector + 1);
        assert_scan_integrity(&fs);

        printf("## fill back up to the sector capacity\n");
        for (int i=0; i<fs.slots_per_sector-1; i++)
            ringfs_append(&fs, (int[]) { i });

        ck_assert_int_eq(ringfs_count_exact(&fs), capacity);
        assert_scan_integrity(&fs);
    }
}
END_TEST

Suite *ringfs_suite(void)
{
    Suite *s = suite_create ("ringfs");
    TCase *tc;

    tc = tcase_create("flashsim");
    tcase_add_test(tc, test_flashsim);
    suite_add_tcase(s, tc);

    tc = tcase_create("ringfs");
    tcase_add_checked_fixture(tc, fixture_flashsim_setup, fixture_flashsim_teardown);
    tcase_add_test(tc, test_ringfs_format);
    tcase_add_test(tc, test_ringfs_scan);
    tcase_add_test(tc, test_ringfs_append);
    tcase_add_test(tc, test_ringfs_discard);
    tcase_add_test(tc, test_ringfs_capacity);
    tcase_add_test(tc, test_ringfs_count);
    tcase_add_test(tc, test_ringfs_overflow);
    suite_add_tcase(s, tc);

    return s;
}

int main()
{
    int number_failed;
    Suite *s = ringfs_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* vim: set ts=4 sw=4 et: */
