# Note-by-Note for Rocksmith 2014

A free, fan-made mod for **Rocksmith 2014 Edition - Remastered** (PC, Steam): the song **waits at each note and chord until you play it**, so you can learn a part at your own pace instead of chasing the highway.

> **Status: early preview.** It works on the current Steam version of the game, and it's being tested by one player. Expect rough edges, and please report what you find.
>
> Not affiliated with or endorsed by Ubisoft. Rocksmith is a trademark of Ubisoft Entertainment. This project contains no game files or song data.

## What it does

- **Waits for you.** When a note reaches the strike line and you haven't played it, the song pauses (music and highway together) until you play the right note. Notes played on time don't stop anything.
- **Tells you what to play** while it waits: the string (by its highway colour) and fret, a small tab, and for chords the name and shape. After a wrong note it says how to fix it ("move up 2 frets", "that's the orange string").
- **Shows the music coming up** as guitar tab, with bar lines, beats and rhythm (stems and beams). It can scroll, turn pages, or use two rows (the next page waits in the second row).
- **Follows the game.** It uses the difficulty (Dynamic Difficulty) you are playing at. Tested mostly with guitar in Learn a Song; Riff Repeater and bass are supported, but less tested.
- **In-game menu (F8):** switch the mode on/off, wait for chords too or only single notes, timing, and tab and display options. You can drag and resize everything with the mouse while the menu is open. F9 skips the note it's waiting for.
- Extras: optionally closes the Ubisoft login popups at start-up, plays the start-up logos faster, and works around one of the game's own random crashes.

## Requirements

- Rocksmith 2014 Edition - Remastered, current Steam version (the mod checks the game version and stays off on others).
- An audio interface with an **ASIO** driver (or ASIO4ALL). Note-by-Note hears your guitar through [RS_ASIO](https://github.com/mdias/rs_asio); the setup installs a build of it that includes Note-by-Note's guitar input.

## Install / uninstall

1. Download the latest `NoteByNote-<version>.zip` from [Releases](../../releases) and unzip it.
2. Close Rocksmith, run **`Note-by-Note Setup.exe`**, check the game folder it found and press **Install**.
3. Start the game as usual. In a song, press **F8** for the menu.

The setup backs up every game-folder file it replaces, and **Uninstall** puts them back exactly as they were (a copy of the setup stays in `<game>\NoteByNote_install\`). Settings live in `NoteByNote.ini` in the game folder.

## How it works (short version)

- **Hearing the guitar:** a small patch to RS_ASIO copies the raw guitar input to shared memory ([mod/rs_asio_tap](mod/rs_asio_tap)). The mod runs its own pitch and onset detection for single notes and a spectral matcher for chords ([mod/nbn/detector.cpp](mod/nbn/detector.cpp)). The game's own scoring isn't used.
- **Knowing what to play:** the chart is read from the game's memory while the song plays, at the difficulty level the game is showing ([mod/nbn/game.cpp](mod/nbn/game.cpp), [mod/nbn/chart.cpp](mod/nbn/chart.cpp)).
- **Waiting:** the song is paused by calling the game's audio engine (Wwise) to pause the music event, and holding the song clock. Nothing is seeked, so music and highway stay in sync. The game's code is never modified: it's protected, and the mod only reads and writes data and calls existing functions.
- **Drawing:** a Direct3D 9 hook with [Dear ImGui](https://github.com/ocornut/imgui) draws the banner, tab, clock and menu ([mod/nbn/overlay.cpp](mod/nbn/overlay.cpp)).

The development log with all the reverse-engineering notes is in [BITACORA.md](BITACORA.md).

## Building from source

Windows, Visual Studio 2022 (C++ desktop, Win32 toolset), CMake, .NET SDK (for the setup and tools), Git with submodules:

```powershell
git clone --recurse-submodules <this repo>
.\mod\build.ps1            # NoteByNote.dll + tests (mod\build\Release)
.\mod\build_rs_asio.ps1    # RS_ASIO v0.7.5 + guitar tap, and avrt.dll (mod\build\rs_asio)
.\package.ps1 -Version 0.1.0 -SkipBuild   # dist\NoteByNote-0.1.0.zip (setup + README + licences)
```

Everything is 32-bit (the game is a 32-bit program). Layout:

| Folder | What |
|---|---|
| `mod/nbn` | the mod (NoteByNote.dll): main loop, wait logic, detector, chart reader, overlay |
| `mod/rs_asio_tap` | the RS_ASIO patch (guitar tap + loader for NoteByNote.dll) |
| `mod/probe`, `mod/injector` | development tools: a probe DLL for exploring game memory, a DLL injector |
| `installer` | the setup program (C#, .NET Framework 4.8) |
| `tools/detector` | Python versions of the detectors, with test and simulation tools |
| `tools/ChartDump` | reads charts from the player's own song files (development/testing) |
| `external` | submodules: RS_ASIO, MinHook, Dear ImGui, Rocksmith2014.NET |

## Plans

Left-handed tab options, UI skins, capo-free play together with pitch-shifting mods, working alongside (or inside) [RSMods](https://github.com/Lovrom8/RSMods), and an audio-to-CDLC tool. See the "Feature backlog" in [BITACORA.md](BITACORA.md).

## Credits

- [RS_ASIO](https://github.com/mdias/rs_asio) by Micael Dias (MIT)
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD 2-clause)
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT)
- [Rocksmith2014.NET](https://github.com/iminashi/Rocksmith2014.NET) by Tapio Malmberg (MIT), used by the development tools
- The [RSMods](https://github.com/Lovrom8/RSMods) project, for its research on the game

## License

[MIT](LICENSE) © 2026 arkhanragel. Please don't share game files, song data or charts made from them.
