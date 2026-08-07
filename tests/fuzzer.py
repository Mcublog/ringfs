#!/usr/bin/env python
# -*- coding: utf-8 -*-

import os
import random

from pyflashsim import FlashSim
from pyringfs import RingFSFlashPartition, RingFS


def compare(a, b):
    if a != b:
        print(">>> %s != %s" % (a, b))
    return a == b


class FuzzRun(object):

    def __init__(self, name, version, object_size, sector_size, total_sectors, sector_offset, sector_count):

        print("FuzzRun[%s]: v=%08x os=%d ss=%d ts=%d so=%d sc=%d" % (name, version, object_size, sector_size,
                total_sectors, sector_offset, sector_count))

        # flashsim_open() reuses an existing file without resizing it; a stale
        # file from a previous (different-geometry) run would cause reads/writes
        # past EOF. Always start from a blank, correctly-sized flash.
        try:
            os.remove(name)
        except OSError:
            pass

        sim = FlashSim(name, total_sectors*sector_size, sector_size)

        def op_sector_erase(address):
            sim.sector_erase(address)
            return 0

        def op_program(address, data):
            sim.program(address, data)
            return len(data)

        def op_read(address, size):
            return sim.read(address, size)

        self.version = version
        self.object_size = object_size

        self.flash = RingFSFlashPartition(sector_size, sector_offset, sector_count,
                op_sector_erase, op_program, op_read)
        self.fs = RingFS(self.flash, self.version, self.object_size)

    def run(self):

        self.fs.format()
        self.fs.dump()

        def do_append():
            self.fs.append(b'x'*self.object_size)

        def do_fetch():
            self.fs.fetch()

        def do_rewind():
            self.fs.rewind()

        def do_discard():
            self.fs.discard()

        for i in range(1000):
            fun = random.choice([do_append]*100 + [do_fetch]*100 + [do_rewind]*10 + [do_discard]*10)
            print(i, fun.__name__)
            fun()

            # consistency check
            newfs = RingFS(self.flash, self.version, self.object_size)
            try:
                assert newfs.scan() == 0
                assert compare(newfs.ringfs.read.sector, self.fs.ringfs.read.sector)
                assert compare(newfs.ringfs.read.slot, self.fs.ringfs.read.slot)
                assert compare(newfs.ringfs.write.sector, self.fs.ringfs.write.sector)
                assert compare(newfs.ringfs.write.slot, self.fs.ringfs.write.slot)
            except AssertionError:
                print("self.fs:")
                self.fs.dump()
                print("newfs:")
                newfs.dump()
                raise


sector_size = random.randint(16, 256)
total_sectors = random.randint(3, 8)
sector_offset = random.randint(0, total_sectors-3)
sector_count = random.randint(3, total_sectors-sector_offset)
version = random.randint(0, 0xffffffff)
# object_size must leave room for the sector+slot headers in a single slot:
# slots_per_sector = (sector_size-8)/(object_size+4) >= 1
object_size = random.randint(1, sector_size-12)

f = FuzzRun('tests/fuzzer.sim', version, object_size, sector_size, total_sectors, sector_offset, sector_count)
f.run()
