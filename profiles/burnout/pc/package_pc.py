"""Burnout Legends PC: the folder to play from (no game file in it).

usage: python package_pc.py BUILD_DIR OUT_DIR
  BUILD_DIR: where burnout_legends.exe was built (with fonts/, postcards/,
             signatures/ copied beside it by the build)
  OUT_DIR:   the player's folder; its data/, settings.ini, controls.ini and
             saves are kept (an update only replaces the program and pictures)
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 1
    build, out = argv[1], argv[2]
    os.makedirs(out, exist_ok=True)
    shutil.copy2(os.path.join(build, 'burnout_legends.exe'), out)
    for folder in ('fonts', 'postcards', 'signatures'):
        src = os.path.join(build, folder)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(out, folder), dirs_exist_ok=True)
        else:
            print('missing: ' + src)
    shutil.copy2(os.path.join(HERE, 'LISEZMOI.txt'), out)
    print('ready: ' + out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
