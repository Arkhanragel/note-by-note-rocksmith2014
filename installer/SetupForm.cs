// The setup window: pick the game folder (found by itself when Steam knows it), see what's there,
// then Install / Update or Uninstall. The work itself is in GameInstall.cs.
using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;

namespace NoteByNoteSetup {

class SetupForm : Form {
    readonly TextBox folder = new TextBox();
    readonly Label status = new Label();
    readonly GroupBox audio = new GroupBox();
    readonly Label audioKept = new Label();  // shown in the audio box instead of the choices when RS_ASIO.ini exists
    readonly Control[] audioChoices;                 // the driver / input choices (only for a new RS_ASIO.ini)
    readonly ComboBox driver = new ComboBox();
    readonly NumericUpDown channel = new NumericUpDown();
    readonly CheckBox removeData = new CheckBox();
    readonly Button install = new Button(), uninstall = new Button();
    readonly TextBox output = new TextBox();
    string gameDir;  // the resolved game folder (null = none)
    bool newIni;     // no RS_ASIO.ini yet: the install creates it from the audio choices

    public SetupForm() {
        Text = $"Note-by-Note for Rocksmith 2014 - Setup {GameInstall.AppVersion()}";
        Font = new Font("Segoe UI", 9.75f);
        AutoScaleMode = AutoScaleMode.Dpi;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(640, 560);
        try { Icon = Icon.ExtractAssociatedIcon(Assembly.GetExecutingAssembly().Location); } catch { }

        var y = 12;
        Add(new Label { Text = "Note-by-Note: the song waits at each note until you play it.",
                        Font = new Font(Font.FontFamily, 11f, FontStyle.Bold), AutoSize = true }, 12, y);
        y += 34;
        Add(new Label { Text = "Rocksmith 2014 folder (or your Steam folder):", AutoSize = true }, 12, y);
        y += 22;
        folder.SetBounds(12, y, 520, 26);
        folder.TextChanged += (s, e) => UpdateStatus(false);
        Controls.Add(folder);
        var browse = new Button { Text = "Browse..." };
        browse.SetBounds(540, y - 1, 88, 28);
        browse.Click += (s, e) => Browse();
        Controls.Add(browse);
        y += 34;
        status.SetBounds(12, y, 616, 84);
        Controls.Add(status);
        y += 90;

        // Audio: the choices are only used when there's no RS_ASIO.ini yet (it's created from them);
        // an existing RS_ASIO.ini is kept as it is, and then the box just says so.
        audio.SetBounds(12, y, 616, 92);
        Controls.Add(audio);
        var driverLabel = new Label { Text = "ASIO driver of your audio interface:", AutoSize = true, Location = new Point(12, 28) };
        driver.DropDownStyle = ComboBoxStyle.DropDownList;
        driver.SetBounds(250, 24, 350, 26);
        var channelLabel = new Label { Text = "Input your guitar is plugged into:", AutoSize = true, Location = new Point(12, 60) };
        channel.SetBounds(250, 56, 60, 26);
        channel.Minimum = 1;
        channel.Maximum = 32;
        audioChoices = new Control[] { driverLabel, driver, channelLabel, channel };
        audio.Controls.AddRange(audioChoices);
        audioKept.SetBounds(12, 26, 590, 56);
        audio.Controls.Add(audioKept);
        y += 100;

        install.SetBounds(12, y, 150, 34);
        install.Click += (s, e) => Run(true);
        Controls.Add(install);
        uninstall.Text = "Uninstall";
        uninstall.SetBounds(172, y, 110, 34);
        uninstall.Click += (s, e) => Run(false);
        Controls.Add(uninstall);
        removeData.Text = "Uninstall: also delete my settings, records and logs";
        removeData.AutoSize = true;
        removeData.Location = new Point(296, y + 8);
        Controls.Add(removeData);
        y += 44;

        output.Multiline = true;
        output.ReadOnly = true;
        output.ScrollBars = ScrollBars.Vertical;
        output.SetBounds(12, y, 616, ClientSize.Height - y - 44);
        Controls.Add(output);
        Add(new Label {
            Text = "Free fan-made mod. Not affiliated with or endorsed by Ubisoft. Uses RS_ASIO (MIT, Micael Dias). "
                 + "Every file it changes is backed up and put back on uninstall.",
            ForeColor = SystemColors.GrayText, AutoSize = false, Size = new Size(616, 36) }, 12, ClientSize.Height - 40);

        foreach (var d in GameLocator.AsioDrivers()) driver.Items.Add(d);
        if (driver.Items.Count > 0) driver.SelectedIndex = 0;

        // Started from NoteByNote_install inside the game folder: that game. Else ask Steam.
        var here = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
        folder.Text = GameLocator.Resolve(here) ?? GameLocator.Find() ?? "";
        if (folder.Text == "") Log("Rocksmith 2014 wasn't found by itself: choose its folder (or your Steam folder).");
        UpdateStatus(false);
    }

    void Add(Control c, int x, int y) { c.Location = new Point(x, y); Controls.Add(c); }

    void Log(string line) { output.AppendText(line + Environment.NewLine); }

    void Browse() {
        using (var dlg = new FolderBrowserDialog { Description = "Choose the Rocksmith 2014 folder or your Steam folder" }) {
            if (Directory.Exists(folder.Text)) dlg.SelectedPath = folder.Text;
            if (dlg.ShowDialog(this) == DialogResult.OK) folder.Text = dlg.SelectedPath;
        }
    }

    // Updates the status line and which buttons make sense for the folder in the box.
    void UpdateStatus(bool afterRun) {
        gameDir = GameLocator.Resolve(folder.Text);
        if (gameDir == null) {
            status.Text = folder.Text.Trim() == "" ? "" : "No Rocksmith 2014 there (Rocksmith2014.exe not found).";
            status.ForeColor = Color.Firebrick;
            install.Enabled = uninstall.Enabled = audio.Enabled = false;
            install.Text = "Install";
            return;
        }
        var st = new GameInstall(gameDir, Log).Status();
        var lines = new System.Collections.Generic.List<string> { "Found: " + gameDir };
        if (st.VersionOlder)
            lines.Add("Game version: supported (the older Rocksmith 2014 Remastered, September 2022).");
        else if (st.VersionSupported)
            lines.Add("Game version: supported.");
        else if (st.VersionModified)
            lines.Add("Game version: supported, but Rocksmith2014.exe was changed (patched?). Note-by-Note will try; "
                      + "if it stays off, please report: " + st.ExeInfo + ".");
        else
            lines.Add("Game version: NOT the one Note-by-Note was made for (it will stay switched off). "
                      + "Please report: " + st.ExeInfo + ".");
        lines.Add(st.RsAsio ? "RS_ASIO: installed" + (st.RsAsioVersion != null ? $" ({st.RsAsioVersion})" : "") + "; it will be replaced by Note-by-Note's build (backed up)."
                            : "RS_ASIO: not installed (Note-by-Note hears your guitar through it; it will be installed).");
        if (st.Installed != null)
            lines.Add("Note-by-Note: installed" + (st.Installed != "" ? " (" + st.Installed + ")" : " (by hand)") + ".");
        status.Text = string.Join(Environment.NewLine, lines);
        status.ForeColor = st.VersionSupported ? SystemColors.ControlText : st.VersionModified ? Color.DarkOrange : Color.Firebrick;
        install.Enabled = true;
        install.Text = st.Installed != null ? "Update / Repair" : "Install";
        uninstall.Enabled = st.Installed != null;
        // The same test as GameInstall.Install: a new RS_ASIO.ini only when there's none.
        newIni = !st.RsAsioIni;
        audio.Enabled = true;
        foreach (var c in audioChoices) c.Visible = newIni;
        audioKept.Visible = !newIni;
        if (newIni) {
            audio.Text = "Audio (there's no RS_ASIO.ini yet: it will be created with this device)";
        } else {
            audio.Text = "Audio";
            audioKept.Text = "Your RS_ASIO.ini is kept as it is"
                + (string.IsNullOrEmpty(st.RsAsioDriver) ? "." : $" (it uses \"{st.RsAsioDriver}\").")
                + " To change the audio device, edit RS_ASIO.ini in the game folder.";
        }
        if (!afterRun && newIni && driver.Items.Count == 0)
            Log("No ASIO driver found. Rocksmith with RS_ASIO needs one: your audio interface's own driver, or ASIO4ALL.");
    }

    void Run(bool doInstall) {
        if (gameDir == null) return;
        if (GameInstall.GameRunning()) {
            MessageBox.Show(this, "Close Rocksmith 2014 first (its files are in use).", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        if (!doInstall && MessageBox.Show(this, "Uninstall Note-by-Note and put back the original files?", Text,
                                          MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK)
            return;
        output.Clear();
        UseWaitCursor = true;
        try {
            var gi = new GameInstall(gameDir, Log);
            if (doInstall) gi.Install(newIni ? driver.SelectedItem as string : null, (int)channel.Value - 1);
            else gi.Uninstall(removeData.Checked);
        } catch (Exception ex) {
            Log("ERROR: " + ex.Message);
            Log(doInstall ? "Nothing is lost: run Uninstall to put back what was changed so far, or try again."
                          : "Try again, or put the files back by hand from NoteByNote_install\\backup.");
        } finally {
            UseWaitCursor = false;
            UpdateStatus(true);
        }
    }
}

static class Program {
    [STAThread]
    static void Main() {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new SetupForm());
    }
}

}  // namespace NoteByNoteSetup
