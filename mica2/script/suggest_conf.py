#!/usr/bin/env python3

import re

seen_physical_core_ids = set()
lcore_ids = []

for line in open('/proc/cpuinfo').readlines():
  if re.match(r'^processor\s*:.*', line) is not None:
    processor_id = int(line.partition(':')[2])
  elif re.match(r'^physical id\s*:.*', line) is not None:
    physical_id = int(line.partition(':')[2])
  elif re.match(r'^core id\s*:.*', line) is not None:
    core_id = int(line.partition(':')[2])

  if not line.strip():
    if (physical_id, core_id) not in seen_physical_core_ids:
      seen_physical_core_ids.add((physical_id, core_id))
      lcore_ids.append(processor_id)

print('[partitions]')
print('lcores = %s' % str(lcore_ids))
print('partition_count = %d' % len(lcore_ids))
