#!/usr/bin/env bash
# license: the release's LICENSE text (GPL-3.0 for the program as a whole, then the MIT and
# third-party notices). Sourced by tools/package.sh and tools/make_installer.sh; needs $R (this
# folder) and $W (the Windows build folder).
license() { # the program's license (GPL-3.0, because of N64ModernRuntime), ours, then the others'
  printf 'This program as a whole is distributed under the GNU General Public License, version 3\n'
  printf '(full text at the end of this file), because it includes N64ModernRuntime, which is under\n'
  printf 'that license. Its source code: https://github.com/studioa2kiadev/ConkerBFDRedux\n'
  printf '(Conker Redux, a fork of Conker'"'"'s Bad Fur Day: Reloaded,\nhttps://github.com/DahSidiAbdallah/ConkerBFDReloaded; its README lists everyone it is built on.)\n'
  printf 'The project'"'"'s own code is also available under the MIT License below.\n\n'
  printf -- '----------------------------------------------------------------------\nConker'"'"'s Bad Fur Day: Reloaded (MIT)\n----------------------------------------------------------------------\n\n'
  cat "$R/LICENSE"
  printf '\n\n----------------------------------------------------------------------\nThird-party software\n----------------------------------------------------------------------\n\n'
  printf 'Parts of this program come from CBFD-Recompiled (https://github.com/sciaschi/CBFD-Recompiled):\n\n'
  cat "$R/recomp/THIRD_PARTY_LICENSE"
  printf '\n\nRT64 (https://github.com/rt64/rt64):\n\n'
  tr -d '\r' < "$W/deps/rt64/LICENSE"
  printf '\n\nAlso built with N64Recomp (MIT), N64ModernRuntime (GPL-3.0, below) and RecompFrontend\n(https://github.com/N64Recomp; no license file published), RmlUi (MIT), SDL2 (zlib license)\nand the DirectX Shader Compiler (University of Illinois/NCSA license). The fonts in assets are\nunder the SIL Open Font License (license files next to them). Also: miniz (MIT),\nnativefiledialog-extended (zlib), controller mappings from SDL_GameControllerDB (zlib), the\nmkst/conker decompilation with splat and spimdisasm, and an installer made with Inno Setup.\nThe GAME SETTINGS, VIDEO, CAMERA FOLLOW and FREECAM word art is studioa2kiadev'"'"'s own, dedicated\nto the public domain under CC0 1.0 (assets/redux_menu/REDUX_ART_LICENSE.txt); the other\nmenu art is Rare'"'"'s, read at runtime from your own ROM and not included.\n\nConker'"'"'s Bad Fur Day is (c) Rare / Microsoft. This project is not affiliated with them.\n'
  printf '\n\n----------------------------------------------------------------------\nGNU General Public License, version 3 (N64ModernRuntime, and this program as a whole)\n----------------------------------------------------------------------\n\n'
  tr -d '\r' < "$W/deps/N64ModernRuntime/COPYING"
}
