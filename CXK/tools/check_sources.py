#!/usr/bin/env python3
# /CXK/tools/check_sources.py
# Aurora Tejeda / CATX SYSTEMS LLC
# Pre-flight: verify every source path referenced in CMakeLists.txt exists on
# disk, so a missing file fails fast with a clear message instead of a cryptic
# NMAKE U1073 partway through the build.
import re, sys, os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
cmake = os.path.join(root, "CMakeLists.txt")
text = open(cmake, encoding="utf-8", errors="replace").read()

# resolve ${SRC_DIR} (set near the top of CMakeLists) so paths can be checked
m = re.search(r'set\(\s*SRC_DIR\s+([^\)]+)\)', text)
src_dir = m.group(1).strip() if m else "${CMAKE_SOURCE_DIR}"
src_dir = src_dir.replace("${CMAKE_SOURCE_DIR}", root).replace("${CMAKE_CURRENT_SOURCE_DIR}", root)
src_dir = os.path.normpath(src_dir) if os.path.isabs(src_dir) else root

missing = []
for raw in re.findall(r'\$\{SRC_DIR\}/([^\s\)"]+\.(?:c|asm|nasm))', text):
    path = os.path.normpath(os.path.join(src_dir, raw))
    if not os.path.isfile(path):
        missing.append(raw)

if missing:
    print("  [pre-flight] MISSING source files referenced by CMakeLists.txt:")
    for mfile in missing:
        print(f"      - {mfile}")
    print("  Place the file(s) and rebuild.")
    sys.exit(1)

print(f"  [pre-flight] all referenced sources present (checked under {os.path.relpath(src_dir, root) or '.'})")
sys.exit(0)