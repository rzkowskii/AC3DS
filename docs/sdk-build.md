# Building on Apple Silicon

Use a complete devkitPro 3DS SDK with devkitARM, libctru, Citro3D, Picasso, 3dsxtool, smdhtool, and the 3DS zlib, Tremor and Ogg portlibs. Install Git, CMake 3.20 or later and Python 3 on the host. Ninja is optional. Follow the [devkitPro installation guide](https://devkitpro.org/wiki/Getting_Started).

Set `DEVKITPRO` to your SDK root. Set `DEVKITARM` if the compiler lives outside `$DEVKITPRO/devkitARM`. With the exact Lockdown Edition archive downloaded, run the commands below from the repository root.

```sh
cmake -G Ninja -S . -B build-3ds \
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-3ds.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DAC3DS_ASSET_ARCHIVE="$HOME/Downloads/AssaultCube_v1.3.0.2_LockdownEdition_RC1.tar.bz2"
cmake --build build-3ds --parallel 4
```

CMake fetches these SDL versions on first configure. Use `FETCHCONTENT_SOURCE_DIR_SDL2` and `FETCHCONTENT_SOURCE_DIR_SDL2_IMAGE` to supply matching local checkouts.

| Dependency | Revision |
| --- | --- |
| SDL2 2.32.10 | `5d249570393f7a37e037abf22cd6012a4cc56a71` |
| SDL2_image 2.8.8 | `c1bf2245b0ba63a25afe2f8574d305feca25af77` |

The development build used Arm GNU Toolchain 15.2.Rel1 with the devkitARM r68 target runtime, CMake 4.4.3 and Ninja 1.13.2. The toolchain targets ARMv6K with VFP and hard-float EABI. Link-time optimization is enabled by default. The resulting `assaultcube.elf` is for symbolication and `assaultcube.3dsx` is the Homebrew Launcher package. See the [installation guide](../README.md#install-on-your-handheld) for the SD layout.
