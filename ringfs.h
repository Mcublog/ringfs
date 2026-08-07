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
 * @defgroup ringfs_api API RingFS
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
      * Описатель раздела flash-памяти.
      */
    typedef struct ringfs_flash_partition
    {
        int sector_size;   /**< Размер сектора, в байтах. */
        int sector_offset; /**< Смещение раздела, в секторах. */
        int sector_count;  /**< Размер раздела, в секторах. */

        /**
         * Стирает сектор.
         * @param address Любой адрес внутри сектора.
         * @returns Ноль при успехе, -1 при ошибке.
         */
        int (*sector_erase)(uint32_t address);
        /**
         * Программирует биты flash-памяти, переключая их с 1 на 0.
         * @param address Начальный адрес, в байтах.
         * @param data Данные для записи.
         * @param size Размер данных.
         * @returns size при успехе, -1 при ошибке.
         */
        ssize_t (*program)(uint32_t address, const void *data, size_t size);
        /**
         * Читает flash-память.
         * @param address Начальный адрес, в байтах.
         * @param data Буфер для сохранения прочитанных данных.
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
     * Экземпляр RingFS. Должен быть инициализирован вызовом ringfs_init()
     * перед использованием. К полям структуры нельзя обращаться напрямую.
     */
    typedef struct ringfs
    {
        /* Постоянные значения, задаются один раз в ringfs_init(). */
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
     * Инициализирует экземпляр RingFS. Должен быть вызван до использования
     * экземпляра с остальными функциями ringfs_*.
     *
     * @param fs Инициализируемый экземпляр RingFS.
     * @param flash Интерфейс flash-памяти. Должен быть реализован извне.
     * @param version Версия объектов. Следует увеличивать при любом
     *                обратно-несовместимом изменении семантики или размера.
     * @param object_size Размер одного хранимого объекта, в байтах.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_init(struct ringfs *fs, const struct ringfs_flash_partition *flash,
                    uint32_t version, int object_size);

    /**
     * Форматирует flash-память.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_format(struct ringfs *fs);

    /**
     * Сканирует flash-память в поисках корректной файловой системы.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_scan(struct ringfs *fs);

    /**
     * Вычисляет максимальную ёмкость RingFS.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Максимальная ёмкость; 0 при некорректном разделе.
     */
    int ringfs_capacity(const struct ringfs *fs);

    /**
     * Вычисляет приблизительное количество объектов.
     * Выполняется за O(1).
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Оценка количества объектов.
     */
    int ringfs_count_estimate(const struct ringfs *fs);

    /**
     * Вычисляет точное количество объектов.
     * Выполняется за O(n).
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Точное количество объектов.
     */
    int ringfs_count_exact(struct ringfs *fs);

    /**
     * Добавляет объект в конец кольца. При необходимости удаляет самые старые
     * объекты.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @param object Сохраняемый объект.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_append(struct ringfs *fs, const void *object);

    /**
     * Извлекает следующий объект из кольца (от старых к новым). Продвигает
     * курсор чтения.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @param object Буфер для сохранения извлечённого объекта.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_fetch(struct ringfs *fs, void *object);

    /**
     * Отбрасывает все извлечённые объекты вплоть до курсора чтения.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_discard(struct ringfs *fs);

    /**
     * Перематывает курсор чтения обратно к самому старому объекту.
     *
     * @param fs Инициализированный экземпляр RingFS.
     * @returns Ноль при успехе, -1 при ошибке.
     */
    int ringfs_rewind(struct ringfs *fs);

    /**
     * Выводит метаданные файловой системы. Для отладки.
     * @param stream Файловый поток для вывода.
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
