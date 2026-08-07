/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

/**
 * @defgroup ringfs_impl Реализация RingFS
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
#define SECTOR_FREE       (0xFFFFFF00U) /**< Сектор затёрт и помечен свободным. */
#define SECTOR_IN_USE     (0xFFFF0000U) /**< Сектор содержит актуальные данные. */
#define SECTOR_ERASING    (0xFF000000U) /**< Сектор помечен к стиранию. */
#define SECTOR_FORMATTING (0x00000000U) /**< Форматируется весь раздел. */

/** Минимальное число секторов, необходимое для безопасной работы RingFS.
 * - 1 сектор для головки чтения
 * - 1 сектор для операций записи
 * - 1 сектор, который всегда должен оставаться свободным (FREE, инвариант)
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
#define SLOT_RESERVED (0xFFFFFF00U) /**< Запись начата, но ещё не завершена. */
#define SLOT_VALID    (0xFFFF0000U) /**< Запись завершена, слот содержит актуальные данные. */
#define SLOT_GARBAGE  (0xFF000000U) /**< Содержимое слота отброшено и более не актуально. */

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

/** Перемещает позицию к началу следующего сектора. */
static void _loc_advance_sector(struct ringfs *fs, struct ringfs_loc *loc)
{
    loc->slot = 0;
    loc->sector++;
    if (loc->sector >= fs->flash->sector_count)
        loc->sector = 0;
}

/** Перемещает позицию к следующему слоту, при необходимости переходя к следующему сектору. */
static void _loc_advance_slot(struct ringfs *fs, struct ringfs_loc *loc)
{
    loc->slot++;
    if (loc->slot >= fs->slots_per_sector)
        _loc_advance_sector(fs, loc);
}

/**
 * @}
 */

/* Поехали. */

int ringfs_init(struct ringfs *fs, const struct ringfs_flash_partition *flash, uint32_t version, int object_size)
{
    /* Проверка минимального количества секторов. */
    if (flash->sector_count < MIN_SECTOR_COUNT) {
        printf("ringfs_init: error - partition requires at least %d sectors, got %d\n",
               MIN_SECTOR_COUNT, flash->sector_count);
        return -1;
    }

    /* Копирование аргументов в экземпляр. */
    fs->flash = flash;
    fs->version = version;
    fs->object_size = object_size;

    /* Проверка размера объекта. */
    if (object_size <= 0) {
        printf("ringfs_init: error - object size must be positive, got %d\n",
               object_size);
        return -1;
    }

    /* Предвычисление часто используемых значений. */
    fs->slots_per_sector = (fs->flash->sector_size - sizeof(struct sector_header)) /
                           (sizeof(struct slot_header) + fs->object_size);

    /* Объект должен помещаться в сектор вместе с заголовками. */
    if (fs->slots_per_sector < 1) {
        printf("ringfs_init: error - object size %d does not fit into a %d-byte sector\n",
               object_size, fs->flash->sector_size);
        return -1;
    }

    return 0;
}

int ringfs_format(struct ringfs *fs)
{
    /* Пометить все секторы, чтобы предотвратить частично отформатированные ФС. */
    for (int sector=0; sector<fs->flash->sector_count; sector++)
        _sector_set_status(fs, sector, SECTOR_FORMATTING);

    /* Стереть, обновить версию, пометить как свободный. */
    for (int sector=0; sector<fs->flash->sector_count; sector++)
        _sector_free(fs, sector);

    /* Начать чтение и запись с первого сектора. */
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
    /* Сектор чтения — первый сектор IN_USE *после* сектора FREE
     * (или первый вообще). */
    int read_sector = 0;
    /* Сектор записи — последний сектор IN_USE *перед* сектором FREE
     * (или последний вообще). */
    int write_sector = fs->flash->sector_count - 1;
    /* В любой момент должен быть хотя бы один сектор FREE. */
    bool free_seen = false;
    /* Если нет ни одного сектора IN_USE, начинаем с первого. */
    bool used_seen = false;

    /* Перебор секторов. */
    for (int sector=0; sector<fs->flash->sector_count; sector++) {
        int addr = _sector_address(fs, sector);

        /* Чтение заголовка сектора. */
        struct sector_header header = {0};
        fs->flash->read(addr, &header, sizeof(header));

        /* Обнаружение частично отформатированных разделов. */
        if (header.status == SECTOR_FORMATTING) {
            printf("ringfs_scan: sector: %d addr: %d\r\n", sector, addr);
            printf("ringfs_scan: partially formatted partition\r\n");
            return -1;
        }

        /* Обнаружение и починка частично затёртых секторов. */
        if (header.status == SECTOR_ERASING || header.status == SECTOR_ERASED) {
            _sector_free(fs, sector);
            header.status = SECTOR_FREE;
            /* _sector_free() перезаписал поле версии, поэтому обновляем
             * кэшированную копию. Иначе устаревшее значение (0xFFFFFFFF для
             * затёртого сектора) не прошло бы проверку версии ниже и сделало
             * бы починку бесполезной. */
            header.version = fs->version;
        }

        /* Обнаружение повреждённых секторов. */
        if (header.status != SECTOR_FREE && header.status != SECTOR_IN_USE) {
            printf("ringfs_scan: corrupted sector %d\r\n", sector);
            return -1;
        }

        /* Обнаружение устаревших версий. Нельзя сделать это раньше, потому что
         * версия могла быть некорректной из-за частичного стирания. */
        if (header.version != fs->version) {
            printf("ringfs_scan: sector: %d addr: %d\r\n", sector, addr);
            printf("ringfs_scan: incompatible version 0x%08"PRIx32"\r\n", header.version);
            return -1;
        }

        /* Отметить наличие сектора FREE. */
        if (header.status == SECTOR_FREE)
            free_seen = true;

        /* Отметить наличие сектора IN_USE. */
        if (header.status == SECTOR_IN_USE)
            used_seen = true;

        /* Обновить секторы чтения и записи по указанным выше правилам. */
        if (header.status == SECTOR_IN_USE && previous_sector_status == SECTOR_FREE)
            read_sector = sector;
        if (header.status == SECTOR_FREE && previous_sector_status == SECTOR_IN_USE)
            write_sector = sector-1;

        previous_sector_status = header.status;
    }

    /* Обнаружение отсутствия сектора FREE. */
    if (!free_seen) {
        printf("ringfs_scan: invariant violated: no FREE sector found\r\n");
        return -1;
    }

    /* Начать запись с первого сектора, если ФС пуста. */
    if (!used_seen) {
        write_sector = 0;
    }

    /* Просканировать сектор записи, пропуская занятые слоты в начале. */
    fs->write.sector = write_sector;
    fs->write.slot = 0;
    while (fs->write.sector == write_sector) {
        uint32_t status;
        _slot_get_status(fs, &fs->write, &status);
        if (status == SLOT_ERASED)
            break;

        _loc_advance_slot(fs, &fs->write);
    }
    /* Если сектор был полон, теперь мы в начале сектора FREE. */

    /* Поместить головку чтения в начало первого сектора IN_USE, затем
     * пропустить мусорные/некорректные слоты, пока не найдётся что-то полезное
     * или мы не достигнем головки записи — значит, данных нет. */
    fs->read.sector = read_sector;
    fs->read.slot = 0;
    while (!_loc_equal(&fs->read, &fs->write)) {
        uint32_t status;
        _slot_get_status(fs, &fs->read, &status);
        if (status == SLOT_VALID)
            break;

        _loc_advance_slot(fs, &fs->read);
    }

    /* Переместить курсор чтения на позицию головки чтения. */
    fs->cursor = fs->read;

    return 0;
}

int ringfs_capacity(const struct ringfs *fs)
{
    /* Объявленная ёмкость как безопасный рабочий запас:
     * - один сектор всегда должен оставаться FREE (инвариант);
     * - один сектор зарезервирован для операций записи.
     * Поэтому объявленная ёмкость = (sector_count - 2) * slots_per_sector.
     * Примечание: физический жёсткий предел на один сектор больше
     * ((count-1)*slots), но хранение сверх объявленной ёмкости при следующем
     * append вызывает немедленное вытеснение целого сектора.
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

    /* Использовать временную позицию для перебора. */
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
     * В добавлении объекта участвуют три сектора:
     * - сектор, в который выполняется добавление: должен быть записываемым;
     * - следующий сектор: должен быть свободным (FREE, инвариант);
     * - при вытеснении следующего сектора головки чтения и курсора
     *   при необходимости передвигаются вперёд.
     */

    /* Убедиться, что следующий сектор свободен. */
    int next_sector = (fs->write.sector+1) % fs->flash->sector_count;
    _sector_get_status(fs, next_sector, &status);
    if (status != SECTOR_FREE) {
        /* Следующий сектор нужно освободить. Но сначала... */

        /* Убрать головки чтения и курсора с пути. */
        if (fs->read.sector == next_sector)
            _loc_advance_sector(fs, &fs->read);
        if (fs->cursor.sector == next_sector)
            _loc_advance_sector(fs, &fs->cursor);

        /* Освободить следующий сектор. */
        _sector_free(fs, next_sector);
    }

    /* Теперь убедимся, что текущий сектор записи доступен для записи. */
    _sector_get_status(fs, fs->write.sector, &status);
    if (status == SECTOR_FREE) {
        /* Свободный сектор. Пометить как используемый. */
        _sector_set_status(fs, fs->write.sector, SECTOR_IN_USE);
    } else if (status != SECTOR_IN_USE) {
        printf("ringfs_append: corrupted filesystem\r\n");
        return -1;
    }

    /* Предварительно зарезервировать слот. */
    _slot_set_status(fs, &fs->write, SLOT_RESERVED);

    /* Записать объект. */
    fs->flash->program(_slot_address(fs, &fs->write) + sizeof(struct slot_header),
            object, fs->object_size);

    /* Зафиксировать запись. */
    _slot_set_status(fs, &fs->write, SLOT_VALID);

    /* Продвинуть головку записи. */
    _loc_advance_slot(fs, &fs->write);

    return 0;
}

int ringfs_fetch(struct ringfs *fs, void *object)
{
    /* Двигаться вперёд в поисках валидного слота. */
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

        /* Чтение заголовка сектора. */
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
