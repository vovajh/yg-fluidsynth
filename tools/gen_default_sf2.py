#!/usr/bin/env python3
"""sf2 -> src/default_sf2.c (встроенный SoundFont, чтобы рендер работал «из коробки»).
Использование: python3 tools/gen_default_sf2.py <файл.sf2> src/default_sf2.c"""
import sys
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as f:
    f.write("/* Сгенерировано tools/gen_default_sf2.py. VintageDreamsWaves-v2.sf2, (c) Ian Wilson — см. licenses/ */\n")
    f.write("#include <stddef.h>\nconst unsigned char yg_default_sf2[] = {\n")
    for i in range(0, len(data), 24):
        f.write(",".join(str(b) for b in data[i:i+24]) + ",\n")
    f.write("};\nconst size_t yg_default_sf2_size = sizeof(yg_default_sf2);\n")
