Note-by-Note for Rocksmith 2014
===============================

A free, fan-made mod: the song waits at each note (and chord) until you play it, so you can learn
a part at your own pace. While it waits, it shows which string (by its highway colour) and fret to
play, and a tab of what's coming.

Not affiliated with or endorsed by Ubisoft. Rocksmith is a trademark of Ubisoft Entertainment.


What you need
-------------
- Rocksmith 2014 Edition - Remastered (Steam, PC): the current version ("Learn & Play"), or the
  older one of September 2022. The setup says which one it found.
- An audio interface with an ASIO driver (or ASIO4ALL). Note-by-Note hears your guitar through
  RS_ASIO, which the setup installs if you don't have it yet.


Install
-------
1. Close Rocksmith.
2. Run "Note-by-Note Setup.exe" (it asks for administrator rights: the game folder is usually under
   Program Files).
3. It finds the game by itself; if not, pick the Rocksmith 2014 folder or your Steam folder.
4. If RS_ASIO isn't installed yet, choose your audio interface's ASIO driver and the input your
   guitar is plugged into.
5. Press Install. Start the game as usual.

In a song, press F5 for the Note-by-Note menu (the mode: Off, Show the notes or Wait for each note;
the Practice page with the places where you stop most; tab and banner options).
F6 skips the note the song is waiting for. Settings are saved in NoteByNote.ini in the game folder.
A new install starts simple (the note to play and a plain tab): the next notes, fingers and hand
position, pick strokes and the rhythm under the tab are switched on in that menu.

"Note-by-Note Guide.html" (next to this file; opens in your web browser) explains everything on
screen and every option, with pictures.


Uninstall
---------
Run "NoteByNote_install\Note-by-Note Setup.exe" in the game folder (or this setup again) and press
Uninstall. Every file the setup replaced was backed up, and is put back; files it added are removed.
Tick "also delete my settings, records and logs" to remove everything the mod itself created as
well: NoteByNote.ini, the log and report, your practice records (NoteByNote_stats) and the debug
recordings. Without the tick those stay, so a later install finds your settings and records again.


What the setup changes in the game folder
-----------------------------------------
- NoteByNote.dll        added (the mod)
- RS_ASIO.dll, avrt.dll replaced by RS_ASIO v0.7.5 with Note-by-Note's guitar input and loader
                        (your existing ones are backed up), or added if you had no RS_ASIO
- RS_ASIO.ini           only if you had no RS_ASIO: added, with the driver you chose
- Rocksmith.ini         only if you had no RS_ASIO: ExclusiveMode=1 and Win32UltraLowLatencyMode=1
                        (as RS_ASIO requires; the old values are put back on uninstall)
- NoteByNote_install\   the backups, the install record and a copy of this setup
Nothing else in the game is modified on disk; the mod works in memory while the game runs.


Troubleshooting
---------------
- Nothing appears in the game: look at NoteByNote.log in the game folder. "UNSUPPORTED game
  version" means your Rocksmith2014.exe is a version the mod doesn't know; it stays switched off.
- No sound, the game says no guitar, or a song closes by itself after a few seconds: check
  RS_ASIO.ini (driver name and input channel): it must name the ASIO driver of the interface
  your guitar is plugged into. See https://github.com/mdias/rs_asio for RS_ASIO's own help.
- Anything else, also a crash: please report it. Copy NoteByNote.log and NoteByNote_report.txt
  from the game folder right after the problem (they are written again at the next start) and
  attach them to an issue on the project's page, with a few words on what happened:
  https://github.com/Arkhanragel/note-by-note-rocksmith2014/issues


Supporting the project
----------------------
Note-by-Note is free and always will be. Reports of what you find and ideas help most, on the
project's page: https://github.com/Arkhanragel/note-by-note-rocksmith2014
If it helped you and you'd like to say thanks, there is a tip page, entirely optional:
https://ko-fi.com/arkhanragel


Credits
-------
- Note-by-Note by arkhanragel (GNU General Public License, version 3 or later: free software;
  its source code: https://github.com/Arkhanragel/note-by-note-rocksmith2014)
- RS_ASIO by Micael Dias (MIT licence)
- MinHook by Tsuda Kageyu (BSD 2-clause licence)
- Dear ImGui by Omar Cornut (MIT licence)
- The RSMods project, for its research on the game
Licence texts: LICENSES.txt
