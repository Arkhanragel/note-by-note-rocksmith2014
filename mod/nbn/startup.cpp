// startup.cpp: see startup.h.
#include "startup.h"

#include "log.h"

namespace nbn::startup {
namespace {

bool g_done = false;          // finished (main menu reached, or stopped): never acts again
bool g_profileSeen = false;   // the player has been on the profile screen
std::string g_screen;         // the screen we are looking at...
DWORD g_nextKey = 0;          // ...the earliest tick for the next key on it...
int g_keys = 0;               // ...and the keys sent on it so far
int g_uplayVisits = 0;        // times the Uplay login appeared (if it keeps coming back, give up)
DWORD g_firstDialog = 0;      // when the first dialog appeared (gives up 3 minutes later)

// A dialog ignores keys while it slides in, so wait a bit after it appears and repeat the key while
// it stays open. After the profile (where nothing is expected) wait longer: the short "Loading
// profile..." message must never get a key.
constexpr DWORD kSettleMs = 700;
constexpr DWORD kSettleAfterProfileMs = 2500;
constexpr DWORD kRepeatMs = 1000;

void Stop(const char* why) {
    g_done = true;
    Log("startup: %s", why);
}

void SendKey(HWND w, WPARAM vk, const char* name) {
    // lParam: repeat count 1; key up also has the "previous state" and "transition" bits.
    PostMessageW(w, WM_KEYDOWN, vk, 1);
    Sleep(30);
    PostMessageW(w, WM_KEYUP, vk, 0xC0000001);
    ++g_keys;
    Log("startup: %s on %s%s", name, g_screen.c_str(), g_profileSeen ? " (after the profile)" : "");
}

}  // namespace

void Tick(bool on, bool menuOk, const std::string& menu, HWND window, DWORD now) {
    if (g_done || !menuOk) return;   // before the first dialog the screen name can't be read yet
    if (!on) { Stop("SkipUbisoftPopups is off"); return; }

    const bool uplay = menu == "UplayLoginDialog", list = menu == "SelectionListDialog",
               profile = menu == "ProfileSelect", simple = menu == "SimpleDialog";
    if (menu != g_screen) {
        g_screen = menu;
        g_nextKey = now + (g_profileSeen ? kSettleAfterProfileMs : kSettleMs);
        g_keys = 0;
        if (!g_firstDialog) g_firstDialog = now;
        if (uplay && ++g_uplayVisits > 3) { Stop("the Uplay login keeps coming back, stopping"); return; }
    }

    if (menu == "MainMenu") { Stop("main menu reached, done"); return; }
    if (now - g_firstDialog > 180000) { Stop("still not in the main menu after 3 minutes, giving up"); return; }
    if (!uplay && !list && !profile && !simple) {
        std::string why = "unexpected screen \"" + menu + "\", stopping (the player takes over)";
        Stop(why.c_str());
        return;
    }

    if (profile) {
        g_profileSeen = true;
        // Delete on the profile screen = "delete this profile?" comes next: never answer that one.
        if (GetForegroundWindow() == window && (GetAsyncKeyState(VK_DELETE) & 0x8000))
            Stop("Delete pressed on the profile screen, stopping");
        return;
    }
    if (simple || !window || now < g_nextKey) return;  // the autosave notice closes by itself
    g_nextKey = now + kRepeatMs;
    if (g_keys >= 5) return;  // this dialog doesn't react: leave it to the player

    if (!g_profileSeen) {
        // "Do you already have a Uplay account?" -> Esc = Exit; then "Log in to use the online
        // features of this title" -> Esc = Cancel (Enter = Accept would go back to the login). After
        // that the game goes to the profiles and never tries the servers.
        if (uplay || list) SendKey(window, VK_ESCAPE, "Esc");
    } else if (list) {
        // Not expected after the Esc route, but seen when the login was accepted: "Connecting to the
        // Ubisoft server..." (nothing to press) turns into "The Ubisoft servers are not available"
        // -> Enter = Accept.
        SendKey(window, VK_RETURN, "Enter");
    }
}

}  // namespace nbn::startup
