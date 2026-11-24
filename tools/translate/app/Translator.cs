// The translation editor's page in a window of its own (the Edge web view Windows has). It remembers the ROM and the
// mods folder, keeps the work in a file as it is typed, and writes the pack straight into the mods folder, where a
// running game reads it again by itself. The page is inside this program; it is served to the view at an address that
// exists only there, and nothing is fetched from the internet. (C# 5: Windows' own csc. As tools/sprites/app.)
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

class Translator : Form {
    const string Home = "https://translation.invalid/";        // (.invalid: never a real site)
    readonly WebView2 view = new WebView2();
    // where it keeps its settings and the work (PSIV_TRANSLATOR_HOME: another place, for tests; off the screen then)
    readonly string testHome = Environment.GetEnvironmentVariable("PSIV_TRANSLATOR_HOME");
    readonly string dir;
    bool closing;
    readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
    readonly HashSet<string> saved = new HashSet<string>();    // the files written since it was started
    string romPath = "", modsPath = "";

    string Work { get { return Path.Combine(dir, "work.json"); } }
    string Ini { get { return Path.Combine(dir, "translator.ini"); } }
    void LoadIni() {
        if (!File.Exists(Ini)) return;
        foreach (var line in File.ReadAllLines(Ini)) {
            if (line.StartsWith("rom = ")) romPath = line.Substring(6).Trim();
            if (line.StartsWith("mods = ")) modsPath = line.Substring(7).Trim();
        }
    }
    // in a folder of its own inside the game's: that game's mods are the ones meant, until another folder is picked
    void FindGame() {
        if (Directory.Exists(modsPath)) return;
        var up = Directory.GetParent(Path.GetDirectoryName(Application.ExecutablePath));
        if (up != null && File.Exists(Path.Combine(up.FullName, "psiv.exe")) && Directory.Exists(Path.Combine(up.FullName, "mods")))
            modsPath = Path.Combine(up.FullName, "mods");
    }
    void SaveIni() { File.WriteAllLines(Ini, new[] { "rom = " + romPath, "mods = " + modsPath }); }

    Translator() {
        dir = testHome ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PSIV Translation Editor");
        Directory.CreateDirectory(dir);
        LoadIni();
        FindGame();
        Text = "PSIV Recomp Translation Editor";
        BackColor = Color.FromArgb(11, 19, 56);
        ClientSize = new Size(1400, 880);
        MinimumSize = new Size(480, 480);
        if (testHome != null) { StartPosition = FormStartPosition.Manual; Location = new Point(-6000, 0); ShowInTaskbar = false; }
        else { StartPosition = FormStartPosition.CenterScreen; WindowState = FormWindowState.Maximized; }
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }
        view.Dock = DockStyle.Fill;
        view.DefaultBackgroundColor = BackColor;
        Controls.Add(view);
        // the page sends the work a moment after the last change: closing waits that long for it
        FormClosing += delegate(object s, FormClosingEventArgs e) {
            if (closing || view.CoreWebView2 == null) return;
            e.Cancel = true;
            closing = true;
            Hide();
            var t = new Timer { Interval = 700 };
            t.Tick += delegate { t.Stop(); Close(); };
            t.Start();
        };
        Load += async delegate {
            try {
                var env = await CoreWebView2Environment.CreateAsync(null, Path.Combine(dir, "view"));
                await view.EnsureCoreWebView2Async(env);
            } catch (Exception e) {
                MessageBox.Show(this, "The Edge web view could not be started:\n" + e.Message, Text);
                Close();
                return;
            }
            var c = view.CoreWebView2;
            c.Settings.IsStatusBarEnabled = false;
            c.Settings.AreDevToolsEnabled = false;
            c.AddWebResourceRequestedFilter("*", CoreWebView2WebResourceContext.All);
            c.WebResourceRequested += delegate(object s, CoreWebView2WebResourceRequestedEventArgs a) { Serve(c, a); };
            c.NewWindowRequested += delegate(object s, CoreWebView2NewWindowRequestedEventArgs a) { a.Handled = true; };
            c.NavigationStarting += delegate(object s, CoreWebView2NavigationStartingEventArgs a) { if (!a.Uri.StartsWith(Home)) a.Cancel = true; };
            c.WebMessageReceived += delegate(object s, CoreWebView2WebMessageReceivedEventArgs a) {
                string text = a.TryGetWebMessageAsString();
                BeginInvoke((Action)delegate { Message(text); });
            };
            c.Navigate(Home);
        };
    }

    void Serve(CoreWebView2 c, CoreWebView2WebResourceRequestedEventArgs a) {
        string path = a.Request.Uri.StartsWith(Home) ? new Uri(a.Request.Uri).AbsolutePath : null;
        Stream body = null;
        string type = "text/html; charset=utf-8";
        if (path == "/") body = Assembly.GetExecutingAssembly().GetManifestResourceStream("index.html");
        else if (path == "/rom" && File.Exists(romPath)) { body = new MemoryStream(File.ReadAllBytes(romPath)); type = "application/octet-stream"; }
        else if (path == "/work" && File.Exists(Work)) { body = new MemoryStream(File.ReadAllBytes(Work)); type = "application/json; charset=utf-8"; }
        else if (path == null && (a.Request.Uri.StartsWith("data:") || a.Request.Uri.StartsWith("blob:"))) return;
        a.Response = body != null ? c.Environment.CreateWebResourceResponse(body, 200, "OK", "Content-Type: " + type)
                                  : c.Environment.CreateWebResourceResponse(null, 404, "Not here", "");
    }

    void Tell(object o) { view.CoreWebView2.PostWebMessageAsString(json.Serialize(o)); }
    void Say(string words) { Tell(new { what = "say", words = words }); }

    void Message(string text) {
        var m = json.Deserialize<Dictionary<string, object>>(text);
        switch (m["what"] as string) {
            case "ready": if (File.Exists(romPath)) Tell(new { what = "rom" }); break;
            case "badrom": romPath = ""; SaveIni(); break;
            case "pickrom":
                using (var d = new OpenFileDialog { Title = "Your Phantasy Star IV (USA) ROM", Filter = "ROMs (*.bin;*.md;*.gen;*.smd)|*.bin;*.md;*.gen;*.smd|All files|*.*" })
                    if (d.ShowDialog(this) == DialogResult.OK) { romPath = d.FileName; SaveIni(); Tell(new { what = "rom" }); }
                break;
            case "keep": Keep((string)m["text"]); break;
            case "export": Export((string)m["name"], (string)m["text"]); break;
            case "game": ToGame((string)m["folder"], (string)m["name"], (string)m["language"], (string)m["text"], Convert.ToInt32(m["lines"])); break;
            case "folder": modsPath = ""; SaveIni(); PickMods(); break;
        }
    }

    // a file written beside the last copy, then put in its place: a crash, or the game reading it, never meets half of it
    static void Write(string path, string text, string before) {
        File.WriteAllText(path + ".new", text, new UTF8Encoding(false));
        if (File.Exists(path)) File.Replace(path + ".new", path, before); else File.Move(path + ".new", path);
    }

    void Keep(string text) {
        try { Write(Work, text, Work + ".before"); }
        catch (Exception e) { Say("Your work could not be kept on disk (" + e.Message + "). Export the pack to be safe."); }
    }

    static bool Plain(string name, string pattern) { return name != null && Regex.IsMatch(name, pattern); }

    void Export(string name, string text) {
        if (!Plain(name, @"^psiv-[a-z0-9_-]{1,80}\.txt$")) return;
        using (var d = new SaveFileDialog { Title = "Your pack", FileName = name, Filter = "A pack (*.txt)|*.txt" })
            if (d.ShowDialog(this) == DialogResult.OK)
                try { Write(d.FileName, text, null); Say("Exported " + Path.GetFileName(d.FileName)); }
                catch (Exception e) { Say("That did not save: " + e.Message); }
    }

    bool PickMods() {
        using (var d = new FolderBrowserDialog { Description = "The game's mods folder: the one named \"mods\" beside psiv.ini and your saves. (Make it if it is not there yet.)" }) {
            if (d.ShowDialog(this) != DialogResult.OK) return false;
            modsPath = d.SelectedPath;
            SaveIni();
            return true;
        }
    }

    void ToGame(string folder, string name, string language, string text, int lines) {
        if (!Plain(folder, @"^[a-z0-9_-]{1,80}$") || !Plain(name, @"^psiv-[a-z0-9_-]{1,80}\.txt$")) return;
        if (!Directory.Exists(modsPath) && !PickMods()) return;
        string to = Path.Combine(modsPath, folder), file = Path.Combine(to, name), about = Path.Combine(to, "mod.txt");
        try {
            if (File.Exists(file) && !saved.Contains(file) && File.ReadAllText(file) != text
                && MessageBox.Show(this, "There is already a pack in\n" + to + "\n\nReplace it with this one?", Text,
                                   MessageBoxButtons.OKCancel, MessageBoxIcon.Question, MessageBoxDefaultButton.Button2) != DialogResult.OK) return;
            Directory.CreateDirectory(to);
            Write(file, text, null);
            bool first = saved.Add(file) && !File.Exists(about);
            if (first) File.WriteAllText(about, "name = " + Regex.Replace(language, @"[\r\n]", " ") + "\nabout = A translation\n", new UTF8Encoding(false));
            Say(first ? "Saved! Switch “" + language + "” on from the launcher’s Mods page." : "Saved to the game: " + lines + (lines == 1 ? " line" : " lines"));
        } catch (Exception e) { Say("That did not save: " + e.Message); }
    }

    [STAThread]
    static void Main() {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new Translator());
    }
}
