"""pio_ota_sign.py: PlatformIO extra script that signs the app image for the
OTA loader after each build.

  [env:board]
  extra_scripts =
      ${env.extra_scripts}
      ../esp-rack/tools/pio_ota_sign.py
  custom_ota_sign = yes

Writes firmware.signed.bin (the image + the 128-byte signature trailer) and
firmware.signed.json (size, SHA-256, DER signature, key id: what a pull
handoff carries) next to firmware.bin. The key: see ota_sign.py (with no
production key a dev key is made on first use, and the build log says so).
A configured production key that is missing (ESPRACK_OTA_KEY) fails the
build. Without custom_ota_sign = yes the script does nothing.
"""

import inspect
import os
import subprocess

Import("env")  # noqa: F821  (PlatformIO injects)

HERE = os.path.dirname(os.path.abspath(inspect.getfile(inspect.currentframe())))
TOOL = os.path.join(HERE, "ota_sign.py")


def _enabled():
    v = env.GetProjectOption("custom_ota_sign", "no")  # noqa: F821
    return str(v).strip().lower() in ("yes", "true", "1", "on")


def _sign(source, target, env):
    image = str(target[0])  # firmware.bin (source: the .elf)
    base = image[:-4]
    cmd = [env.subst("$PYTHONEXE"), TOOL, "sign", image,
           "-o", base + ".signed.bin", "--json", base + ".signed.json"]
    rc = subprocess.call(cmd)
    if rc != 0:
        print("pio_ota_sign: signing failed (%d)" % rc)
    return rc


if _enabled():
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin",  # noqa: F821
                      env.VerboseAction(_sign, "Signing ${PROGNAME}.bin for the OTA loader"))  # noqa: F821
