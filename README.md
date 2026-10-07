# BBS KH2-Style

A mod for **Kingdom Hearts Birth by Sleep Final Mix** (Steam, HD 1.5+2.5 ReMIX) that makes it play more like
Kingdom Hearts II: a KH2 command menu, MP instead of command reloads, Command Styles as offers, faster pacing and
a KH2-style camera.

It is a single DLL that the OpenKH mod loader starts with the game. It patches the game in memory only; no game
file is changed, and removing the mod puts everything back.

## What it changes

- **Command menu**: Attack / Magic / Items / D-Link, as in KH2. Magic, Items and D-Link open a list of what is in
  your deck, with a header and a colour of its own (Magic blue, Items green, D-Link the D-Link blue). The confirm
  button or d-pad right opens a list; the jump button closes it. After a command is used the cursor is back on
  Attack.
- **MP instead of reloads**: commands cost MP (their old reload time, so Fire 10, Firaga 15 ...). At 0 MP the bar
  recharges (MP charge, 25 seconds). The Magic Haste and Attack Haste abilities are now both **MP Haste**: every copy
  installed makes the charge 5 % faster. Reload Boost is now **Berserker**: 5 % more damage dealt while the bar
  recharges. Cure takes all remaining MP. Ethers and Elixirs restore MP, and starting a D-Link
  refills the bar. KH2-style MP bar under the Focus gauge; a command that would empty the bar has its name in
  yellow.
- **Shortcuts**: hold L1 and the menu turns into four commands on the face buttons, as in KH2. Set them in the
  pause menu under *Command Decks > Shortcuts*.
- **Command Styles and finishers are offers**: a full gauge shows a prompt with a timer above the menu. Press the
  style button (the game's old deck-command button) to take it; Attack stays Attack meanwhile.
- **Reaction prompts**: Talk, Open, Save and the like are answered with triangle and sit above the menu.
- **Pace**: attacks, commands and items play 15 % faster; a tilted stick ends an action as soon as its last hit
  is over; after an aerial attack you fall instead of hanging; spells go off and unlock at KH2's times.
- **Camera**: KH2-style field and battle camera.
- **Revenge values**: humanoid bosses can no longer roll dice to escape on every hit. Each hit builds a hidden
  value (hit 1, magic 1.5, finisher 3, shotlock hit 0.3) and at the boss's limit he breaks out with his own
  counter. Large bosses are untouched.

## Requirements

- Steam version of KINGDOM HEARTS HD 1.5+2.5 ReMIX.
- [OpenKH](https://github.com/OpenKH/OpenKh) Mod Manager set up for the PC version **with the mod loader
  (Panacea)**. The mod is a DLL the mod loader starts together with the game; the Mod Manager's "patch the game
  files" way of installing mods will not load it.

## Install

1. Download [`dist/BBS-KH2-Style.zip`](dist/BBS-KH2-Style.zip).
2. In the Mod Manager choose **Birth by Sleep** as the game.
3. *Mods > Install a New Mod > Select and install Mod Archive*, pick the zip.
4. Tick the mod, then *Mod Loader > Build and Run* (or *Build Only* and start the game yourself).

To remove it, untick it and build again.

## Settings

Everything can be changed or switched off in `bbskh2.ini`, which is installed next to the DLL
(`<OpenKH mod folder>/bbs/dll/`). The file explains each setting; it is read when the game starts, and deleting a
line gives that setting its default back.

| Section | What it holds |
|---|---|
| `[MP]`, `[Cost]`, `[Bar]` | maximum MP, recharge, what commands cost, the MP bar's place, size and look |
| `[Menu]` | the command menu: positions, texts, list headers and colours, button behaviour |
| `[Shortcuts]` | the L1 shortcut list |
| `[Style]` | Command Style and finisher offers |
| `[Speed]` | action speed, walk-out, air behaviour, cast times |
| `[Combat]`, `[Bundle]` | damage floor, and the camera / walk-out / revenge-value parts |
| `[Safety]` | an optional 2D texture guard (off by default) |

Two files appear next to the game's exe while the mod is in use: `bbskh2_shortcuts.ini` (your shortcuts, per
character) and `bbskh2_log.txt` (what the mod did at start-up, and details should the game crash). A file named
`bbskh2_off.txt` there makes the mod load without changing anything.

## Building

The mod is plain C, cross-compiled for Windows with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) or any
x86_64 mingw-w64 compiler:

```sh
CC=/path/to/x86_64-w64-mingw32-clang ./build.sh   # -> out/dinput8.dll
tools/package.sh                                  # -> dist/BBS-KH2-Style.zip
```

It only works with the game build it was written for: at start-up every place it patches is compared with the
bytes it expects, and if anything differs nothing is changed.

## Tests

`test/run.sh` maps your own copy of the game's executable into memory on Linux (x86-64, gcc), applies every patch
and runs the patched game code natively:

```sh
BBS_EXE="/path/to/KINGDOM HEARTS Birth by Sleep FINAL MIX.exe" test/run.sh
```

Some tests also need files extracted from the game (layouts and textures, named by `BBS_...` environment
variables in `test/run.c`); they are skipped when those are not set. No game files are part of this repository.

## Layout

| Path | Contents |
|---|---|
| `src/` | the mod: `menu.c` command menu, `mp.c` MP and the bar, `style.c` offers, `speed.c` pacing, `shortcut.c` / `sccamp.c` shortcuts, `bundle.c` camera and revenge values, `tex.c` added art, `guard.c` 2D safety checks, `core.c` patching |
| `bundle_src/` | sources of the revenge-value boss scripts and the camera / walk-out data patches |
| `tools/` | generators for the art, hooks and package |
| `test/` | the offline tests |
| `docs/` | notes on how the game works and why each part of the mod is built the way it is |
| `modmanager/` | the Mod Manager package's `mod.yml`, readme and images |
| `dist/` | the built package and the default `bbskh2.ini` |

## Credits

Built with [Claude](https://claude.com/claude-code). Loaded by OpenKH's Panacea. Not affiliated with or endorsed
by Square Enix or Disney; Kingdom Hearts is their property, and you need your own copy of the game.
