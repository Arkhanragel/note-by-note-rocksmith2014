// Installing and uninstalling Note-by-Note in the game folder, keeping the game folder restorable.
//
// What goes into the game folder:
//   NoteByNote.dll    the mod
//   RS_ASIO.dll       RS_ASIO v0.7.5 + Note-by-Note's guitar tap and loader (it loads NoteByNote.dll)
//   avrt.dll          RS_ASIO's loader (unchanged v0.7.5), so avrt.dll and RS_ASIO.dll always match
//   RS_ASIO.ini       only when RS_ASIO wasn't installed yet (with the ASIO driver the player picked);
//                     then Rocksmith.ini also gets the two settings RS_ASIO asks for
//   NoteByNote_install\   the install record (install.txt), the backups of every file that was
//                     replaced (backup\), this setup program (to uninstall later), README, licenses
//
// Every change is written to install.txt BEFORE it is made, so an interrupted install can still be
// undone. Uninstall walks the record backwards: files that were added are deleted, replaced ones
// get their backup back, changed ini values get their old value back. A file that changed after
// the install (the player updated RS_ASIO, say) is left alone and reported, and its backup kept.
// The mod's own files (NoteByNote.ini settings, NoteByNote.log) are only deleted when asked.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace NoteByNoteSetup {

// The install record, NoteByNote_install\install.txt (tab-separated lines).
class Manifest {
    public class FileEntry {
        public string Kind;  // "added" (wasn't there before) or "replaced" (original in backup\)
        public string Name;  // file name in the game folder
        public string Hash;  // SHA-256 of what we put there ("" = not written yet)
    }
    public class IniEntry {
        public string File, Section, Key;
        public string Old;   // the value before the install; null = the key didn't exist
    }
    public string Version = "";
    public List<FileEntry> Files = new List<FileEntry>();
    public List<IniEntry> Inis = new List<IniEntry>();

    const string Absent = "<absent>";

    public static Manifest Load(string path) {
        if (!File.Exists(path)) return null;
        var m = new Manifest();
        foreach (var line in File.ReadAllLines(path)) {
            var p = line.Split('\t');
            if (p[0] == "version" && p.Length >= 2) m.Version = p[1];
            else if (p[0] == "file" && p.Length >= 4) m.Files.Add(new FileEntry { Kind = p[1], Name = p[2], Hash = p[3] });
            else if (p[0] == "ini" && p.Length >= 5)
                m.Inis.Add(new IniEntry { File = p[1], Section = p[2], Key = p[3], Old = p[4] == Absent ? null : p[4] });
        }
        return m;
    }

    public void Save(string path) {
        var sb = new StringBuilder();
        sb.AppendLine("# Note-by-Note install record: what the setup changed in this folder, so it can be undone.");
        sb.AppendLine("# Used by \"Note-by-Note Setup.exe\" > Uninstall. Please don't edit.");
        sb.AppendLine("version\t" + Version);
        foreach (var f in Files) sb.AppendLine($"file\t{f.Kind}\t{f.Name}\t{f.Hash}");
        foreach (var i in Inis) sb.AppendLine($"ini\t{i.File}\t{i.Section}\t{i.Key}\t{i.Old ?? Absent}");
        File.WriteAllText(path, sb.ToString());
    }

    public FileEntry Find(string name) =>
        Files.FirstOrDefault(f => string.Equals(f.Name, name, StringComparison.OrdinalIgnoreCase));
}

// What's in a game folder right now (for the window's status line and the install choices).
class GameStatus {
    public bool GameFound;
    public bool VersionSupported;  // the exe the mod was made for (else the mod stays switched off)
    public bool VersionModified;   // that same build, but changed on disk (a patched exe): may work
    public bool VersionOlder;      // the older Remastered build (September 2022), not supported yet
    public string ExeInfo;         // the exe's numbers, for a bug report when it isn't supported
    public bool RsAsio;            // RS_ASIO is installed (avrt.dll + RS_ASIO.dll + RS_ASIO.ini)
    public bool RsAsioIni;         // RS_ASIO.ini is there: the install keeps it (and the player's audio device)
    public string RsAsioDriver;    // the ASIO driver in that RS_ASIO.ini (output's, else the guitar input's)
    public string RsAsioVersion;   // its file version, if it has one
    public string Installed;       // installed Note-by-Note version (null = not installed, "" = unknown)
}

class GameInstall {
    public const string InstallDirName = "NoteByNote_install";
    public const string SetupExeName = "Note-by-Note Setup.exe";
    // PE checksum of the Rocksmith2014.exe the mod supports (the same check as mod/nbn/game.cpp).
    const uint SupportedChecksum = 0x0176EC34;
    // Link time in its PE header. A patched copy of that exe keeps this and the CheckSum field of
    // its header, while the checksum computed over the file changes.
    const uint SupportedTimestamp = 0x67497D00;
    // The older build many players keep (RSMods' "RemasteredSeptember2022", same kind of checksum).
    const uint OlderChecksum = 0x00B13D7C;
    static readonly string[] Dlls = { "avrt.dll", "RS_ASIO.dll", "NoteByNote.dll" };

    readonly string game, inst, backup, manifestPath;
    readonly Action<string> log;

    public GameInstall(string gameDir, Action<string> log) {
        game = gameDir;
        inst = Path.Combine(game, InstallDirName);
        backup = Path.Combine(inst, "backup");
        manifestPath = Path.Combine(inst, "install.txt");
        this.log = log;
    }

    public static string AppVersion() {
        var v = Assembly.GetExecutingAssembly().GetName().Version;
        return $"{v.Major}.{v.Minor}.{v.Build}";
    }

    // Tests only (set by reflection): work on a fake game folder while the real game runs.
    static bool ignoreRunningGame = false;

    public static bool GameRunning() => !ignoreRunningGame && Process.GetProcessesByName("Rocksmith2014").Length > 0;

    public GameStatus Status() {
        var st = new GameStatus { GameFound = GameLocator.IsGameDir(game) };
        if (!st.GameFound) return st;
        try {
            var b = File.ReadAllBytes(Path.Combine(game, GameLocator.ExeName));
            uint sum = PeChecksum(b), header = PeHeaderChecksum(b), time = PeTimestamp(b);
            st.VersionSupported = sum == SupportedChecksum;
            st.VersionModified = !st.VersionSupported && header == SupportedChecksum && time == SupportedTimestamp;
            st.VersionOlder = sum == OlderChecksum;
            st.ExeInfo = $"checksum {sum:X8}, header {header:X8}, time {time:X8}, size {b.Length}";
            var build = SteamBuildId();
            if (build != null) st.ExeInfo += ", Steam build " + build;
        } catch (Exception ex) { st.ExeInfo = "could not read the exe: " + ex.Message; }
        var rs = Path.Combine(game, "RS_ASIO.dll");
        var rsIni = Path.Combine(game, "RS_ASIO.ini");
        st.RsAsioIni = File.Exists(rsIni);
        st.RsAsio = File.Exists(rs) && File.Exists(Path.Combine(game, "avrt.dll")) && st.RsAsioIni;
        if (st.RsAsioIni) {
            // Sound can go out through WASAPI instead (EnableWasapiOutputs): then the guitar's driver.
            st.RsAsioDriver = ReadIni(rsIni, "Asio.Output", "Driver");
            if (string.IsNullOrWhiteSpace(st.RsAsioDriver)) st.RsAsioDriver = ReadIni(rsIni, "Asio.Input.0", "Driver");
        }
        if (File.Exists(rs)) {
            var fv = FileVersionInfo.GetVersionInfo(rs).FileVersion;
            st.RsAsioVersion = string.IsNullOrWhiteSpace(fv) ? null : fv.Trim();
        }
        var m = Manifest.Load(manifestPath);
        if (m != null) st.Installed = m.Version;
        else if (File.Exists(Path.Combine(game, "NoteByNote.dll"))) st.Installed = "";  // installed by hand
        return st;
    }

    // ------------------------------------------------------------------ install / update

    // asioDriver / inputChannel (0-based): only used when RS_ASIO isn't installed yet.
    public void Install(string asioDriver, int inputChannel) {
        if (GameRunning()) throw new InvalidOperationException("Close Rocksmith 2014 first (its files are in use).");
        Directory.CreateDirectory(backup);
        var m = Manifest.Load(manifestPath) ?? new Manifest();
        var first = m.Files.Count == 0;
        if (first) AdoptOldBackup(m);
        // RS_ASIO needs setting up (an ini with the audio device) only if it's not there at all.
        var newRsAsio = !File.Exists(Path.Combine(game, "RS_ASIO.ini"));

        foreach (var name in Dlls) PutFile(m, name, Payload(name));

        if (newRsAsio) {
            var ini = Path.Combine(game, "RS_ASIO.ini");
            AddEntry(m, "RS_ASIO.ini");
            File.WriteAllBytes(ini, Payload("RS_ASIO.ini"));
            if (!string.IsNullOrEmpty(asioDriver)) {
                WriteIni(ini, "Asio.Output", "Driver", asioDriver);
                WriteIni(ini, "Asio.Input.0", "Driver", asioDriver);
                WriteIni(ini, "Asio.Input.0", "Channel", inputChannel.ToString());
                log($"RS_ASIO.ini: sound through \"{asioDriver}\", guitar on its input {inputChannel + 1}.");
            } else {
                log("RS_ASIO.ini: no ASIO driver chosen. Put your audio interface's ASIO driver name in it "
                    + "([Asio.Output] and [Asio.Input.0] Driver=) before playing.");
            }
            m.Find("RS_ASIO.ini").Hash = Sha256(ini);
            m.Save(manifestPath);
            // RS_ASIO's own instructions: the game must use exclusive, low-latency audio.
            var rsIni = Path.Combine(game, "Rocksmith.ini");
            if (File.Exists(rsIni)) {
                SetIni(m, "Rocksmith.ini", "Audio", "ExclusiveMode", "1");
                SetIni(m, "Rocksmith.ini", "Audio", "Win32UltraLowLatencyMode", "1");
            } else {
                log("Rocksmith.ini not found (the game creates it on its first start). RS_ASIO needs "
                    + "ExclusiveMode=1 and Win32UltraLowLatencyMode=1 in it.");
            }
        }

        // The setup itself (to uninstall later without the download), the readme and the licenses.
        var self = Assembly.GetExecutingAssembly().Location;
        var selfCopy = Path.Combine(inst, SetupExeName);
        if (!string.Equals(Path.GetFullPath(self), Path.GetFullPath(selfCopy), StringComparison.OrdinalIgnoreCase))
            File.Copy(self, selfCopy, true);
        foreach (var doc in new[] { "README.txt", "LICENSES.txt" })
            if (HasPayload(doc)) File.WriteAllBytes(Path.Combine(inst, doc), Payload(doc));

        m.Version = AppVersion();
        m.Save(manifestPath);
        log($"Note-by-Note {m.Version} is installed. Start the game as usual; press F8 in a song for its menu.");
        log($"To uninstall: run \"{Path.Combine(InstallDirName, SetupExeName)}\" in the game folder.");
    }

    // Puts one of our files in the game folder, recording it first (and backing up what was there).
    void PutFile(Manifest m, string name, byte[] data) {
        var target = Path.Combine(game, name);
        var e = m.Find(name);
        if (e == null) {
            // A NoteByNote.dll already there is an older copy of the mod (installed by hand): not
            // something to put back on uninstall.
            if (File.Exists(target) && name != "NoteByNote.dll") {
                var copy = Path.Combine(backup, name);
                if (!File.Exists(copy)) File.Copy(target, copy);
                e = new Manifest.FileEntry { Kind = "replaced", Name = name, Hash = "" };
                log($"{name}: the existing one is saved in {InstallDirName}\\backup.");
            } else {
                e = new Manifest.FileEntry { Kind = "added", Name = name, Hash = "" };
            }
            m.Files.Add(e);
            m.Save(manifestPath);
        }
        File.WriteAllBytes(target, data);
        e.Hash = Sha256(target);
        m.Save(manifestPath);
        log($"{name}: installed.");
    }

    void AddEntry(Manifest m, string name) {
        if (m.Find(name) != null) return;
        m.Files.Add(new Manifest.FileEntry { Kind = "added", Name = name, Hash = "" });
        m.Save(manifestPath);
    }

    // An install made by hand before this setup existed (the development script kept the original
    // RS_ASIO.dll as RS_ASIO.dll.original): take that as the backup of RS_ASIO.dll.
    void AdoptOldBackup(Manifest m) {
        var old = Path.Combine(game, "RS_ASIO.dll.original");
        var copy = Path.Combine(backup, "RS_ASIO.dll");
        if (!File.Exists(old) || File.Exists(copy)) return;
        File.Move(old, copy);
        m.Files.Add(new Manifest.FileEntry { Kind = "replaced", Name = "RS_ASIO.dll", Hash = "" });
        m.Save(manifestPath);
        log("Found an earlier Note-by-Note install: its saved original RS_ASIO.dll is now the backup.");
    }

    // Sets an ini value, recording the old one first (only the first time: a reinstall keeps the
    // value from before the very first install).
    void SetIni(Manifest m, string file, string section, string key, string value) {
        var path = Path.Combine(game, file);
        var old = ReadIni(path, section, key);
        if (old == value) return;
        if (!m.Inis.Any(i => i.File == file && i.Section == section && i.Key == key)) {
            m.Inis.Add(new Manifest.IniEntry { File = file, Section = section, Key = key, Old = old });
            m.Save(manifestPath);
        }
        WriteIni(path, section, key, value);
        log($"{file}: {key}={value} (was {old ?? "not set"}).");
    }

    // ------------------------------------------------------------------ uninstall

    // Returns true when everything was put back (false = something was left, see the log).
    public bool Uninstall(bool removeUserData) {
        if (GameRunning()) throw new InvalidOperationException("Close Rocksmith 2014 first (its files are in use).");
        var m = Manifest.Load(manifestPath);
        var clean = true;
        if (m == null) {
            clean = UninstallOld();
        } else {
            for (var i = m.Files.Count - 1; i >= 0; --i) clean &= Undo(m.Files[i]);
            for (var i = m.Inis.Count - 1; i >= 0; --i) {
                var e = m.Inis[i];
                var path = Path.Combine(game, e.File);
                if (!File.Exists(path)) continue;
                WriteIni(path, e.Section, e.Key, e.Old);  // null = remove the key
                log($"{e.File}: {e.Key} back to {e.Old ?? "not set"}.");
            }
        }
        if (removeUserData) RemoveUserData();
        else log("Your Note-by-Note settings (NoteByNote.ini) were kept.");

        if (clean) RemoveInstallDir();
        else log($"Some files were kept (see above); {InstallDirName} still has their backups.");
        log(clean ? "Note-by-Note is uninstalled. The game folder is as it was before." : "Uninstall finished, with notes above.");
        return clean;
    }

    bool Undo(Manifest.FileEntry e) {
        var target = Path.Combine(game, e.Name);
        // Changed since we put it there (e.g. the player updated RS_ASIO): not ours to remove.
        // RS_ASIO.ini is meant to be edited by the player, and any NoteByNote.dll is ours (a newer
        // build copied over by hand, say), so those two don't count.
        var ours = e.Name == "RS_ASIO.ini" || e.Name == "NoteByNote.dll";
        var changed = !ours && File.Exists(target) && e.Hash != "" && Sha256(target) != e.Hash;
        if (changed) {
            log($"{e.Name}: changed after Note-by-Note was installed, so it was left as it is"
                + (e.Kind == "replaced" ? $" (the original is in {InstallDirName}\\backup)." : "."));
            return false;
        }
        if (e.Kind == "added") {
            if (File.Exists(target)) File.Delete(target);
            log($"{e.Name}: removed.");
        } else {
            var copy = Path.Combine(backup, e.Name);
            if (!File.Exists(copy)) {
                log($"{e.Name}: its backup is missing, left as it is.");
                return false;
            }
            File.Copy(copy, target, true);
            log($"{e.Name}: the original is back.");
        }
        return true;
    }

    // No install record: an install made by hand / with the development script.
    bool UninstallOld() {
        var old = Path.Combine(game, "RS_ASIO.dll.original");
        if (File.Exists(old)) {
            File.Copy(old, Path.Combine(game, "RS_ASIO.dll"), true);
            File.Delete(old);
            log("RS_ASIO.dll: the original is back.");
        } else if (File.Exists(Path.Combine(game, "RS_ASIO.dll"))) {
            log("RS_ASIO.dll: no original saved, left as it is (Note-by-Note won't load without NoteByNote.dll).");
        }
        var mod = Path.Combine(game, "NoteByNote.dll");
        if (File.Exists(mod)) { File.Delete(mod); log("NoteByNote.dll: removed."); }
        return true;
    }

    // Everything the mod creates by itself while running (settings, log, debug recordings, old copies).
    void RemoveUserData() {
        foreach (var f in new[] { "NoteByNote.ini", "NoteByNote.log", "NoteByNote.unload" })
            TryDelete(Path.Combine(game, f));
        foreach (var f in Directory.GetFiles(game, "NoteByNote.dll.old_*")) TryDelete(f);
        foreach (var d in new[] { "NoteByNote_debug", "NoteByNote_charts" }) {
            var p = Path.Combine(game, d);
            if (Directory.Exists(p)) try { Directory.Delete(p, true); } catch (Exception ex) { log($"{d}: {ex.Message}"); }
        }
        log("Your Note-by-Note settings and logs were deleted.");
    }

    // Deletes NoteByNote_install. If this program runs from there, it can't delete itself while
    // running: a hidden command does it two seconds after the program closes.
    void RemoveInstallDir() {
        var self = Path.GetFullPath(Assembly.GetExecutingAssembly().Location);
        if (self.StartsWith(Path.GetFullPath(inst) + "\\", StringComparison.OrdinalIgnoreCase)) {
            foreach (var f in Directory.GetFiles(inst, "*", SearchOption.AllDirectories))
                if (!string.Equals(Path.GetFullPath(f), self, StringComparison.OrdinalIgnoreCase)) TryDelete(f);
            Process.Start(new ProcessStartInfo("cmd.exe", $"/c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"{inst}\"") {
                CreateNoWindow = true, UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden });
        } else if (Directory.Exists(inst)) {
            Directory.Delete(inst, true);
        }
    }

    void TryDelete(string path) {
        try { if (File.Exists(path)) File.Delete(path); } catch (Exception ex) { log($"{Path.GetFileName(path)}: {ex.Message}"); }
    }

    // ------------------------------------------------------------------ helpers

    // The embedded files (see NoteByNoteSetup.csproj).
    static bool HasPayload(string name) =>
        Assembly.GetExecutingAssembly().GetManifestResourceInfo("payload/" + name) != null;

    static byte[] Payload(string name) {
        using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload/" + name)) {
            if (s == null) throw new FileNotFoundException("This setup is incomplete: " + name + " is missing inside it.");
            using (var ms = new MemoryStream()) { s.CopyTo(ms); return ms.ToArray(); }
        }
    }

    static string Sha256(string path) {
        using (var sha = SHA256.Create())
        using (var f = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
    }

    // The PE checksum Windows' MapFileAndCheckSum computes: the file as 16-bit words, added with
    // end-around carry, skipping the header's own CheckSum field, plus the file length.
    static uint PeChecksum(byte[] b) {
        var field = BitConverter.ToInt32(b, 0x3C) + 24 + 64;  // e_lfanew -> optional header -> CheckSum
        ulong sum = 0;
        for (var i = 0; i < b.Length; i += 2) {
            if (i == field || i == field + 2) continue;
            sum += i + 1 < b.Length ? (uint)(b[i] | b[i + 1] << 8) : b[i];
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        sum = (sum & 0xFFFF) + (sum >> 16);
        return (uint)sum + (uint)b.Length;
    }

    // The CheckSum field written in the header (by the linker; not updated when the file is patched).
    static uint PeHeaderChecksum(byte[] b) => BitConverter.ToUInt32(b, BitConverter.ToInt32(b, 0x3C) + 24 + 64);

    // The link time in the file header (e_lfanew -> PE signature -> Machine, NumberOfSections, TimeDateStamp).
    static uint PeTimestamp(byte[] b) => BitConverter.ToUInt32(b, BitConverter.ToInt32(b, 0x3C) + 8);

    // Steam's build number of the installed game, from steamapps\appmanifest_221680.acf next to
    // steamapps\common\Rocksmith2014 (null = not a Steam folder layout or not readable).
    string SteamBuildId() {
        try {
            var acf = Path.GetFullPath(Path.Combine(game, @"..\..\appmanifest_221680.acf"));
            if (!File.Exists(acf)) return null;
            var mm = System.Text.RegularExpressions.Regex.Match(File.ReadAllText(acf), @"""buildid""\s+""(\d+)""");
            return mm.Success ? mm.Groups[1].Value : null;
        } catch { return null; }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetPrivateProfileString(string section, string key, string def, StringBuilder buf, uint size, string file);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern bool WritePrivateProfileString(string section, string key, string value, string file);

    // null = the key isn't there.
    static string ReadIni(string file, string section, string key) {
        const string none = "\u0001none";
        var sb = new StringBuilder(512);
        GetPrivateProfileString(section, key, none, sb, 512, file);
        return sb.ToString() == none ? null : sb.ToString();
    }

    static void WriteIni(string file, string section, string key, string value) {
        if (!WritePrivateProfileString(section, key, value, file))
            throw new IOException($"Could not write {key} to {Path.GetFileName(file)}.");
    }
}

}  // namespace NoteByNoteSetup
