#!/usr/bin/env python3
import sys
import re

if len(sys.argv) < 2:
    sys.exit(1)
trace_code_file = sys.argv[1]

id_name_pattern = re.compile(r'0x([0-9a-fA-F]+)\s+([^\s]*)')
code_table = []

with open(trace_code_file, 'rt') as codes:
    for line in codes:
        m = id_name_pattern.match(line)
        if m:
            code_table.append((int(m.group(1), base=16), m.group(2)))

print("typedef struct {")
print("        uint32_t   id;")
print("        const char *name;")
print("} kd_event_t;")
print("kd_event_t kd_events[] = {")
for mapping in sorted(code_table, key=lambda x: x[0]):
    print("        {0x%x, \"%s\"}," % mapping)
print("};")
