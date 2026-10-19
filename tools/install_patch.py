"""Copies the patch into a RoboRumble install.

    python tools/install_patch.py "D:\\Games\\...\\RoBoRumble"                       install
    python tools/install_patch.py "D:\\Games\\...\\RoBoRumble" --dgvoodoo <dgVoodoo dir>
    python tools/install_patch.py "D:\\Games\\...\\RoBoRumble" --remove              uninstall

Puts winmm.dll and rrpatch.ini into <game>\\DATA. An existing winmm.dll that is not ours
(GOG ships ogg-winmm) is kept as winmm.dll.orig and restored on --remove. rrpatch.ini is
only written when missing, so local settings survive reinstalls.

--dgvoodoo points at an unpacked dgVoodoo2 release (https://github.com/dege-diosg/dgVoodoo2).
Its 32-bit Glide and DirectDraw dlls replace nGlide / the system ddraw for rr_glide.exe and
rr_dx5.exe (originals kept as *.orig), with a dgVoodoo.conf that starts windowed. The patch
then handles the window and the mouse like in the software renderer.
"""
import argparse
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(ROOT, "patch", "build", "winmm.dll")
INI = os.path.join(ROOT, "patch", "rrpatch.ini")
MARKER = b"RoboRumble patch loaded"
DGV_FILES = [("3Dfx/x86", "Glide.dll"), ("3Dfx/x86", "Glide2x.dll"), ("3Dfx/x86", "Glide3x.dll"),
             ("MS/x86", "DDraw.dll"), ("MS/x86", "D3DImm.dll")]
DGV_SETTINGS = {
    # [General]
    "FullScreenMode": "false",
    "KeepWindowAspectRatio": "true",
    "CaptureMouse": "false",          # the patch confines the mouse itself
    "ScalingMode": "stretched_ar",
    # [Glide]
    "3DfxWatermark": "false",
    "3DfxSplashScreen": "false",
    # [DirectX]
    "AppControlledScreenMode": "false",
    "dgVoodooWatermark": "false",
}
DGV_SECTION_SETTINGS = {"Glide": {"Resolution": "2x"}, "DirectX": {"Resolution": "2x"}}


def is_ours(path):
    with open(path, "rb") as f:
        return MARKER in f.read()


def write_dgvoodoo_conf(template, target):
    section = None
    out = []
    with open(template, encoding="utf-8", errors="replace") as f:
        for line in f.read().splitlines():
            m = re.match(r"\s*\[(\w+)\]", line)
            if m:
                section = m.group(1)
            kv = re.match(r"(\s*)([A-Za-z0-9]+)(\s*)=", line)
            if kv and not line.lstrip().startswith(";"):
                key = kv.group(2)
                value = DGV_SECTION_SETTINGS.get(section, {}).get(key, DGV_SETTINGS.get(key))
                if value is not None:
                    line = f"{kv.group(1)}{key}{kv.group(3)}= {value}"
            out.append(line)
    with open(target, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(out) + "\n")


def install_dgvoodoo(data, dgv):
    for sub, name in DGV_FILES:
        src = os.path.join(dgv, *sub.split("/"), name)
        if not os.path.isfile(src):
            sys.exit(f"missing {src} - is this an unpacked dgVoodoo2 release?")
    for sub, name in DGV_FILES:
        target = os.path.join(data, name)
        # Windows names are case-insensitive: GOG's glide2x.dll is the same file as Glide2x.dll
        existing = next((f for f in os.listdir(data) if f.lower() == name.lower()), None)
        if existing and not os.path.exists(os.path.join(data, existing + ".orig")):
            os.replace(os.path.join(data, existing), os.path.join(data, existing + ".orig"))
            print(f"kept {existing} as {existing}.orig")
        shutil.copy2(os.path.join(dgv, *sub.split("/"), name), target)
    conf = os.path.join(data, "dgVoodoo.conf")
    if not os.path.exists(conf):
        write_dgvoodoo_conf(os.path.join(dgv, "dgVoodoo.conf"), conf)
    print("installed dgVoodoo (windowed)")


def remove_dgvoodoo(data):
    names = {n.lower() for _, n in DGV_FILES}
    for f in os.listdir(data):
        if f.lower() in names:
            os.remove(os.path.join(data, f))
    for f in os.listdir(data):
        if f.lower().endswith(".orig") and f[:-5].lower() in names:
            os.replace(os.path.join(data, f), os.path.join(data, f[:-5]))
            print(f"restored {f[:-5]}")
    conf = os.path.join(data, "dgVoodoo.conf")
    if os.path.exists(conf):
        os.remove(conf)


GLIDE_DLLS = ["glide.dll", "glide2x.dll", "glide3x.dll"]


def switch_glide(data, target):
    """Swaps the active Glide wrapper between the original (nGlide on GOG, kept as *.orig)
    and dgVoodoo (kept as *.dgvoodoo while switched off). dgVoodoo.conf is parked as
    dgVoodoo.conf.off with nGlide so the patch doesn't apply its dgVoodoo window handling."""
    files = {f.lower(): f for f in os.listdir(data)}
    conf, conf_off = os.path.join(data, "dgVoodoo.conf"), os.path.join(data, "dgVoodoo.conf.off")
    for name in GLIDE_DLLS:
        cur = os.path.join(data, files.get(name, name))
        orig = os.path.join(data, files.get(name + ".orig", name + ".orig"))
        dgv = os.path.join(data, files.get(name + ".dgvoodoo", name + ".dgvoodoo"))
        if target == "nglide" and os.path.exists(orig):
            if os.path.exists(cur):
                os.replace(cur, dgv)
            os.replace(orig, cur)
        elif target == "dgvoodoo" and os.path.exists(dgv):
            if os.path.exists(cur):
                os.replace(cur, orig)
            os.replace(dgv, cur)
    if target == "nglide" and os.path.exists(conf):
        os.replace(conf, conf_off)
    if target == "dgvoodoo" and os.path.exists(conf_off):
        os.replace(conf_off, conf)
    print(f"Glide wrapper: {target}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("--remove", action="store_true")
    ap.add_argument("--dgvoodoo", metavar="DIR", help="unpacked dgVoodoo2 release to install alongside")
    ap.add_argument("--glide", choices=["nglide", "dgvoodoo"], help="switch the Glide wrapper and exit")
    args = ap.parse_args()
    data = os.path.join(args.game_dir, "DATA")
    if not os.path.isfile(os.path.join(data, "rr_soft.exe")):
        sys.exit(f"{data} does not look like a RoboRumble DATA folder")
    if args.glide:
        switch_glide(data, args.glide)
        return
    target = os.path.join(data, "winmm.dll")
    backup = target + ".orig"

    if args.remove:
        if os.path.exists(target) and is_ours(target):
            os.remove(target)
            print("removed winmm.dll")
        if os.path.exists(backup):
            os.replace(backup, target)
            print("restored the original winmm.dll")
        remove_dgvoodoo(data)
        for leftover in ("rrpatch_adjust.ini", "rrpatch.log"):
            p = os.path.join(data, leftover)
            if os.path.exists(p):
                os.remove(p)
        gen = os.path.join(data, "rrpatch_gen")
        if os.path.isdir(gen):
            shutil.rmtree(gen)
        print("rrpatch.ini left in place")
        return

    if not os.path.isfile(DLL):
        sys.exit("build the dll first: sh patch/build.sh")
    if os.path.exists(target) and not is_ours(target) and not os.path.exists(backup):
        os.replace(target, backup)
        print("kept the existing winmm.dll as winmm.dll.orig")
    shutil.copy2(DLL, target)
    print(f"installed {target}")
    ini_target = os.path.join(data, "rrpatch.ini")
    if not os.path.exists(ini_target):
        shutil.copy2(INI, ini_target)
        print(f"installed {ini_target}")
    if args.dgvoodoo:
        install_dgvoodoo(data, args.dgvoodoo)


if __name__ == "__main__":
    main()
