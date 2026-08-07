# RingFS changelog

## 0.2.1, unreleased

* BUGFIX: ringfs_scan() now refreshes the cached sector version after repairing
  a partially-erased sector. Previously the stale value (0xFFFFFFFF) failed the
  version check, making the recovery path useless.
* BUGFIX: ringfs_init() rejects non-positive object sizes and objects that do
  not fit into a sector alongside the headers.
* Capacity semantics aligned: ringfs_capacity() returns the declared safe
  capacity ((sector_count - 2) * slots_per_sector); the C tests were updated to
  match and the overflow test now exercises eviction at the hard limit.
* Python test bindings ported to Python 3. Fixed RingFS.fetch() which wrongly
  called ringfs_append() instead of ringfs_fetch(); fuzzer now actually reads
  data. Fuzzer generates partitions with at least MIN_SECTOR_COUNT (3) sectors.
* Fuzzer runs in a dedicated virtualenv (env/).

## 0.2.0, released 2014/05/07

* BUGFIX: used_seen was not updated in ringfs_scan(), causing corruption.
* Improved tests.
* Basic Python bindings.
* Basic fuzzing support.

## 0.1.0, released 2014/04/28

First "full" release. Entire API is implemented but subject to change.
Tests cover most common cases.
