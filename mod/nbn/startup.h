// startup.h: gets through the game's start-up dialogs by itself (Ubisoft servers are offline).
//
// When the game starts it tries to log in to Ubisoft (Uplay) and shows a chain of dialogs that all
// need a key press, even though the servers are gone:
//
//   title "Press Enter"       <- the player (kept on purpose)
//   UplayLoginDialog          "Do you already have a Uplay account?"      -> Esc (Exit)
//   SelectionListDialog       "Log in to use the online features"         -> Esc (Cancel)
//   ProfileSelect             choose a profile                            <- the player
//   SimpleDialog              the autosave notice (closes by itself)
//   MainMenu                  -> done, never acts again
//
// (Answering Accept on the second dialog instead sends the game back to the login and, later, to
// "Connecting to the Ubisoft server..." + "The Ubisoft servers are not available" after the
// profile; if those appear anyway the helper answers Enter = Accept.)
//
// The helper only knows the screen names (game::GetMenu), not the dialog texts, so it is strict:
// it acts only before the main menu, only on the screens above, and stops for good at the first
// screen it doesn't expect (e.g. creating a new profile) or when the player presses Delete on the
// profile screen (so a "delete this profile?" dialog is never answered for them). Keys are posted
// to the game window (WM_KEYDOWN/WM_KEYUP), so they work even if the window isn't in front.
#pragma once
#include <windows.h>

#include <string>

namespace nbn::startup {

// Call every main-loop iteration. on = the SkipUbisoftPopups setting; menuOk/menu = game::GetMenu;
// window = the game window (nullptr until the overlay found it).
void Tick(bool on, bool menuOk, const std::string& menu, HWND window, DWORD now);

}  // namespace nbn::startup
