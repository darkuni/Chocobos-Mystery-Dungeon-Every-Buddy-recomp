Chocobo's Mystery Dungeon EVERY BUDDY! (PS4) as a native program for Windows and Linux / SteamOS. No game files are included: bring your own fake PKG of the US game (CUSA14219). The Japanese 1.01 update (CUSA14078) also works with it.

**Early test build.** The game boots to the title screen and takes controller input. How far it plays past that is what this build is for.

## Downloads

- **Windows:** `chocobo-win64.zip`
- **Linux / Steam Deck:** `chocobo-linux-x86_64.tar.gz`

## How to play

1. Unpack the download into a folder.
2. Put your PKG files next to `chocobo.exe` (Linux: `chocobo`): the game, and the JP 1.01 update if you have it. Some US dumps have blank executables. The game then refuses to start and the log says so, and adding the update fixes it.
3. Run it. The first start unpacks the PKGs into `game` and prepares the game once (under a minute). Every start after that goes straight into the game.

- **Controls:** any controller. Keyboard: Space = Cross, Left Shift = Circle, E = Square, Enter = Options, I / J / K / L = d-pad.
- **Settings menu:** in game, press **Insert** on the keyboard or **R3 + L2** on a controller (not F1 as in some other recomps).
- **Steam Deck Gaming Mode:** add `chocobo` as a Non-Steam Game and set Compatibility to "Steam Linux Runtime" (not Proton).

## Reporting problems

Every start writes `user/last_run.log`. When something goes wrong, please open an issue that says how far you got and what happened, and attach that log.
