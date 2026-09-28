// Finding the Rocksmith 2014 folder: from Steam's registry entry and its library list, or from any
// folder the player picks (the game folder itself, a Steam library, or the Steam folder).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace NoteByNoteSetup {

static class GameLocator {
    public const string ExeName = "Rocksmith2014.exe";
    const string GameSubdir = @"steamapps\common\Rocksmith2014";

    public static bool IsGameDir(string dir) =>
        !string.IsNullOrWhiteSpace(dir) && File.Exists(Path.Combine(dir, ExeName));

    // Where Steam is installed (null = not found). HKCU is the usual place; the HKLM one is older.
    public static string SteamDir() {
        foreach (var (hive, key, value) in new[] {
                     (Registry.CurrentUser, @"Software\Valve\Steam", "SteamPath"),
                     (Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                     (Registry.LocalMachine, @"SOFTWARE\Valve\Steam", "InstallPath")}) {
            try {
                using (var k = hive.OpenSubKey(key))
                    if (k?.GetValue(value) is string s && Directory.Exists(s)) return Path.GetFullPath(s);
            } catch { }
        }
        return null;
    }

    // Every Steam library of a Steam install: the Steam folder itself plus the "path" entries of
    // steamapps\libraryfolders.vdf (a text file: "path"  "D:\\SteamLibrary").
    public static List<string> Libraries(string steamDir) {
        var libs = new List<string> { steamDir };
        try {
            var vdf = Path.Combine(steamDir, @"steamapps\libraryfolders.vdf");
            if (File.Exists(vdf))
                foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                    libs.Add(m.Groups[1].Value.Replace(@"\\", @"\"));
        } catch { }
        return libs.Distinct(StringComparer.OrdinalIgnoreCase).ToList();
    }

    // The game folder for what the player typed or picked (null = no game there):
    // the game folder, a folder inside it, a Steam library, or the Steam folder.
    public static string Resolve(string dir) {
        if (string.IsNullOrWhiteSpace(dir)) return null;
        try { dir = Path.GetFullPath(dir.Trim().Trim('"')); } catch { return null; }
        for (var d = new DirectoryInfo(dir); d != null; d = d.Parent)  // the game folder or inside it
            if (IsGameDir(d.FullName)) return d.FullName;
        if (IsGameDir(Path.Combine(dir, GameSubdir))) return Path.Combine(dir, GameSubdir);  // a library
        if (Directory.Exists(Path.Combine(dir, "steamapps")))  // a Steam folder: look in its libraries
            foreach (var lib in Libraries(dir))
                if (IsGameDir(Path.Combine(lib, GameSubdir))) return Path.Combine(lib, GameSubdir);
        return null;
    }

    // The game folder found by itself (null = not found).
    public static string Find() {
        var steam = SteamDir();
        return steam == null ? null : Resolve(steam);
    }

    // The 32-bit ASIO drivers installed (the game is 32-bit, so only those work), by the name
    // RS_ASIO.ini uses ("Focusrite USB ASIO").
    public static List<string> AsioDrivers() {
        try {
            using (var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32))
            using (var k = hklm.OpenSubKey(@"SOFTWARE\ASIO"))
                return k?.GetSubKeyNames().OrderBy(n => n).ToList() ?? new List<string>();
        } catch { return new List<string>(); }
    }
}

}  // namespace NoteByNoteSetup
