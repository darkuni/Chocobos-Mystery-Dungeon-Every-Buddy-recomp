RESOGUN for PC - native port of the PS4 game (Windows and Linux / SteamOS)

No game files are included. You need your own RESOGUN for PS4 (CUSA00038) as fake PKGs:
the game, and the 1.10 update if you have it.

FIRST START
  1. Put your PKG files in this folder, next to resogun.exe (Linux: resogun).
  2. Run resogun.exe (Linux: ./resogun).
  3. The first start unpacks the PKGs into the "game" folder here (about 1 GB, under a minute)
     and prepares the game once. After that it starts straight away, and the PKGs can be
     deleted or moved.
  Already have the game unpacked? A folder named "game" here (with eboot.bin in it) works too.

SETTINGS
  In the game: Insert on the keyboard, or R3 + L2 on a controller. Saved in resogun.ini.

CONTROLS
  Any controller works. Keyboard: Space = Cross, Left Shift = Circle, E = Square,
  Enter = Options, I / J / K / L = d-pad.

FILES MADE HERE
  game/          the unpacked game
  out/           the prepared program (made again when the game files change)
  user/          saves, shader caches and last_run.log
  resogun.ini    settings

STEAM DECK / LINUX
  Unpack the folder anywhere, put the PKGs in it, run ./resogun once from the desktop.
  For Gaming Mode: Steam > Add a Non-Steam Game > browse to resogun, then in the shortcut's
  Properties > Compatibility choose "Steam Linux Runtime" (not Proton).

PROBLEMS
  Every start writes user/last_run.log. Please attach it to a bug report:
  https://github.com/darkuni/resogun-recomp/issues
