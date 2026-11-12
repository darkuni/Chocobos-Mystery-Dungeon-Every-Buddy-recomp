Chocobo's Mystery Dungeon EVERY BUDDY! for PC - native port of the PS4 game (Windows and Linux / SteamOS)

EARLY TEST BUILD: the game boots to the title screen and takes controller input. How far
it plays past that is what testing is finding out.

No game files are included. You need your own copy of the PS4 game (CUSA14219, US) as a fake PKG.

FIRST START
  1. Put your PKG file in this folder, next to chocobo.exe (Linux: chocobo).
     Update PKGs go next to it too. The Japanese 1.01 update (CUSA14078) works with the US
     game: it replaces the executables and keeps the US game otherwise. Some US dumps have
     blank executables; the game then refuses to start and the log says so. Adding the
     update fixes that.
  2. Run chocobo.exe (Linux: ./chocobo).
  3. The first start unpacks the PKG into the "game" folder here (about 1.5 GB, under a minute)
     and prepares the game once. After that it starts straight away, and the PKG can be
     deleted or moved.
  Already have the game unpacked? A folder named "game" here (with eboot.bin in it) works too.

SETTINGS MENU
  In the game: Insert on the keyboard, or R3 + L2 on a controller. Saved in chocobo.ini.
  (Not F1 as in some other recomps: this one uses the Resogun recomp's runtime.)

CONTROLS
  Any controller works. Keyboard: Space = Cross, Left Shift = Circle, E = Square,
  Enter = Options, I / J / K / L = d-pad.

FILES MADE HERE
  game/          the unpacked game
  out/           the prepared program (made again when the game files change)
  user/          saves, shader caches and last_run.log
  chocobo.ini    settings

STEAM DECK / LINUX
  Unpack the folder anywhere, put the PKG in it, run ./chocobo once from the desktop.
  For Gaming Mode: Steam > Add a Non-Steam Game > browse to chocobo, then in the shortcut's
  Properties > Compatibility choose "Steam Linux Runtime" (not Proton).

PROBLEMS
  Every start writes user/last_run.log. Please attach it to a bug report:
  https://github.com/darkuni/Chocobos-Mystery-Dungeon-Every-Buddy-recomp/issues
