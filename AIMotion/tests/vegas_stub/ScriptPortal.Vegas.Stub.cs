// COMPILE-CHECK STUB - NOT THE REAL VEGAS API.
//
// A minimal stand-in for ScriptPortal.Vegas.dll so the C# files in vegas/ can
// be compiled (C# 5) on a machine without VEGAS, and so tests/test_vegas_cs.py
// can run the importer against a fake timeline. It only declares the members
// the AI Motion code uses directly, with the signatures it assumes; members
// the code reaches by reflection are deliberately absent or optional here.
// Passing this check does not prove the real API matches - that is only
// known once build_extension.bat compiles against your VEGAS installation.

using System;
using System.Collections;
using System.Collections.Generic;

namespace ScriptPortal.Vegas
{
    public class Timecode
    {
        public double Milliseconds;
        public static Timecode FromMilliseconds(double ms) { Timecode t = new Timecode(); t.Milliseconds = ms; return t; }
    }

    public class ProjectVideoProperties { public int Width = 1920; public int Height = 1080; public double FrameRate = 29.97; }

    public class Track { public bool Selected; public int Index; public string Name; }

    public class TrackList : IEnumerable
    {
        public List<Track> Items = new List<Track>();
        public void Add(Track t)
        {
            VideoTrack vt = t as VideoTrack;
            int at = vt == null ? Items.Count : Math.Min(vt.RequestedIndex, Items.Count);
            Items.Insert(at, t);
            for (int i = 0; i < Items.Count; i++) Items[i].Index = i;
        }
        public IEnumerator GetEnumerator() { return Items.GetEnumerator(); }
    }

    public class MediaStream { public Media Media; }
    public class VideoStream : MediaStream { }

    public class Media
    {
        public string FilePath;
        public Media(string path) { FilePath = path; }
        public VideoStream GetVideoStreamByIndex(int i) { VideoStream s = new VideoStream(); s.Media = this; return s; }
    }

    public class MediaPool
    {
        public List<string> Sequences = new List<string>();
        public Media AddImageSequence(string first, int count, double fps) { Sequences.Add(first); return new Media(first); }
    }

    public class Take { public string Name; public MediaStream Stream; }

    public class PlugInNode : IEnumerable
    {
        public string Name { get; set; }
        public string UniqueID { get; set; }
        public List<PlugInNode> Kids = new List<PlugInNode>();
        public IEnumerator GetEnumerator() { return Kids.GetEnumerator(); }
    }

    public struct OFXDouble2D { public double X; public double Y; }
    public class OFXChoice { public int Index; public string Name; }

    public class OFXParameter { public string Name; public int Changed; public void ParameterChanged() { Changed++; } }
    public class OFXBooleanParameter : OFXParameter { public bool Value; }
    public class OFXDoubleParameter : OFXParameter { public double Value; }
    public class OFXIntegerParameter : OFXParameter { public int Value; }
    public class OFXChoiceParameter : OFXParameter { public OFXChoice[] Choices; public OFXChoice Value; }
    public class OFXDouble2DParameter : OFXParameter
    {
        public OFXDouble2D Value;
        public bool IsAnimated;
        public List<KeyValuePair<double, OFXDouble2D>> Keys = new List<KeyValuePair<double, OFXDouble2D>>();
        public void SetValueAtTime(Timecode t, OFXDouble2D v) { Keys.Add(new KeyValuePair<double, OFXDouble2D>(t.Milliseconds, v)); }
    }

    public class OFXEffect
    {
        public Dictionary<string, OFXParameter> Params = new Dictionary<string, OFXParameter>();
        public OFXParameter FindParameterByName(string n) { OFXParameter p; return Params.TryGetValue(n, out p) ? p : null; }
    }

    public class Effect
    {
        public PlugInNode PlugIn;
        public OFXEffect OFXEffect;
        public Effect(PlugInNode p) { PlugIn = p; OFXEffect = StubWorld.MakeOfx(p); }
    }

    public class EffectList : List<Effect> { }

    public class VideoEvent
    {
        public Timecode Start, Length;
        public EffectList Effects = new EffectList();
        public List<Take> Takes = new List<Take>();
        public Take AddTake(MediaStream s) { Take t = new Take(); t.Stream = s; Takes.Add(t); return t; }
    }

    public class VideoTrack : Track
    {
        public List<VideoEvent> Events = new List<VideoEvent>();
        public int RequestedIndex;
        public VideoTrack(Project p, int index, string name) { Name = name; RequestedIndex = index; }
        public VideoEvent AddVideoEvent(Timecode start, Timecode length)
        {
            VideoEvent e = new VideoEvent(); e.Start = start; e.Length = length; Events.Add(e); return e;
        }
    }

    public class Project
    {
        public ProjectVideoProperties Video = new ProjectVideoProperties();
        public TrackList Tracks = new TrackList();
        public MediaPool MediaPool = new MediaPool();
    }

    public class TransportControl { public Timecode CursorPosition = Timecode.FromMilliseconds(1000); }

    public class Vegas
    {
        public Project Project = new Project();
        public TransportControl Transport = new TransportControl();
        public PlugInNode VideoFX = new PlugInNode();
        public event EventHandler PlaybackStarted;
        public event EventHandler PlaybackStopped;
        public void FirePlayback(bool on) { EventHandler h = on ? PlaybackStarted : PlaybackStopped; if (h != null) h(this, EventArgs.Empty); }
    }

    public class UndoBlock : IDisposable { public UndoBlock(string n) { } public void Dispose() { } }

    public enum CommandCategory { Tools, View, Edit }

    public class CustomCommand
    {
        public string DisplayName;
        public event EventHandler Invoked;
        public CustomCommand(CommandCategory c, string name) { }
        public void Invoke() { if (Invoked != null) Invoked(this, EventArgs.Empty); }
    }

    public interface ICustomCommandModule
    {
        void InitializeModule(Vegas vegas);
        ICollection GetCustomCommands();
    }

    // Test helpers (not part of any VEGAS API).
    public static class StubWorld
    {
        public static OFXEffect MakeOfx(PlugInNode p)
        {
            OFXEffect e = new OFXEffect();
            OFXChoiceParameter mode = new OFXChoiceParameter();
            mode.Choices = new OFXChoice[] { new OFXChoice(), new OFXChoice() };
            mode.Choices[1].Index = 1;
            e.Params["mode"] = mode;
            OFXChoiceParameter area = new OFXChoiceParameter();
            area.Choices = new OFXChoice[] { new OFXChoice(), new OFXChoice() };
            area.Choices[1].Index = 1;
            e.Params["meshArea"] = area;
            e.Params["alphaThreshold"] = new OFXDoubleParameter();
            e.Params["meshDensity"] = new OFXIntegerParameter();
            e.Params["expansion"] = new OFXDoubleParameter();
            e.Params["rigidity"] = new OFXIntegerParameter();
            for (int k = 1; k <= 20; k++)
            {
                string n = "pin" + k.ToString("00");
                e.Params[n + "_on"] = new OFXBooleanParameter();
                e.Params[n + "_pos"] = new OFXDouble2DParameter();
                e.Params[n + "_anchor"] = new OFXDouble2DParameter();
            }
            return e;
        }
    }
}
