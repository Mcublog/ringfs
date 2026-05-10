/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

/**
 * @defgroup ringfs_impl RingFS implementation
 * @details
 *
 * @{
 */

#include <ringfs.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>


/**
 * @defgroup sector
 * @{
 */
#define SECTOR_ERASED     (0xFFFFFFFFU) /**< Состояние по умолчанию после стирания NOR flash. */
#define SECTOR_FREE       (0xFFFFFF00U) /**< Сектор стёрт и готов к записи. */
#define SECTOR_IN_USE     (0xFFFF0000U) /**< Сектор содержит валидные данные. */
#define SECTOR_ERASING    (0xFF000000U) /**< Сектор помечен на стирание. */
#define SECTOR_FORMATTING (0x00000000U) /**< Раздел полностью форматируется. */

/** Минимальное количество секторов, необходимое для безопасной работы RingFS.
 * - 1 сектор для указателя чтения (read head)
 * - 1 сектор для операций записи (write operations)
 * - 1 сектор, который ВСЕГДА должен оставаться FREE (инвариант)
 */
#define MIN_SECTOR_COUNT 3

struct sector_header {
    uint32_t status;
    uint32_t version;
};

static int _sector_address(const struct ringfs *fs, int sector_offset)
{
    return (fs->flash->sector_offset + sector_offset) * fs->flash->sector_size;
}

static int _sector_get_status(struct ringfs *fs, int sector, uint32_t *status)
{
    return fs->flash->read(_sector_address(fs, sector) + offsetof(struct sector_header, status),
            status, sizeof(*status));
}

static int _sector_set_status(struct ringfs *fs, int sector, uint32_t status)
{
    return fs->flash->program(_sector_address(fs, sector) + offsetof(struct sector_header, status),
            &status, sizeof(status));
}

static int _sector_free(struct ringfs *fs, int sector)
{
    int sector_addr = _sector_address(fs, sector);
    _sector_set_status(fs, sector, SECTOR_ERASING);
    fs->flash->sector_erase(sector_addr);
    fs->flash->program(sector_addr + offsetof(struct sector_header, version), &fs->version, sizeof(fs->version));
    _sector_set_status(fs, sector, SECTOR_FREE);
    return 0;
}

/**
 * @}
 * @defgroup slot
 * @{
 */
#define SLOT_ERASED   (0xFFFFFFFFU) /**< Состояние по умолчанию после стирания NOR flash. */
#define SLOT_RESERVED (0xFFFFFF00U) /**< Запись начата, но ещё не подтверждена. */
#define SLOT_VALID    (0xFFFF0000U) /**< Запись подтверждена, слот содержит валидные данные. */
#define SLOT_GARBAGE  (0xFF000000U) /**< Содержимое слота отброшено и более не валидно. */

struct slot_header {
    uint32_t status;
};

static int _slot_address(const struct ringfs *fs, const struct ringfs_loc *loc)
{
    return _sector_address(fs, loc->sector) +
           sizeof(struct sector_header) +
           (sizeof(struct slot_header) + fs->object_size) * loc->slot;
}

static int _slot_get_status(const struct ringfs *fs, const struct ringfs_loc *loc, uint32_t *status)
{
    return fs->flash->read(_slot_address(fs, loc) + offsetof(struct slot_header, status),
            status, sizeof(*status));
}

static int _slot_set_status(struct ringfs *fs, struct ringfs_loc *loc, uint32_t status)
{
    return fs->flash->program(_slot_address(fs, loc) + offsetof(struct slot_header, status),
            &status, sizeof(status));
}

/**
 * @}
 * @defgroup loc
 * @{
 */

static bool _loc_equal(struct ringfs_loc *a, struct ringfs_loc *b)
{
    return (a->sector == b->sector) && (a->slot == b->slot);
}

/** Перейти в начало следующего сектора. */
static void _loc_advance_sector(struct ringfs *fs, struct ringfs_loc *loc)
{
    loc->slot = 0;
    loc->sector++;
    if (loc->sector >= fs->flash->sector_count)
        loc->sector = 0;
}

/** Перейти к следующему слоту, переходя в следующий сектор при необходимости. */
static void _loc_advance_slot(struct ringfs *fs, struct ringfs_loc *loc)
{
    loc->slot++;
    if (loc->slot >= fs->slots_per_sector)
        _loc_advance_sector(fs, loc);
}

/**
 * @}
 */

/* И вот мы начинаем. */

int ringfs_init(struct ringfs *fs, const struct ringfs_flash_partition *flash, uint32_t version, int object_size)
{
    /* Проверка минимального требуемого количества секторов. */
    if (flash->sector_count < MIN_SECTOR_COUNT) {
        printf("ringfs_init: error - partition requires at least %d sectors, got %d\n",
               MIN_SECTOR_COUNT, flash->sector_count);
        return -1;
    }

    /* Копируем аргументы в экземпляр. */
    fs->flash = flash;
    fs->version = version;
    fs->object_size = object_size;

    /* Предварительно вычисляем часто используемые значения. */
    fs->slots_per_sector = (fs->flash->sector_size - sizeof(struct sector_header)) /
                           (sizeof(struct slot_header) + fs->object_size);

    return 0;
}

int ringfs_format(struct ringfs *fs)
{
    /* Помечаем все секторы для предотвращения половинчато стёртых файловых систем. */
    for (int sector=0; sector<fs->flash->sector_count; sector++)
        _sector_set_status(fs, sector, SECTOR_FORMATTING);

    /* Стираем, обновляем версию, помечаем как свободные. */
    for (int sector=0; sector<fs->flash->sector_count; sector++)
        _sector_free(fs, sector);

    /* Начинаем читать и писать с первого сектора. */
    fs->read.sector = 0;
    fs->read.slot = 0;
    fs->write.sector = 0;
    fs->write.slot = 0;
    fs->cursor.sector = 0;
    fs->cursor.slot = 0;

    return 0;
}

int ringfs_scan(struct ringfs *fs)
{
    uint32_t previous_sector_status = SECTOR_FREE;
    /* Сектор чтения - это первый IN_USE сектор ПОСЛЕ FREE сектора
     * (или первый, если паттерна нет). */
    int read_sector = 0;
    /* Сектор записи - это последний IN_USE сектор ДО FREE сектора
     * (или последний, если паттерна нет). */
    int write_sector = fs->flash->sector_count - 1;
    /* Всегда должен быть доступен хотя бы один FREE сектор. */
    bool free_seen = false;
    /* Если нет IN_USE сектора, мы начинаем с первого. */
    bool used_seen = false;

    /* Итерируем по секторам. */
    for (int sector=0; sector<fs->flash->sector_count; sector++) {
        int addr = _sector_address(fs, sector);

        /* Читаем заголовок сектора. */
        struct sector_header header = {0};
        fs->flash->read(addr, &header, sizeof(header));

        /* Обнаруживаем частично отформатированные разделы. */
        if (header.status == SECTOR_FORMATTING) {
            printf("ringfs_scan: sector: %d addr: %d\r\n", sector, addr);
            printf("ringfs_scan: partially formatted partition\r\n");
            return -1;
        }

        /* Обнаруживаем и исправляем частично стёртые секторы. */
        if (header.status == SECTOR_ERASING || header.status == SECTOR_ERASED) {
            _sector_free(fs, sector);
            header.status = SECTOR_FREE;
        }

        /* Обнаруживаем повреждённые секторы. */
        if (header.status != SECTOR_FREE && header.status != SECTOR_IN_USE) {
            printf("ringfs_scan: corrupted sector %d\r\n", sector);
            return -1;
        }

        /* Обнаруживаем устаревшие версии. Мы не можем проверить это раньше,
         * потому что версия могла быть невалидной из-за частичного стирания. */
        if (header.version != fs->version) {
            printf("ringfs_scan: sector: %d addr: %d\r\n", sector, addr);
            printf("ringfs_scan: incompatible version 0x%08"PRIx32"\r\n", header.version);
            return -1;
        }

        /* Записываем наличие FREE сектора. */
        if (header.status == SECTOR_FREE)
            free_seen = true;

        /* Записываем наличие IN_USE сектора. */
        if (header.status == SECTOR_IN_USE)
            used_seen = true;

        /* Обновляем секторы чтения и записи согласно указанным правилам. */
        if (header.status == SECTOR_IN_USE && previous_sector_status == SECTOR_FREE)
            read_sector = sector;
        if (header.status == SECTOR_FREE && previous_sector_status == SECTOR_IN_USE)
            write_sector = sector-1;

        previous_sector_status = header.status;
    }

    /* Обнаруживаем отсутствие FREE сектора. */
    if (!free_seen) {
        printf("ringfs_scan: invariant violated: no FREE sector found\r\n");
        return -1;
    }

    /* Начинаем писать с первого сектора, если файловая система пуста. */
    if (!used_seen) {
        write_sector = 0;
    }

    /* Сканируем сектор записи и пропускаем все занятые слоты в начале. */
    fs->write.sector = write_sector;
    fs->write.slot = 0;
    while (fs->write.sector == write_sector) {
        uint32_t status;
        _slot_get_status(fs, &fs->write, &status);
        if (status == SLOT_ERASED)
            break;

        _loc_advance_slot(fs, &fs->write);
    }
    /* Если сектор был полон, мы теперь в начале FREE сектора. */

    /* Позиционируем указатель чтения в начало первого IN_USE сектора, затем пропускаем
     * GARBAGE/невалидные слоты, пока не найдём что-то ценное или не дойдём до
     * указателя записи, что означает отсутствие данных. */
    fs->read.sector = read_sector;
    fs->read.slot = 0;
    while (!_loc_equal(&fs->read, &fs->write)) {
        uint32_t status;
        _slot_get_status(fs, &fs->read, &status);
        if (status == SLOT_VALID)
            break;

        _loc_advance_slot(fs, &fs->read);
    }

    /* Перемещаем курсор чтения на позицию указателя чтения. */
    fs->cursor = fs->read;

    return 0;
}

int ringfs_capacity(const struct ringfs *fs)
{
    /* Вычисление ёмкости должно учитывать инвариант:
     * - Один сектор ВСЕГДА должен оставаться FREE
     * - Один сектор используется для операций записи
     * Поэтому максимальная полезная ёмкость = (sector_count - 2) * slots_per_sector
     */
    if (fs->flash->sector_count < MIN_SECTOR_COUNT)
        return 0;
    return fs->slots_per_sector * (fs->flash->sector_count - 2);
}

int ringfs_count_estimate(const struct ringfs *fs)
{
    int sector_diff = (fs->write.sector - fs->read.sector + fs->flash->sector_count) %
        fs->flash->sector_count;

    return sector_diff * fs->slots_per_sector + fs->write.slot - fs->read.slot;
}

int ringfs_count_exact(struct ringfs *fs)
{
    int count = 0;

    /* Используем временную переменную для итерации. */
    struct ringfs_loc loc = fs->read;
    while (!_loc_equal(&loc, &fs->write)) {
        uint32_t status;
        _slot_get_status(fs, &loc, &status);

        if (status == SLOT_VALID)
            count++;

        _loc_advance_slot(fs, &loc);
    }

    return count;
}

int ringfs_append(struct ringfs *fs, const void *object)
{
    uint32_t status;

    /*
     * При добавлении значения задействованы три сектора:
     * - сектор, в который происходит добавление: должен быть записываемым
     * - следующий сектор: должен быть свободным (инвариант)
     * - через-один сектор: указатели read и cursor перемещаются туда при необходимости
     */

    /* Убеждаемся, что следующий сектор свободен. */
    int next_sector = (fs->write.sector+1) % fs->flash->sector_count;
    _sector_get_status(fs, next_sector, &status);
    if (status != SECTOR_FREE) {
        /* Следующий сектор должен быть освобожден. Но сначала... */

        /* Перемещаем указатели read и cursor в сторону. */
        if (fs->read.sector == next_sector)
            _loc_advance_sector(fs, &fs->read);
        if (fs->cursor.sector == next_sector)
            _loc_advance_sector(fs, &fs->cursor);

        /* Освобождаем следующий сектор. */
        _sector_free(fs, next_sector);
    }

    /* Теперь мы можем убедиться, что текущий сектор записи записываемый. */
    _sector_get_status(fs, fs->write.sector, &status);
    if (status == SECTOR_FREE) {
        /* Свободный сектор. Помечаем как используемый. */
        _sector_set_status(fs, fs->write.sector, SECTOR_IN_USE);
    } else if (status != SECTOR_IN_USE) {
        printf("ringfs_append: corrupted filesystem\r\n");
        return -1;
    }

    /* Предварительное выделение слота. */
    _slot_set_status(fs, &fs->write, SLOT_RESERVED);

    /* Записываем объект. */
    fs->flash->program(_slot_address(fs, &fs->write) + sizeof(struct slot_header),
            object, fs->object_size);

    /* Фиксируем запись. */
    _slot_set_status(fs, &fs->write, SLOT_VALID);

    /* Перемещаем указатель записи. */
    _loc_advance_slot(fs, &fs->write);

    return 0;
}

int ringfs_fetch(struct ringfs *fs, void *object)
{
    /* Продвигаемся вперёд в поиске валидного слота. */
    while (!_loc_equal(&fs->cursor, &fs->write)) {
        uint32_t status;

        _slot_get_status(fs, &fs->cursor, &status);

        if (status == SLOT_VALID) {
            fs->flash->read(_slot_address(fs, &fs->cursor) + sizeof(struct slot_header),
                    object, fs->object_size);
            _loc_advance_slot(fs, &fs->cursor);
            return 0;
        }

        _loc_advance_slot(fs, &fs->cursor);
    }

    return -1;
}

int ringfs_discard(struct ringfs *fs)
{
    while (!_loc_equal(&fs->read, &fs->cursor)) {
        _slot_set_status(fs, &fs->read, SLOT_GARBAGE);
        _loc_advance_slot(fs, &fs->read);
    }

    return 0;
}

int ringfs_rewind(struct ringfs *fs)
{
    fs->cursor = fs->read;
    return 0;
}

void ringfs_dump(FILE *stream, struct ringfs *fs)
{
    const char *description;

    fprintf(stream, "RingFS read: {%d,%d} cursor: {%d,%d} write: {%d,%d}\n",
            fs->read.sector, fs->read.slot,
            fs->cursor.sector, fs->cursor.slot,
            fs->write.sector, fs->write.slot);

    for (int sector=0; sector<fs->flash->sector_count; sector++) {
        int addr = _sector_address(fs, sector);

        /* Читаем заголовок сектора. */
        struct sector_header header;
        fs->flash->read(addr, &header, sizeof(header));

        switch (header.status) {
            case SECTOR_ERASED: description = "ERASED"; break;
            case SECTOR_FREE: description = "FREE"; break;
            case SECTOR_IN_USE: description = "IN_USE"; break;
            case SECTOR_ERASING: description = "ERASING"; break;
            case SECTOR_FORMATTING: description = "FORMATTING"; break;
            default: description = "UNKNOWN"; break;
        }

        fprintf(stream, "[%04d] [v=0x%08"PRIx32"] [%-10s] ",
                sector, header.version, description);

        for (int slot=0; slot<fs->slots_per_sector; slot++) {
            struct ringfs_loc loc = { sector, slot };
            uint32_t status;
            _slot_get_status(fs, &loc, &status);

            switch (status) {
                case SLOT_ERASED: description = "E"; break;
                case SLOT_RESERVED: description = "R"; break;
                case SLOT_VALID: description = "V"; break;
                case SLOT_GARBAGE: description = "G"; break;
                default: description = "?"; break;
            }

            fprintf(stream, "%s", description);
        }

        fprintf(stream, "\n");
    }

    fflush(stream);
}

/**
 * @}
 */

/* vim: set ts=4 sw=4 et: */
