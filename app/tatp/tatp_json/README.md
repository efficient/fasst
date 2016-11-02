# Table bucket capacities
 * On average, every worker adds `SUBSCRIBERS_PER_MACHINE` (1 M) subscribers to
its `SUBSCRIBER` table partition.

 * There are 2.5 `ACCESS_INFO` records per subscriber, so 2.5 M `ACCESS_INFO`
records in total. Similarly, there are 2.5 M `SPECIAL_FACILITY` records.

 * There are 1.25 `CALL_FORWARDING` records per `SPECIAL_FACILITY` record, so
around 3.2 M `CALL_FORWARDING` records in total.

## Index sizing
We allocate 25% extra space in the index for all records:
 * `SUBSCRIBER`: 1.25 M
 * Secondary `SUBSCRIBER` table: 1.25 M
 * `ACCESS_INFO`: 3.2 M -> Does not work so make it 4 M
 * `SPECIAL_FACILITY`: 3.2 M -> Does not work so make it 4 M
 * `CALL_FORWARDING`: 4 M -> Does not work so make it 5 M

# Additional info
## Table key-value sizes
 * All key sizes are fixed at 8 bytes. Value sizes are padded to next multiple of
   8 bytes
 * `SUBSCRIBER`: 40 bytes
 * Secondary `SUBSCRIBER` table: 8 bytes
 * `ACCESS_INFO`: 16 bytes
 * `SPECIAL_FACILITY`: 8 bytes
 * `CALL_FORWARDING`: 24 bytes

## Pool sizing (irrelevant for FixedTable)
On top of an (assumed) 32-byte per-entry pool overhead, allocate 0% extra
pool space. Also add the 8-byte HoTS object header to make the total header
size = 40 bytes.
 * `SUBSCRIBER`: `(40 + 40) * .125 = 10 MB`
 * Secondary `SUBSCRIBER` table: `(40 + 8) * .125 = 6 MB`
 * `ACCESS_INFO`: `(40 + 16) * .32 = 18 MB`
 * `SPECIAL_FACILITY`: `(40 + 8) * .32 = 16 MB`
 * `CALL_FORWARDING`: `(40 + 24) * .4 = 26 MB`
