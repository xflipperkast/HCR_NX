<div align="center">

<img src="icon.jpg" alt="hcr_nx" width="160">

# hcr_nx

**Hill Climb Racing on Nintendo Switch**

An unofficial Nintendo Switch port of the Android version of  
**Hill Climb Racing 1.71.1**

[![Switch](https://img.shields.io/badge/Nintendo_Switch-Homebrew-E60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#)
[![Ko-fi](https://img.shields.io/badge/Support_on_Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/flippyy)

</div>

---

## About

`hcr_nx` is an experimental port of **Hill Climb Racing Android 1.71.1** to
Nintendo Switch.

The project provides the Switch-side compatibility layer needed to run the
Android game code under Horizon OS, including asset loading, JNI shims,
OpenGL ES, audio, input, and offline platform fallbacks.

> This repository does **not** include Hill Climb Racing game assets, game
> binaries, or other proprietary files.

---

## Build

### Requirements

- [devkitPro](https://devkitpro.org/)
- devkitA64
- libnx
- GNU Make
- `switch-dev`
- `switch-mesa`
- `switch-libdrm_nouveau`
- `switch-sdl2`
- `switch-zlib`
- Your own legally obtained **Hill Climb Racing 1.71.1 APK**

### Compile

From the `hcr_nx` directory, make sure your devkitPro environment is
configured:

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
```

Build the NRO:

```bash
make -j
```

To rebuild from scratch:

```bash
make clean
make -j
```

On Windows with WSL and devkitPro installed, run:

```bat
build_clean.bat
```

The build produces `hcr_nx.nro` in this directory.

---

## Running

Create this folder on your SD card and copy the two required files into it:

```text
sdmc:/switch/hcr_nx/
  hcr_nx.nro
  hcr.apk
```

`hcr.apk` must be your legally obtained Android 1.71.1 APK. On first launch,
the loader extracts the required `lib/arm64-v8a/libgame.so` and `assets/`
files, then keeps the APK available for the game's asset loader.

Launch the application through a title override by holding **R** while
starting a game. Album/applet mode does not work properly for this port.

Touchscreen input maps directly to the Android game. Controller input also
provides a virtual cursor and mapped driving controls.

---

## Status

`hcr_nx` is still under development and should be considered experimental
until tested on an actual Nintendo Switch.

---

## Support

If you like the project and want to support development:

<div align="center">

[![Support me on Ko-fi](https://img.shields.io/badge/%E2%98%95_Support_me_on_Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/flippyy)

</div>

---

## Disclaimer

This is an unofficial fan-made project and is not affiliated with or endorsed
by **Fingersoft**.

Hill Climb Racing and all related trademarks, artwork, audio, game assets, and
other copyrighted material belong to their respective owners.

Do not distribute the original APK, extracted game assets, or proprietary game
binaries with this project.

---

## Contribution

Contributions are welcome if they provide meaningful fixes, compatibility
improvements, or performance work. Please open an issue for bugs or submit a
well-documented pull request with testing details and a clear description of
the change.
