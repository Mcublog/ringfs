/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include "flashsim.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

/* Результаты I/O сохраняются в переменные ДО assert (побочных эффектов
 * в assert() быть не должно). В NDEBUG assert вырезается, поэтому такие
 * переменные помечаются использованными явным (void) — иначе сборка с
 * -DNDEBUG -Wall -Wextra -Werror падает на -Wunused-*. */

#ifdef FLASHSIM_LOG
#define logprintf(args...) printf(args)
#else
#define logprintf(args...) do {} while (0)
#endif

struct flashsim {
    int size;
    int sector_size;
    const char *name;

    FILE *fh;
};

static const char *kOpenMode = "r+";
static const char *kCreateMode = "w+";

struct flashsim *flashsim_open(const char *name, int size, int sector_size)
{
    struct flashsim *sim = malloc(sizeof(struct flashsim));
    assert(sim != NULL);

    sim->size = size;
    sim->sector_size = sector_size;
    sim->name = name;

    sim->fh = fopen(name, kOpenMode);
    if (sim->fh == NULL)
    {
        logprintf("create new file: %s", name);
        sim->fh = fopen(name, kCreateMode);
        assert(sim->fh != NULL);
    }

    /* Размер файла обязан быть ровно size. Существующий файл может быть
     * обрезан (падение во время записи), а новый создаётся нулевым.
     * Недостающая часть заполняется 0xFF — состояние стёртого NOR:
     * нули читаются ringfs_scan как SECTOR_FORMATTING (частично
     * отформатированная партиция), а чтение за концом файла падает. */
    struct stat st;
    int rc = fstat(fileno(sim->fh), &st);
    assert(rc == 0);
    (void)rc;

    if (st.st_size < (off_t)size)
    {
        off_t filled = st.st_size;
        rc = ftruncate(fileno(sim->fh), size);
        assert(rc == 0);

        uint8_t erased[64];
        memset(erased, 0xff, sizeof(erased));

        rc = fseek(sim->fh, filled, SEEK_SET);
        assert(rc == 0);
        while (filled < (off_t)size)
        {
            int chunk = ((off_t)size - filled) < (off_t)sizeof(erased)
                            ? (int)((off_t)size - filled)
                            : (int)sizeof(erased);
            size_t written = fwrite(erased, 1, chunk, sim->fh);
            assert(written == (size_t)chunk);
            (void)written;
            filled += chunk;
        }
    }
    else if (st.st_size > (off_t)size)
    {
        rc = ftruncate(fileno(sim->fh), size);
        assert(rc == 0);
    }

    assert(sim->fh != NULL);
    return sim;
}

void flashsim_close(struct flashsim *sim)
{
    fclose(sim->fh);
    free(sim);
}

void flashsim_sector_erase(struct flashsim *sim, uint32_t addr)
{
    int sector_start = addr - (addr % sim->sector_size);
    logprintf("flashsim_erase  (0x%08x) * erasing sector at 0x%08x\n", addr, sector_start);

    void *empty = malloc(sim->sector_size);
    assert(empty != NULL);
    memset(empty, 0xff, sim->sector_size);

    int rc = fseek(sim->fh, sector_start, SEEK_SET);
    assert(rc == 0);
    (void)rc;
    size_t written = fwrite(empty, 1, sim->sector_size, sim->fh);
    assert(written == (size_t) sim->sector_size);
    (void)written;

    free(empty);
}

void flashsim_read(struct flashsim *sim, uint32_t addr, uint8_t *buf, int len)
{
    int rc = fseek(sim->fh, addr, SEEK_SET);
    assert(rc == 0);
    (void)rc;
    size_t nread = fread(buf, 1, len, sim->fh);
    assert(nread == (size_t) len);
    (void)nread;

    logprintf("flashsim_read   (0x%08x) = %d bytes [ ", addr, len);
    for (int i=0; i<len; i++) {
        logprintf("%02x ", buf[i]);
        if (i == 15) {
            logprintf("... ");
            break;
        }
    }
    logprintf("]\n");
}

void flashsim_program(struct flashsim *sim, uint32_t addr, const uint8_t *buf, int len)
{
    logprintf("flashsim_program(0x%08x) + %d bytes [ ", addr, len);
    for (int i=0; i<len; i++) {
        logprintf("%02x ", buf[i]);
        if (i == 15) {
            logprintf("... ");
            break;
        }
    }
    logprintf("]\n");

    uint8_t *data = malloc(len);
    assert(data != NULL);

    int rc = fseek(sim->fh, addr, SEEK_SET);
    assert(rc == 0);
    (void)rc;
    size_t nread = fread(data, 1, len, sim->fh);
    assert(nread == (size_t) len);
    (void)nread;

    for (int i=0; i<(int) len; i++)
        data[i] &= buf[i];

    rc = fseek(sim->fh, addr, SEEK_SET);
    assert(rc == 0);
    size_t written = fwrite(data, 1, len, sim->fh);
    assert(written == (size_t) len);
    (void)written;

    /* Нужно корректно сбросить данные в файл */
    fflush(sim->fh);
    fclose(sim->fh);
    sim->fh = fopen(sim->name, kOpenMode);

    free(data);
}

/* vim: set ts=4 sw=4 et: */
