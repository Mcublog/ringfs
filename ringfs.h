/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#ifndef RINGFS_H
#define RINGFS_H

/**
 * @defgroup ringfs_api RingFS API
 * @{
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * Дескриптор Flash памяти и раздела.
     */
    typedef struct ringfs_flash_partition
    {
        int sector_size;   /**< Размер сектора в байтах. */
        int sector_offset; /**< Смещение раздела в секторах. */
        int sector_count;  /**< Размер раздела в секторах. */

        /**
         * Стереть сектор.
         * @param address Любой адрес внутри сектора.
         * @returns Ноль при успехе, -1 при ошибке.
         */
        int (*sector_erase)(uint32_t address);
        /**
         * Запрограммировать бит Flash памяти (переключение с 1 на 0).
         * @param address Начальный адрес в байтах.
         * @param data Данные для записи.
         * @param size Размер данных.
         * @returns size при успехе, -1 при ошибке.
         */
        ssize_t (*program)(uint32_t address, const void *data, size_t size);
        /**
         * Прочитать Flash память.
         * @param address Начальный адрес в байтах.
         * @param data Буфер для хранения прочитанных данных.
         * @param size Размер данных.
         * @returns size при успехе, -1 при ошибке.
         */
        ssize_t (*read)(uint32_t address, void *data, size_t size);
    } ringfs_flash_partition_t;

    /** @private */
    typedef struct ringfs_loc
    {
        int sector;
        int slot;
    } ringfs_loc_t;

    /**
     * Экземпляр RingFS. Должен быть инициализирован с помощью ringfs_init() перед использованием.
     * Поля структуры не должны быть доступны напрямую.
     * */
    typedef struct ringfs
    {
        /* Постоянные значения, установленные один раз при ringfs_init(). */
        const struct ringfs_flash_partition *flash;
        uint32_t version;
        int object_size;
        /* Кэшированные значения. */
        int slots_per_sector;

        /* Указатели чтения/записи. Изменяются по мере необходимости. */
        struct ringfs_loc read;
        struct ringfs_loc write;
        struct ringfs_loc cursor;
    } ringfs_t;
    /**
     * Инициализировать экземпляр RingFS. Должна быть вызвана перед использованием
     * экземпляра с другими функциями ringfs_*.
     *
     * @param fs Экземпляр RingFS для инициализации.
     * @param flash Интерфейс Flash памяти. Должен быть реализован внешним образом.
     * @param version Версия объекта. Должна быть увеличена, когда семантика
     *                или размер объекта изменяются несовместимым образом.
     * @param object_size Размер одного сохраняемого объекта в байтах.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_init(struct ringfs *fs, const struct ringfs_flash_partition *flash,
                    uint32_t version, int object_size);

    /**
     * Отформатировать Flash память.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_format(struct ringfs *fs);

    /**
     * Просканировать Flash память в поиске валидной файловой системы.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_scan(struct ringfs *fs);

    /**
     * Вычислить максимальную ёмкость RingFS.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Максимальная ёмкость при успехе, -1 при ошибке.
     */
    int ringfs_capacity(const struct ringfs *fs);

    /**
     * Вычислить приблизительное количество объектов.
     * Работает за O(1).
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Приблизительное количество объектов при успехе, -1 при ошибке.
     */
    int ringfs_count_estimate(const struct ringfs *fs);

    /**
     * Вычислить точное количество объектов.
     * Работает за O(n).
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Точное количество объектов при успехе, -1 при ошибке.
     */
    int ringfs_count_exact(struct ringfs *fs);

    /**
     * Добавить объект в конец кольца. Удаляет самые старые объекты при необходимости.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @param object Объект для сохранения.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_append(struct ringfs *fs, const void *object);

    /**
     * Получить следующий объект из кольца, начиная с самого старого. Продвигает курсор чтения.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @param object Буфер для хранения полученного объекта.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_fetch(struct ringfs *fs, void *object);

    /**
     * Отбросить все полученные объекты до курсора чтения.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_discard(struct ringfs *fs);

    /**
     * Вернуть курсор чтения на самый старый объект.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_rewind(struct ringfs *fs);

    /**
     * Вывести метаданные файловой системы. Для целей отладки.
     * @param stream Поток файла для записи.
     * @param fs Инициализированный экземпляр RingFS.
     */
    void ringfs_dump(FILE *stream, struct ringfs *fs);

    /**
     * @}
     * @}
  */

#ifdef __cplusplus
}
#endif

#endif

/* vim: set ts=4 sw=4 et: */
