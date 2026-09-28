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
    readonly ComboBox driver = new ComboBox();
    readonly NumericUpDown channel = new NumericUpDown();
    readonly CheckBox removeData = new CheckBox();
    readonly Button install = new Button(), uninstall = new Button();
    readonly TextBox output = new TextBox();
    string gameDir;  // the resolved game folder (null = none)

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
        status.SetBounds(12, y, 616, 64);
        Controls.Add(status);
        y += 70;

        // Audio: only when RS_ASIO isn't installed yet (then RS_ASIO.ini is created from this).
        audio.Text = "Audio (RS_ASIO is not installed yet: it will be, with this device)";
        audio.SetBounds(12, y, 616, 92);
        Controls.Add(audio);
        audio.Controls.Add(new Label { Text = "ASIO driver of your audio interface:", AutoSize = true, Location = new Point(12, 28) });
        driver.DropDownStyle = ComboBoxStyle.DropDownList;
        driver.SetBounds(250, 24, 350, 26);
        audio.Controls.Add(driver);
        audio.Controls.Add(new Label { Text = "Input your guitar is plugged into:", AutoSize = true, Location = new Point(12, 60) });
        channel.SetBounds(250, 56, 60, 26);
        channel.Minimum = 1;
        channel.Maximum = 32;
        audio.Controls.Add(channel);
        y += 100;

        install.SetBounds(12, y, 150, 34);
        install.Click += (s, e) => Run(true);
        Controls.Add(install);
        uninstall.Text = "Uninstall";
        uninstall.SetBounds(172, y, 110, 34);
        uninstall.Click += (s, e) => Run(false);
        Controls.Add(uninstall);
        removeData.Text = "When uninstalling, also delete my settings and logs";
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
        lines.Add(st.VersionSupported ? "Game version: supported."
                                      : "Game version: NOT the one Note-by-Note was made for. It can be installed, but it will stay switched off.");
        lines.Add(st.RsAsio ? "RS_ASIO: installed" + (st.RsAsioVersion != null ? $" ({st.RsAsioVersion})" : "") + "; it will be replaced by Note-by-Note's build (backed up)."
                            : "RS_ASIO: not installed (Note-by-Note hears your guitar through it; it will be installed).");
        if (st.Installed != null)
            lines.Add("Note-by-Note: installed" + (st.Installed != "" ? " (" + st.Installed + ")" : " (by hand)") + ".");
        status.Text = string.Join(Environment.NewLine, lines);
        status.ForeColor = st.VersionSupported ? SystemColors.ControlText : Color.DarkOrange;
        install.Enabled = true;
        install.Text = st.Installed != null ? "Update / Repair" : "Install";
        uninstall.Enabled = st.Installed != null;
        audio.Enabled = !st.RsAsio && st.Installed == null;
        if (!afterRun && audio.Enabled && driver.Items.Count == 0)
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
            if (doInstall) gi.Install(audio.Enabled ? driver.SelectedItem as string : null, (int)channel.Value - 1);
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
