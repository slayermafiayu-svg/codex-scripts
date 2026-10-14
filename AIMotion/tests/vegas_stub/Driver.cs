// Test driver: runs the real importer/bridge code against the stub API and
// prints what ended up on the fake timeline (read by tests/test_vegas_cs.py).
using System;
using System.Globalization;
using System.IO;
using System.Reflection;
using ScriptPortal.Vegas;

class Driver
{
    static int Main(string[] args)
    {
        Vegas v = new Vegas();
        v.Project.Video.Width = int.Parse(args[0]);
        v.Project.Video.Height = int.Parse(args[1]);
        v.Project.Video.FrameRate = double.Parse(args[2], CultureInfo.InvariantCulture);
        PlugInNode group = new PlugInNode();
        group.Name = "Distort";
        PlugInNode pp = new PlugInNode();
        pp.Name = "Puppet Pin";
        pp.UniqueID = "{Svfx:io.github.slayermafiayu.PuppetPin}";
        group.Kids.Add(pp);
        v.VideoFX.Kids.Add(group);

        string msg = AIMotionImporter.ImportLatest(v);
        Console.WriteLine("MSG " + msg.Replace("\n", " | "));
        foreach (Track tr in v.Project.Tracks.Items)
        {
            VideoTrack vt = (VideoTrack)tr;
            int enabled = 0;
            if (vt.Events[0].Effects.Count > 0)
                for (int k = 1; k <= 20; k++)
                    if (((OFXBooleanParameter)vt.Events[0].Effects[0].OFXEffect.FindParameterByName("pin" + k.ToString("00") + "_on")).Value) enabled++;
            Console.WriteLine("TRACK" + vt.Index + " " + vt.Name + "|" + vt.Events[0].Takes[0].Stream.Media.FilePath + "|" + enabled);
        }
        VideoTrack t = (VideoTrack)v.Project.Tracks.Items[v.Project.Tracks.Items.Count - 1];
        VideoEvent e = t.Events[0];
        Console.WriteLine(string.Format(CultureInfo.InvariantCulture, "EVENT {0} {1} {2}|{3}", e.Start.Milliseconds,
            e.Length.Milliseconds, e.Takes[0].Name, e.Takes[0].Stream.Media.FilePath));
        if (e.Effects.Count > 0)
        {
            OFXEffect o = e.Effects[0].OFXEffect;
            Console.WriteLine("MODE " + ((OFXChoiceParameter)o.FindParameterByName("mode")).Value.Index);
            Console.WriteLine("DENSITY " + ((OFXIntegerParameter)o.FindParameterByName("meshDensity")).Value);
            for (int k = 1; k <= 20; k++)
            {
                string n = "pin" + k.ToString("00");
                OFXBooleanParameter on = (OFXBooleanParameter)o.FindParameterByName(n + "_on");
                if (!on.Value) continue;
                OFXDouble2DParameter pos = (OFXDouble2DParameter)o.FindParameterByName(n + "_pos");
                OFXDouble2DParameter an = (OFXDouble2DParameter)o.FindParameterByName(n + "_anchor");
                Console.WriteLine(string.Format(CultureInfo.InvariantCulture,
                    "PIN {0} {1} {2} {3} {4} {5} {6} {7} {8}", k, an.Value.X, an.Value.Y, pos.IsAnimated, pos.Keys.Count,
                    pos.Keys[0].Key, pos.Keys[pos.Keys.Count - 1].Key, pos.Keys[pos.Keys.Count - 1].Value.X,
                    pos.Keys[pos.Keys.Count - 1].Value.Y));
            }
        }
        else
        {
            Console.WriteLine("SEQ " + v.Project.MediaPool.Sequences[0]);
        }

        // Bridge heartbeat: hook the stub's playback events by reflection, then fire one.
        AIMotionBridgeModule m = new AIMotionBridgeModule();
        BindingFlags bf = BindingFlags.Instance | BindingFlags.NonPublic;
        typeof(AIMotionBridgeModule).GetField("vegas", bf).SetValue(m, v);
        string state = Path.Combine(Path.Combine(AIMotionImporter.Root(), "state"), "vegas_state.json");
        typeof(AIMotionBridgeModule).GetField("stateFile", bf).SetValue(m, state);
        MethodInfo hook = typeof(AIMotionBridgeModule).GetMethod("Hook", bf);
        bool a = (bool)hook.Invoke(m, new object[] { "PlaybackStarted", "OnPlaybackStarted" });
        bool b = (bool)hook.Invoke(m, new object[] { "PlaybackStopped", "OnPlaybackStopped" });
        bool r = (bool)hook.Invoke(m, new object[] { "RenderStarted", "OnRenderStarted" });
        typeof(AIMotionBridgeModule).GetField("haveEvents", bf).SetValue(m, a && b);
        Console.WriteLine("HOOKS " + a + " " + b + " " + r);
        v.FirePlayback(true);
        Console.WriteLine("STATE_PLAYING " + File.ReadAllText(state));
        v.FirePlayback(false);
        Console.WriteLine("STATE_STOPPED " + File.ReadAllText(state));
        return 0;
    }
}
