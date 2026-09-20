# RVGL — Nintendo Switch port (SDL2 / ENet / OpenAL wrapper)

This is a native wrapper / loader that runs the original ARM64 Android build of RVGL on Switch homebrew. It contains no game code and no game assets — it loads RVGL's own libraries and recreates, natively, the Android layer underneath them: bionic's C library, OpenSL ES audio, BSD sockets, the software keyboard, and the SDL surface the engine expects.

## Install & run


You need the RGVL 23.1030a1.apk
```
sdmc:/switch/<any name>
├── revoltnx.nro
├── libmain.so  libopenal.so  libmpg123.so  libsndfile.so  libunistring.so
├── cursor.png                              <- optional
└── assets 
```

Launch via title override (hold R while starting an installed game).

## Controls

RVGL reads the pad as an SDL game controller, so the game's own bindings apply. Text entry uses the Switch software keyboard.

| Input | Action |
|---|---|
| Pad | The game as designed — remappable in RVGL's own menus |
| Wheel or keyboard | Name entry; both work |
| ZL + ZR | Toggle the on-screen cursor |
| Left stick (cursor up) | Move the cursor |
| A (cursor up) | Tap at the cursor — hold to drag |

Optionally drop a cursor.png (up to 64×64, transparency respected) beside the NRO to replace the built-in arrow.

## Multiplayer
Local multiplayer works across devices i.e. Android, Windows on the same Wi-Fi network just leave the Computer Name field empty and attempt to connect.

## Building
```
Requires devkitPro with the switch-dev group plus these portlibs. switch-sdl2_image is optional — RVGL imports four of its symbols and the build stands in for them when it is absent.

pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib

export DEVKITPRO=/opt/devkitpro
make                        # -> revoltnx.nro
```
## Credits

The loader/shim infrastructure (so_util, the bionic shims, libc_shim, fakefd, opensles) derives from the open-source Switch .so-loader lineage — Andy Nguyen and fgsfds, building on TheOfficialFloW's Vita/Switch loader tradition — by way of the Papa Pear Switch port, from which most of the game-agnostic code here is taken unchanged.
