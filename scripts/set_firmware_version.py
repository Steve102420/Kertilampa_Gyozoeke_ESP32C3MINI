import os
import re
import subprocess

Import("env")

version = os.environ.get("FIRMWARE_VERSION", "development")
if not re.fullmatch(r"[A-Za-z0-9._-]+", version):
    raise RuntimeError("FIRMWARE_VERSION contains unsupported characters")

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", '\\"%s\\"' % version)])
print("Firmware version: %s" % version)
