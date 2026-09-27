// ChartDump: Phase 1 tool for the Note-by-Note mod.
//
// A Rocksmith 2014 song is a .psarc archive (a zlib-compressed container with an
// AES-encrypted table of contents). Inside it, every arrangement (Lead, Rhythm, Bass...)
// has an encrypted binary chart: songs/bin/generic/<name>_<arrangement>.sng.
// Rocksmith2014.NET does all the decryption and parsing. This program only:
//   1. lists the arrangements in a .psarc
//   2. takes one arrangement, rebuilds the full-difficulty note list, and groups notes into
//      "targets" (things the player has to play at one moment: one note or one chord)
//   3. writes those targets to JSON and prints a short summary
//
// Usage:
//   ChartDump list  <song.psarc>
//   ChartDump files <any.psarc>       -> every file name in the archive
//   ChartDump dump  <song.psarc> <sng-name-or-substring> [out.json]
//   ChartDump scan  <folder>          -> one summary line per arrangement, to find easy test songs

using System.Text.Json;
using Microsoft.FSharp.Control;
using Rocksmith2014.Common;
using Rocksmith2014.PSARC;
using Rocksmith2014.SNG;

static class Program
{
    // MIDI numbers of the open strings in standard E tuning, low string first.
    // The SNG stores tuning as semitone offsets from these (e.g. Drop D = [-2,0,0,0,0,0]).
    static readonly int[] GuitarOpenMidi = { 40, 45, 50, 55, 59, 64 }; // E2 A2 D3 G3 B3 E4
    static readonly int[] BassOpenMidi = { 28, 33, 38, 43, 47, 52 };   // E1 A1 D2 G2 (+ unused)
    static readonly string[] NoteNames = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("Usage: ChartDump list <psarc> | dump <psarc> <sng> [out.json] | scan <folder>");
            return 1;
        }

        switch (args[0])
        {
            case "list":
                foreach (var name in ListSngEntries(args[1])) Console.WriteLine(name);
                return 0;

            case "files": // files <psarc>: every entry in the archive (not only charts), e.g. video.psarc
                using (var psarc = OpenPsarc(args[1]))
                    foreach (var name in psarc.Manifest) Console.WriteLine(name);
                return 0;

            case "dump":
                var chart = LoadChart(args[1], args[2]);
                PrintSummary(chart);
                var outPath = args.Length > 3 ? args[3] : Path.GetFileNameWithoutExtension(chart.SngName) + ".json";
                File.WriteAllText(outPath, JsonSerializer.Serialize(chart, new JsonSerializerOptions { WriteIndented = true }));
                Console.WriteLine($"Written: {Path.GetFullPath(outPath)}");
                return 0;

            case "cat": // print a text file from the archive, e.g. a manifest .json with the song title
                using (var psarc = OpenPsarc(args[1]))
                {
                    var name = psarc.Manifest.First(n => n.Contains(args[2], StringComparison.OrdinalIgnoreCase));
                    using var s = psarc.GetEntryStream(name).GetAwaiter().GetResult();
                    s.Position = 0;
                    Console.WriteLine(new StreamReader(s).ReadToEnd());
                }
                return 0;

            case "scan":
                Scan(args[1]);
                return 0;

            case "levels": // levels <psarc> <sng>: phrases, phrase iterations and notes per difficulty level
                using (var psarc = OpenPsarc(args[1]))
                {
                    var name = psarc.Manifest.First(n => n.EndsWith(".sng") && n.Contains(args[2], StringComparison.OrdinalIgnoreCase));
                    var sng = ReadSng(psarc, name);
                    Console.WriteLine($"{name}: {sng.Phrases.Length} phrases, {sng.PhraseIterations.Length} phrase iterations, {sng.Levels.Length} levels");
                    for (int i = 0; i < sng.Phrases.Length; i++)
                        Console.WriteLine($"  phrase {i}: \"{sng.Phrases[i].Name}\" maxDifficulty={sng.Phrases[i].MaxDifficulty} iterations={sng.Phrases[i].IterationCount}");
                    for (int i = 0; i < sng.PhraseIterations.Length; i++)
                    {
                        var pi = sng.PhraseIterations[i];
                        Console.WriteLine($"  PI {i}: phrase {pi.PhraseId} {pi.StartTime:F3}-{pi.EndTime:F3} heroLevels=[{string.Join(",", pi.Difficulty)}]");
                    }
                    foreach (var lv in sng.Levels)
                    {
                        var first = lv.Notes.Take(8).Select(n => $"{n.Time:F2}:s{n.StringIndex}f{n.Fret}(pi{n.PhraseIterationId})");
                        Console.WriteLine($"  level {lv.Difficulty}: {lv.Notes.Length} notes; first: {string.Join(" ", first)}");
                    }
                }
                return 0;

            case "export": // export <psarc-or-folder> [more...] <outdir>
                Export(args.Skip(1).Take(args.Length - 2).ToArray(), args[^1]);
                return 0;
        }
        return 1;
    }

    // ---------------------------------------------------------------- PSARC access

    // Opens a .psarc for reading while allowing others to read/write it too. The game keeps songs.psarc
    // and etudes.psarc open while it runs, and PSARC.OpenFile would fail with a sharing violation.
    static PSARC OpenPsarc(string path) =>
        PSARC.Read(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));

    static List<string> ListSngEntries(string psarcPath)
    {
        using var psarc = OpenPsarc(psarcPath);
        // The manifest is the list of file names in the archive.
        return psarc.Manifest.Where(n => n.EndsWith(".sng", StringComparison.OrdinalIgnoreCase)).ToList();
    }

    static SNG ReadSng(PSARC psarc, string entryName)
    {
        // GetEntryStream returns a Task<MemoryStream> with the decompressed file.
        using var stream = psarc.GetEntryStream(entryName).GetAwaiter().GetResult();
        stream.Position = 0;
        // SNG.fromStream is an F# Async. It decrypts (AES-CTR, PC key) and parses the chart.
        // F# compiles module "SNG" as class "SNGModule" because a type SNG also exists.
        return FSharpAsync.StartAsTask(SNGModule.fromStream(stream, Platform.PC), null, null).GetAwaiter().GetResult();
    }

    // ---------------------------------------------------------------- Chart building

    record NoteOut(int String, int Fret, int Midi, string Name, float Sustain, List<string> Techniques);
    record Target(int Index, double Time, bool IsChord, string? ChordName, List<NoteOut> Notes);
    record Chart(string Psarc, string SngName, bool IsBass, int[] Tuning, int Capo, double SongLength,
                 int SingleNoteTargets, int ChordTargets, List<Target> Targets);

    static Chart LoadChart(string psarcPath, string sngFilter)
    {
        using var psarc = OpenPsarc(psarcPath);
        var sngName = psarc.Manifest.FirstOrDefault(n => n.EndsWith(".sng") && n.Contains(sngFilter, StringComparison.OrdinalIgnoreCase))
                      ?? throw new ArgumentException($"No .sng matching '{sngFilter}'. Use 'list' to see the names.");
        return BuildChart(psarcPath, sngName, ReadSng(psarc, sngName));
    }

    static Chart BuildChart(string psarcPath, string sngName, SNG sng)
    {
        bool isBass = sngName.Contains("_bass", StringComparison.OrdinalIgnoreCase);
        int[] tuning = sng.MetaData.Tuning.Select(t => (int)t).ToArray();
        int capo = sng.MetaData.CapoFretId < 0 ? 0 : sng.MetaData.CapoFretId; // -1 means "no capo"

        // HOW DIFFICULTY WORKS IN SNG:
        // The chart is split into "phrase iterations" (a phrase = a musical section, like the
        // intro riff). Each phrase has its own MaxDifficulty, and sng.Levels[d] holds the notes of
        // difficulty d for the whole song. The full "Master" chart is: for each phrase iteration,
        // take the notes from the level equal to that phrase's MaxDifficulty.
        var notes = new List<Note>();
        for (int pi = 0; pi < sng.PhraseIterations.Length; pi++)
        {
            int maxDiff = sng.Phrases[sng.PhraseIterations[pi].PhraseId].MaxDifficulty;
            var level = sng.Levels.FirstOrDefault(l => l.Difficulty == maxDiff) ?? sng.Levels.Last();
            notes.AddRange(level.Notes.Where(n => n.PhraseIterationId == pi));
        }
        notes.Sort((a, b) => a.Time.CompareTo(b.Time));

        // Each SNG Note is already one "target": a single note, or a chord (Mask has Chord and
        // ChordId points to sng.Chords, whose Frets[] has -1 for strings that aren't played).
        var targets = new List<Target>();
        foreach (var n in notes)
        {
            var techs = Techniques(n.Mask);
            if (techs.Contains("Ignore")) continue; // notes marked "ignore" are never scored by the game

            if (n.ChordId >= 0 && n.Mask.HasFlag(NoteMask.Chord))
            {
                var chord = sng.Chords[n.ChordId];
                var chordNotes = new List<NoteOut>();
                for (int s = 0; s < chord.Frets.Length; s++)
                    if (chord.Frets[s] >= 0)
                        chordNotes.Add(MakeNote(s, chord.Frets[s], n.Sustain, techs, isBass, tuning, capo));
                targets.Add(new Target(targets.Count, Math.Round(n.Time, 3), true, chord.Name, chordNotes));
            }
            else
            {
                targets.Add(new Target(targets.Count, Math.Round(n.Time, 3), false, null,
                    new List<NoteOut> { MakeNote(n.StringIndex, n.Fret, n.Sustain, techs, isBass, tuning, capo) }));
            }
        }

        return new Chart(Path.GetFileName(psarcPath), sngName, isBass, tuning, capo, sng.MetaData.SongLength,
            targets.Count(t => !t.IsChord), targets.Count(t => t.IsChord), targets);
    }

    static NoteOut MakeNote(int str, int fret, float sustain, List<string> techs, bool isBass, int[] tuning, int capo)
    {
        // Sounding pitch = open string + tuning offset + fret.
        // TODO(verify): with a capo, check whether open strings are stored as fret 0 or as the
        // capo fret. For now an open string (fret 0) is counted as capo fret.
        int effectiveFret = (fret == 0 && capo > 0) ? capo : fret;
        int midi = (isBass ? BassOpenMidi : GuitarOpenMidi)[str] + tuning[str] + effectiveFret;
        return new NoteOut(str, fret, midi, MidiName(midi), (float)Math.Round(sustain, 3), techs);
    }

    static string MidiName(int midi) => NoteNames[midi % 12] + (midi / 12 - 1);

    // The note mask is a bit field of techniques. We only keep the ones that matter for detection.
    static List<string> Techniques(NoteMask mask)
    {
        var interesting = new[] { NoteMask.HammerOn, NoteMask.PullOff, NoteMask.Slide, NoteMask.Bend, NoteMask.Tap,
            NoteMask.Harmonic, NoteMask.PinchHarmonic, NoteMask.FretHandMute, NoteMask.PalmMute, NoteMask.Mute,
            NoteMask.Tremolo, NoteMask.Vibrato, NoteMask.Ignore, NoteMask.Arpeggio, NoteMask.DoubleStop };
        return interesting.Where(f => mask.HasFlag(f)).Select(f => f.ToString()).ToList();
    }

    // ---------------------------------------------------------------- Output

    static void PrintSummary(Chart c)
    {
        Console.WriteLine($"{c.Psarc} :: {c.SngName}");
        Console.WriteLine($"  bass={c.IsBass} tuning=[{string.Join(",", c.Tuning)}] capo={c.Capo} length={c.SongLength:F1}s");
        Console.WriteLine($"  targets={c.Targets.Count} single={c.SingleNoteTargets} chords={c.ChordTargets}");
        foreach (var t in c.Targets.Take(15))
            Console.WriteLine($"  {t.Time,8:F3}s  " + (t.IsChord ? $"CHORD {t.ChordName} " : "") +
                string.Join(" + ", t.Notes.Select(n => $"s{n.String}f{n.Fret}={n.Name}")) +
                (t.Notes[0].Techniques.Count > 0 ? "  [" + string.Join(",", t.Notes[0].Techniques) + "]" : ""));
        if (c.Targets.Count > 15) Console.WriteLine("  ...");
    }

    // ---------------------------------------------------------------- Export for the mod
    // Writes one small text chart per arrangement, which the C++ mod reads when a song starts:
    //   <outdir>/<songkey lowercase>/<arrangement>.nbn     e.g. charts/notegel1/lead.nbn
    // The folder name is the SongKey from the song's manifest, the same key the game uses in its
    // audio events ("Play_NoteGel1"), so the mod can find the chart for the song being played.
    // <arrangement> is the .sng suffix: lead, lead2, rhythm, bass, combo...
    // Also writes <outdir>/index.txt (key | artist | title | arrangements) for humans.
    //
    // .nbn format (text, one target per line, times in seconds):
    //   # comments
    //   song <SongKey>
    //   title <Artist - Title>
    //   arrangement <name>
    //   bass <0|1>
    //   tuning <6 semitone offsets>
    //   N <time> <midi> <string> <fret>        single note
    //   C <time> <n> <midi1> <midi2> ...       chord (not used yet by the mod)
    static void Export(string[] inputs, string outDir)
    {
        var files = inputs.SelectMany(i => Directory.Exists(i) ? Directory.GetFiles(i, "*.psarc") : new[] { i })
                          .Where(f => !f.EndsWith("_m.psarc", StringComparison.OrdinalIgnoreCase)) // Mac versions
                          .OrderBy(f => f).ToList();
        var index = new List<string>();
        int written = 0;
        foreach (var file in files)
        {
            try
            {
                using var psarc = OpenPsarc(file);
                foreach (var sngName in psarc.Manifest.Where(n => n.EndsWith(".sng") && !n.Contains("vocals")))
                {
                    string baseName = Path.GetFileNameWithoutExtension(sngName);  // e.g. notegel1_lead
                    string? manifestName = psarc.Manifest.FirstOrDefault(n => n.EndsWith(baseName + ".json", StringComparison.OrdinalIgnoreCase));
                    if (manifestName == null) continue;
                    var attrs = ReadManifestAttributes(psarc, manifestName);
                    if (attrs == null) continue;
                    string songKey = attrs.Value.GetProperty("SongKey").GetString() ?? "";
                    string artist = attrs.Value.TryGetProperty("ArtistName", out var a) ? a.GetString() ?? "" : "";
                    string title = attrs.Value.TryGetProperty("SongName", out var t) ? t.GetString() ?? "" : "";
                    string arr = baseName.Contains('_') ? baseName[(baseName.LastIndexOf('_') + 1)..].ToLowerInvariant() : "lead";

                    var sng = ReadSng(psarc, sngName);
                    var chart = BuildChart(file, sngName, sng);
                    var dir = Path.Combine(outDir, songKey.ToLowerInvariant());
                    Directory.CreateDirectory(dir);
                    using (var w = new StreamWriter(Path.Combine(dir, arr + ".nbn")))
                        WriteChartV2(w, songKey, $"{artist} - {title}", arr, chart, sng);
                    index.Add($"{songKey} | {artist} | {title} | {arr} | single={chart.SingleNoteTargets} chords={chart.ChordTargets}");
                    ++written;
                }
            }
            catch (Exception e)
            {
                Console.WriteLine($"{Path.GetFileName(file)}: ERROR {e.GetType().Name}: {e.Message}");
            }
        }
        File.WriteAllLines(Path.Combine(outDir, "index.txt"), index.OrderBy(l => l));
        Console.WriteLine($"Exported {written} arrangements from {files.Count} files to {Path.GetFullPath(outDir)}");
    }

    // Chart format v2: ALL difficulty levels. With Dynamic Difficulty the game shows, for each phrase
    // iteration, the notes of that phrase's CURRENT level (the mod reads the level from game memory),
    // so the chart must contain every level. Lines:
    //   levels <count>                        number of difficulty levels
    //   count <level> <notes>                 notes in each level, exactly as in the game's memory (used
    //                                         to check that this chart is the arrangement being played)
    //   pi <index> <phraseId> <start> <end>   phrase iterations
    //   N <level> <pi> <time> <midi> <string> <fret> <ignore>      single note
    //   C <level> <pi> <time> <ignore> <n> <midi1> ... <midiN>     chord
    static void WriteChartV2(StreamWriter w, string songKey, string title, string arr, Chart chart, SNG sng)
    {
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        w.WriteLine("# Note-by-Note chart v2 (all difficulty levels), generated by ChartDump from your own game files. Do not distribute.");
        w.WriteLine($"song {songKey}");
        w.WriteLine($"title {title}");
        w.WriteLine($"arrangement {arr}");
        w.WriteLine($"bass {(chart.IsBass ? 1 : 0)}");
        w.WriteLine($"tuning {string.Join(" ", chart.Tuning)}");
        w.WriteLine($"levels {sng.Levels.Length}");
        foreach (var lv in sng.Levels) w.WriteLine($"count {lv.Difficulty} {lv.Notes.Length}");
        for (int i = 0; i < sng.PhraseIterations.Length; i++)
        {
            var pi = sng.PhraseIterations[i];
            w.WriteLine(string.Format(inv, "pi {0} {1} {2:F3} {3:F3}", i, pi.PhraseId, pi.StartTime, pi.EndTime));
        }
        int[] tuning = chart.Tuning;
        int capo = chart.Capo;
        foreach (var lv in sng.Levels)
            foreach (var n in lv.Notes)
            {
                int ign = n.Mask.HasFlag(NoteMask.Ignore) ? 1 : 0;
                if (n.ChordId >= 0 && n.Mask.HasFlag(NoteMask.Chord))
                {
                    var ch = sng.Chords[n.ChordId];
                    var midis = Enumerable.Range(0, ch.Frets.Length).Where(s => ch.Frets[s] >= 0)
                        .Select(s => MakeNote(s, ch.Frets[s], 0, new List<string>(), chart.IsBass, tuning, capo).Midi).ToList();
                    w.WriteLine(string.Format(inv, "C {0} {1} {2:F3} {3} {4} {5}", lv.Difficulty, n.PhraseIterationId, n.Time, ign,
                        midis.Count, string.Join(" ", midis)));
                }
                else
                {
                    var note = MakeNote(n.StringIndex, n.Fret, 0, new List<string>(), chart.IsBass, tuning, capo);
                    w.WriteLine(string.Format(inv, "N {0} {1} {2:F3} {3} {4} {5} {6}", lv.Difficulty, n.PhraseIterationId, n.Time,
                        note.Midi, n.StringIndex, n.Fret, ign));
                }
            }
    }

    // The manifest is JSON: { "Entries": { "<id>": { "Attributes": { "SongKey": ..., ... } } } }
    static System.Text.Json.JsonElement? ReadManifestAttributes(PSARC psarc, string name)
    {
        using var s = psarc.GetEntryStream(name).GetAwaiter().GetResult();
        s.Position = 0;
        using var doc = JsonDocument.Parse(s);
        foreach (var entry in doc.RootElement.GetProperty("Entries").EnumerateObject())
            if (entry.Value.TryGetProperty("Attributes", out var attrs))
                return attrs.Clone();
        return null;
    }

    // Summarizes every arrangement in every .psarc in a folder, so we can pick
    // chord-free songs for the first tests.
    static void Scan(string folder)
    {
        foreach (var file in Directory.GetFiles(folder, "*.psarc").OrderBy(f => f))
        {
            try
            {
                using var psarc = OpenPsarc(file);
                foreach (var sngName in psarc.Manifest.Where(n => n.EndsWith(".sng")))
                {
                    if (sngName.Contains("vocals", StringComparison.OrdinalIgnoreCase)) continue;
                    var c = BuildChart(file, sngName, ReadSng(psarc, sngName));
                    Console.WriteLine($"{Path.GetFileName(file),-60} {Path.GetFileName(sngName),-40} " +
                                      $"len={c.SongLength,6:F0}s single={c.SingleNoteTargets,5} chords={c.ChordTargets,5}");
                }
            }
            catch (Exception e)
            {
                Console.WriteLine($"{Path.GetFileName(file),-60} ERROR {e.GetType().Name}: {e.Message}");
            }
        }
    }
}
