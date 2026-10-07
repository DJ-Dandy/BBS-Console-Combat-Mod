# BBS KH2 Style

Makes *Kingdom Hearts Birth by Sleep Final Mix* (PC, HD 1.5+2.5 ReMIX, Steam) play more like
Kingdom Hearts II. Work in progress.

It is a code mod: one DLL that patches the running game in memory. No game files are changed.

## What it does so far

| | |
|---|---|
| MP | Deck commands cost MP (their old reload time in seconds) instead of reloading. Cure, Cura and Curaga use all remaining MP. |
| MP burn | At zero MP the bar turns pink and refills in 20 seconds; until then no commands and no new D-Link. Attack, items and Shotlocks still work. |
| MP bar | Under the Focus gauge. |
| Ethers | Ether, Mega-Ether, Elixir and Megalixir restore MP as well as Focus. |
| Command menu | Attack / Magic / Item / Link. The confirm button opens a list or uses the chosen entry; d-pad left goes back. (First build, still being adjusted.) |
| Built in | Combo Flow, the KH2-style camera and Revenge Value. Do not enable those three mods separately. |

Still to come: the deck editor split into commands and items, Command Styles as an optional prompt,
and a speed pass over attacks and magic.

## Install with the OpenKH Mod Manager

1. Choose *Birth by Sleep* as the game, then *Mods > Install a new mod > Select and install Mod
   Archive* and pick `BBS-KH2-Style.zip`.
2. Tick the mod; untick Combo Flow, KH2 Camera and Revenge Value if you have them.
3. *Mod Loader > Build Only*, then start the game.

It needs the Mod Loader (Panacea). "Build and Patch" does not work for this mod: a DLL cannot be
patched into the game packages.

## Install without the Mod Manager

Copy `dll\bbskh2.dll` into the game folder (next to `KINGDOM HEARTS Birth by Sleep FINAL MIX.exe`) and
rename it to `dbghelp.dll`; copy `dll\bbskh2.ini` next to it. This cannot be combined with the Mod
Loader, which uses the same file name. The DLL only acts in Birth by Sleep.

## Settings and log

`bbskh2.ini` holds the settings (MP costs, burn time, bar and menu positions, switches for the three
built-in mods and for the menu). A `bbskh2.ini` in the game folder is used if there is one, otherwise
the one next to the DLL. The mod writes `bbskh2_log.txt` in the game folder. An empty file called
`bbskh2_off.txt` in the game folder switches the mod off.
