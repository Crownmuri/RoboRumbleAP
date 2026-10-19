"""
Packs apworld/roborumble and the game patch (patch/build/winmm.dll) into build/roborumble.apworld.

    python tools/build_apworld.py
    python tools/build_apworld.py --install "D:\\Games\\Randomizer\\Archipelago\\custom_worlds"
"""
import argparse
import shutil
import zipfile

from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORLD = REPO / "apworld" / "roborumble"
OUTPUT = REPO / "build" / "roborumble.apworld"


def build() -> Path:
    OUTPUT.parent.mkdir(exist_ok=True)

    with zipfile.ZipFile(OUTPUT, "w", zipfile.ZIP_DEFLATED) as apworld:
        for path in sorted(WORLD.rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                apworld.write(path, Path("roborumble") / path.relative_to(WORLD))
        # the game patch, which the client's /patch and /run install
        dll = REPO / "patch" / "build" / "winmm.dll"
        if not dll.is_file():
            raise SystemExit("patch/build/winmm.dll is missing: run sh patch/build.sh first")
        apworld.write(dll, "roborumble/patch/winmm.dll")
        apworld.write(REPO / "patch" / "rrpatch.ini", "roborumble/patch/rrpatch.ini")

    return OUTPUT


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install", metavar="CUSTOM_WORLDS_DIR", help="also copy the apworld into this folder")
    args = parser.parse_args()

    output = build()
    print(f"Built {output}")

    if args.install:
        print(f"Installed {shutil.copy(output, args.install)}")
