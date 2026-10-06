// crashlog.h: writes the game's fatal errors to NoteByNote.log (ini key CrashLog, on by default).
//
// When the game dies, the log just stops: nothing says where, and a player would have to make it
// happen again with some extra tool to tell us more. So every serious processor exception (access
// violation, illegal instruction...) is logged once, with the module and the offset where it
// happened: the log a player sends after a crash already has it. Many are harmless (the game or
// its protector handles them and goes on); the one that killed the game is the last one before the
// log ends.
//
// Nothing is changed: the handler only looks, and always passes the exception on.
#pragma once

namespace nbn::crashlog {

// Installs the handler (once).
void Start();

// Removes it. Must be called before the DLL is freed (the dev unload): a handler left registered
// points into freed memory, and the game's next exception, even a harmless one, jumps there.
void Stop();

}  // namespace nbn::crashlog
