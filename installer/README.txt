Note-by-Note for Rocksmith 2014
===============================

A free, fan-made mod: the song waits at each note (and chord) until you play it, so you can learn
a part at your own pace. While it waits, it shows which string (by its highway colour) and fret to
play, and a tab of what's coming.

Not affiliated with or endorsed by Ubisoft. Rocksmith is a trademark of Ubisoft Entertainment.


What you need
-------------
- Rocksmith 2014 Edition - Remastered (Steam, PC), the current version.
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

In a song, press F8 for the Note-by-Note menu (the mode: Off, Show the notes or Wait for each note;
the Practice page with the places where you stop most; tab and banner options).
F9 skips the note the song is waiting for. Settings are saved in NoteByNote.ini in the game folder.


Uninstall
---------
Run "NoteByNote_install\Note-by-Note Setup.exe" in the game folder (or this setup again) and press
Uninstall. Every file the setup replaced was backed up, and is put back; files it added are removed.
Tick "also delete my settings and logs" to remove NoteByNote.ini and the log as well.


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
- No sound / the game says no guitar: check RS_ASIO.ini (driver name and input channel). See
  https://github.com/mdias/rs_asio for RS_ASIO's own help.


Credits
-------
- Note-by-Note by arkhanragel (MIT licence)
- RS_ASIO by Micael Dias (MIT licence)
- MinHook by Tsuda Kageyu (BSD 2-clause licence)
- Dear ImGui by Omar Cornut (MIT licence)
- The RSMods project, for its research on the game
Licence texts: LICENSES.txt
