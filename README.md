# SAINTS ROW: ReInstated

Saints Row (2009) and Saints Row 2 (2012), the Xbox 360 games, recompiled to run natively on the PS5,
in one app, by ChaosTheory1075.

> Built on [elripalda's GTA IV for PS5](https://github.com/elripalda/gta4-ps5), with GPU and timing
> work from [saulob's Crackdown Recompiled](https://github.com/saulob/crackdown-xbox360-recompiled).
> All the credit goes to them.

An unofficial fan project, not affiliated with or endorsed by Deep Silver, Volition, THQ,
Microsoft or Sony. **No game files are included:** you need your own copies of the games for the
Xbox 360.

## What's in it
- Saints Row and Saints Row 2, recompiled to run natively on the PS5 (not emulated), in one app: the
  menu's HOME page picks the game
- Saints Row 2's DLC
- Both games' achievements as PS5-style trophies, with their own page in the menu
- Installs the discs and the DLC from your own files, at the drive's full speed
- Imports your Xbox 360 saves, backs up and restores saves
- A menu with HOME, CONTENT, SAVES, SETTINGS, TROPHIES and CREDITS pages, each game's own art and
  music

## What you need
- A jailbroken PS5 that runs homebrew apps, with a payload loader on port 9021 (etaHEN or elfldr)
- Your **own** Saints Row and/or Saints Row 2 for the Xbox 360, as `.iso` files or the discs' files
- Optional: Saints Row 2's DLC packages
- No title updates: the app runs the discs' own code

| | File | Title id |
|---|---|---|
| Saints Row | `Saints Row (World) (En,Fr,Es,It).iso` (any name) | 4156084E |
| Saints Row 2 | `Saints Row 2 (USA, Europe).iso` (any name) | 415608A7 |
| Saints Row 2 DLC | the packages in `415608A7/00000002` | |

## Install
1. Copy the `PPSA54107` folder to the PS5, wherever your homebrew loader looks (`/data/homebrew/`,
   `/mnt/ext0/homebrew/`, `/mnt/usb0/homebrew/`, ...).
2. Put your disc images (or the discs' files) and the DLC packages in `PPSA54107/install/`.
3. Open **SAINTS ROW: ReInstated**. It installs everything it finds, one at a time.
4. When PLAY lights up, you're set.

See [package/README.txt](package/README.txt) for the rest.

## How it's put together
Each game is its own recompiled program: `eboot.bin` holds the menu and Saints Row, `sr2.bin` holds
Saints Row 2, which the menu starts in its place (`sceSystemServiceLoadExec`). Both are built from
`sr-recomp/` (`SR_GAME` 1 or 2), each from its own codegen (`sr-recomp/p1`, `p2`).

## Build from source
The build runs on Ubuntu 24.04 (WSL works), as root, with your own game files:

```bash
bash tools/local/toolchain.sh   # the PS5 toolchain (ps5-plume-triangle), once
bash tools/local/framework.sh   # gta4-ps5's framework and the Crackdown PC recomp's patches, once
SR1_XEX=<Saints Row's default.xex> SR2_XEX=<Saints Row 2's default.xex> \
  bash tools/local/build.sh . <where to put the PPSA54107 folder>
```

The build checks the two files by their SHA-1s (`b95f841c...` and `50c1220b...`). It applies
`ci/pc_patches.py`, `ci/fw_perf_fixes.py`, `ci/patch_framework.py` and `ci/sr_codegen.py` (dcbz
clears the Xbox 360's whole 128-byte cache line, as Xenia Canary does since it fixed Saints Row's
collision) to gta4-ps5's framework, recompiles both games (ReXGlue codegen), builds and signs both
programs and the installer payload, and packages the app folder. `tools/make_sr_art.py` makes the
menu's and home screen's art from your own pictures.

## Credits
- **elripalda**: [GTA IV for PS5](https://github.com/elripalda/gta4-ps5), the PS5 port this one is
  built on
- **saulob**: [Crackdown Recompiled](https://github.com/saulob/crackdown-xbox360-recompiled)
- **OZORDI** and **vaduur**: Liberty Recompiled and LibertyRecompAndroid
- **ReXGlue** and the **Xenia** team
- **BlackBearReloaded**: the PS5 AT9 Converter (the home screen music)
- Built with the PS5 Payload SDK, Mesa RADV, Dear ImGui, stb_image, dr_mp3, zlib, xxHash, and the
  Oswald, Inter and Pirata One fonts (SIL Open Font License)
- Special thanks to **Volition**, for Saints Row

## License
GNU General Public License v3.0 (see [LICENSE](LICENSE)), as GTA IV for PS5 is. No game files are
included. Saints Row and all related names belong to their owners.
