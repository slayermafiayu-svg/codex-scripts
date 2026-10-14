/*
  AIMotionBridge - VEGAS Pro application extension for AI Motion.

  1. Heartbeat: about four times a second it writes
       <root>\state\vegas_state.json
     with whether VEGAS is playing or rendering and the project's frame size
     and rate. The AI Motion worker pauses every model step while VEGAS plays
     or renders, and also while this file is missing or stale and VEGAS is
     running, so heavy AI work never competes with playback.
  2. Tools > Extensions:
       "AI Motion: Import Latest Result"  (same code as AIMotion_Import.cs)
       "AI Motion: Open Generator"        (starts the local UI)

  Build: build_extension.bat (compiles this file + AIMotion_Import.cs against
  your VEGAS's ScriptPortal.Vegas.dll and copies the DLL to the
  Application Extensions folder). C# 5, .NET Framework 4.

  Playback is detected two ways because the API differs between versions:
  the Vegas PlaybackStarted/PlaybackStopped events and, if present, a
  Transport.IsPlaying property (polled). If neither exists the heartbeat says
  "playing": null and AI Motion treats VEGAS as busy whenever it is running.
*/

using System;
using System.Collections;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Windows.Forms;
using ScriptPortal.Vegas;

public class AIMotionBridgeModule : ICustomCommandModule
{
    Vegas vegas;
    Timer timer;
    bool playingByEvents, rendering, haveEvents, closed;
    PropertyInfo isPlayingProp;
    object transport;
    string stateFile;

    public void InitializeModule(Vegas v)
    {
        vegas = v;
        stateFile = Path.Combine(Path.Combine(AIMotionImporter.Root(), "state"), "vegas_state.json");
        bool a = Hook("PlaybackStarted", "OnPlaybackStarted");
        bool b = Hook("PlaybackStopped", "OnPlaybackStopped");
        haveEvents = a && b;
        Hook("RenderStarted", "OnRenderStarted");
        Hook("RenderFinished", "OnRenderFinished");
        PropertyInfo tp = vegas.GetType().GetProperty("Transport");
        transport = tp == null ? null : tp.GetValue(vegas, null);
        isPlayingProp = transport == null ? null : transport.GetType().GetProperty("IsPlaying");
        if (isPlayingProp != null && isPlayingProp.PropertyType != typeof(bool)) isPlayingProp = null;

        timer = new Timer();
        timer.Interval = 250;
        timer.Tick += delegate(object s, EventArgs e) { WriteState(); };
        timer.Start();
        Application.ApplicationExit += delegate(object s, EventArgs e) { closed = true; WriteState(); };
        WriteState();
    }

    public ICollection GetCustomCommands()
    {
        CustomCommand import = new CustomCommand(CommandCategory.Tools, "AIMotionImportLatest");
        import.DisplayName = "AI Motion: Import Latest Result";
        import.Invoked += OnImport;
        CustomCommand open = new CustomCommand(CommandCategory.Tools, "AIMotionOpenGenerator");
        open.DisplayName = "AI Motion: Open Generator";
        open.Invoked += OnOpen;
        return new CustomCommand[] { import, open };
    }

    // -------------------------------------------------------------- events

    bool Hook(string eventName, string handler)
    {
        try
        {
            EventInfo ei = vegas.GetType().GetEvent(eventName);
            if (ei == null) return false;
            MethodInfo mi = GetType().GetMethod(handler, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            Delegate d = Delegate.CreateDelegate(ei.EventHandlerType, this, mi, false);
            if (d == null) return false;
            ei.AddEventHandler(vegas, d);
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    void OnPlaybackStarted(object sender, EventArgs e) { playingByEvents = true; WriteState(); }
    void OnPlaybackStopped(object sender, EventArgs e) { playingByEvents = false; WriteState(); }
    void OnRenderStarted(object sender, EventArgs e) { rendering = true; WriteState(); }
    void OnRenderFinished(object sender, EventArgs e) { rendering = false; WriteState(); }

    // -------------------------------------------------------------- heartbeat

    string PlayingJson()
    {
        if (closed) return "false";
        if (isPlayingProp != null)
        {
            try
            {
                if ((bool)isPlayingProp.GetValue(transport, null)) return "true";
                if (!haveEvents) return "false";
            }
            catch (Exception) { }
        }
        if (haveEvents) return playingByEvents ? "true" : "false";
        return isPlayingProp != null ? "false" : "null";
    }

    void WriteState()
    {
        try
        {
            CultureInfo ci = CultureInfo.InvariantCulture;
            double ts = (DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
            StringBuilder sb = new StringBuilder();
            sb.Append("{\"version\": 1");
            sb.AppendFormat(ci, ", \"pid\": {0}", Process.GetCurrentProcess().Id);
            sb.AppendFormat(ci, ", \"ts\": {0:0.000}", ts);
            sb.Append(", \"playing\": ").Append(PlayingJson());
            sb.Append(", \"rendering\": ").Append(rendering && !closed ? "true" : "false");
            sb.Append(", \"closed\": ").Append(closed ? "true" : "false");
            Project p = vegas.Project;
            if (p != null)
                sb.AppendFormat(ci, ", \"project\": {{\"width\": {0}, \"height\": {1}, \"fps\": {2:0.######}}}",
                    p.Video.Width, p.Video.Height, p.Video.FrameRate);
            sb.AppendFormat(ci, ", \"bridge\": {{\"events\": {0}, \"is_playing_prop\": {1}}}",
                haveEvents ? "true" : "false", isPlayingProp != null ? "true" : "false");
            sb.Append("}");

            Directory.CreateDirectory(Path.GetDirectoryName(stateFile));
            string tmp = stateFile + ".tmp";
            File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
            if (File.Exists(stateFile)) File.Replace(tmp, stateFile, null);
            else File.Move(tmp, stateFile);
        }
        catch (Exception)
        {
            // The reader may hold the file for a moment; the next tick writes again.
        }
    }

    // -------------------------------------------------------------- commands

    void OnImport(object sender, EventArgs e)
    {
        try
        {
            string msg;
            using (UndoBlock undo = new UndoBlock("AI Motion: içe aktar"))
            {
                msg = AIMotionImporter.ImportLatest(vegas);
            }
            MessageBox.Show(msg, "AI Motion");
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "AI Motion - içe aktarılamadı", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    void OnOpen(object sender, EventArgs e)
    {
        try
        {
            string homeFile = Path.Combine(AIMotionImporter.Root(), "app_home.txt");
            string home = File.Exists(homeFile) ? File.ReadAllText(homeFile).Trim() : "";
            string bat = Path.Combine(home, "start_ui.bat");
            if (!File.Exists(bat))
                throw new FileNotFoundException("AI Motion kurulumu bulunamadı (" + homeFile + "). install.ps1'i çalıştırın.");
            ProcessStartInfo psi = new ProcessStartInfo(bat);
            psi.WorkingDirectory = home;
            psi.UseShellExecute = true;
            Process.Start(psi);
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "AI Motion", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
