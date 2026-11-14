SAINTS ROW: ReInstated
======================

Saints Row and Saints Row 2 (Xbox 360) recompiled to run natively on the PS5, in
one app: pick the game on the menu's HOME page. With both games' DLC, your
saves and both games' achievements as trophies. An unofficial fan project: no
game files are included, you need your own copies of the games.

What you need
-------------
- A PS5 that runs homebrew apps from /data/homebrew/ or homebrew/ on an
  external drive, with a payload loader on port 9021 (etaHEN or elfldr) for
  fast installs.
- Your Saints Row and/or Saints Row 2 disc images (.iso), or the discs' files.
- Each game's title update package: Saints Row's Title Update 1 and Saints
  Row 2's Title Update 3 (the programs are built from them: both are needed).
- Optional: either game's DLC packages.

Install
-------
1. Copy this whole PPSA54107 folder to the PS5's homebrew folder
   (/data/homebrew/, or homebrew/ on an external drive).
2. Put these in PPSA54107/install/ (see install/README.txt):
   - your disc images (.iso, any names), or folders of the discs' files,
   - the title update packages (TU_...),
   - DLC packages (optional).
   Extract any .7z / .rar / .zip on a PC first.
3. Open "SAINTS ROW: ReInstated". It installs what it finds, one at a time,
   with the progress on its HOME page (each disc image takes a few minutes).
   When PLAY lights up, you're set; the CONTENT page shows what's installed.
   At the end it asks whether to delete the files it installed from (X) or
   keep them (O).

The menu
--------
HOME      the two games (UP / DOWN chooses), PLAY; an install's progress;
          what's missing and where it goes.
CONTENT   Saints Row, Saints Row 2 (each with its title update), the DLC
          and HD textures: installed or not, X installs one from install/,
          triangle (twice) uninstalls it.
SAVES     BACK UP SAVES (to install/backups/), IMPORT an Xbox 360 save
          package from install/, RESTORE one of the backups.
SETTINGS  FRAME RATE        30, as on the Xbox 360, or 60 (experimental:
                            both games run twice as often where the PS5
                            keeps up; set it back to 30 if anything plays
                            too fast or stutters).
          UPSCALING         FSR 1 (AMD's upscaler, up to 4K) or BILINEAR.
          SHARPNESS         how much FSR 1 sharpens: LOW, MEDIUM or HIGH.
          ANTI-ALIASING     off, FXAA or SMAA.
          TEXTURE FILTERING 2x to 16x anisotropic.
          GPU HELPER        a second graphics thread (on); turn it off if
                            something draws wrong.
          FPS COUNTER       off, simple or detailed, at the left edge.
          TREE DETAIL       Saints Row: trees always at full detail.
          BUTTON PROMPTS    PlayStation or Xbox 360 buttons in the games.
          DUALSENSE FEATURES the light bar in the Saints' purple.
          LANGUAGE          AUTO (follows the PS5), English, French,
                            German, Italian, Spanish, or Portuguese (BR):
                            this menu and both games' text and subtitles
                            (this app's translation; voices stay English).
          SKIP INTRO        the games start at their title screens, without
                            the THQ and Volition logo movies.
          CHEAT MENU        the L3 + R3 quick menu on or off.
          RENDERER          ACCURATE (the Xbox 360's video memory, exactly)
                            or FAST (the PS5's own render targets: quicker;
                            some effects may draw wrong).
          MENU MUSIC        the menu song's level.
TROPHIES  each game's achievements (triangle: the other game's), earned ones
          first, with their dates (filled in once that game has been started).
CREDITS   who made it possible.
L1 / R1 change pages, O goes back to HOME. Settings are kept in menu.ini.

In the games
------------
L3 + R3 (both sticks pressed in) opens the quick menu over the game:
- CHEATS   God Mode, No Police Notoriety, Enemies Ignore You. Kept on
           between sessions (cheats1.ini, cheats2.ini).
- ACTIONS  Clear Notoriety; in Saints Row 2 also Give $10,000 and
           Show / Hide HUD.
- DISPLAY  the FPS counter.
- the DualSense features.
UP / DOWN moves, LEFT / RIGHT or X changes, O (or L3 + R3) closes it.

The app's two programs: eboot.bin (the menu, and Saints Row) and sr2.bin
(Saints Row 2, which the menu starts). Both are needed.

Your data (back these up before deleting or reinstalling the app!)
------------------------------------------------------------------
- PPSA54107/saves/          both games' saves
- PPSA54107/UserData/       trophies (earned and their dates) and the shader caches
- PPSA54107/menu.ini        your settings
SAVES > BACK UP SAVES copies the saves, trophies and settings to
PPSA54107/install/backups/: copy that folder off the PS5. Deleting the
PPSA54107 folder deletes them too.

The first time each area is seen, its shaders are built and saved: the
first play-through can stutter now and then; later sessions load them.

Updating to a new build
-----------------------
Replace eboot.bin, sr2.bin, sce_sys/ and sce_module/ (and hub/ and
payloads/ if they changed). Keep game1/, game2/, dlc1/, dlc2/, install/, saves/,
UserData/ and menu.ini.

Problems
--------
- PLAY stays grey: the HOME and CONTENT pages say what's missing.
- An install failed: the CONTENT page says why (not enough space, not the
  right file, ...). Fix it and press X on that row.
- Something draws wrong: SETTINGS > GPU HELPER off.
- The home screen tile, background or music didn't change after an update:
  restart the PS5 (it caches them).
- A log of every run is written to PPSA54107/logs/. Send the newest one with
  any report.

Credits
-------
Built on elripalda's GTA IV for PS5 (github.com/elripalda/gta4-ps5), saulob's
Crackdown Recompiled (github.com/saulob/crackdown-xbox360-recompiled), OZORDI's
Liberty Recompiled and vaduur's LibertyRecompAndroid, ReXGlue and Xenia.
Hover music made with BlackBearReloaded's PS5 AT9 Converter. Title lettering in
Pirata One (SIL Open Font License).
Special thanks to Volition, for Saints Row.

Saints Row and all related names belong to their owners. This project is not
affiliated with or endorsed by Deep Silver, Volition, THQ, Microsoft or Sony.
