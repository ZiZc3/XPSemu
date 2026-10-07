#!/usr/bin/env python3
from pathlib import Path
import json

here = Path(__file__).resolve().parent
source = here / "default-xemu.toml"
output = here / "default-xemu-config.h"
contents = source.read_text(encoding="utf-8")
generated = (
    "/* Generated from ps5/default-xemu.toml by embed-default-config.py. */" + chr(10)
    + "static const char XEMU_PS5_DEFAULT_CONFIG[] = "
    + json.dumps(contents)
    + ";" + chr(10)
)
if not output.exists() or output.read_text(encoding="utf-8") != generated:
    output.write_text(generated, encoding="utf-8")
