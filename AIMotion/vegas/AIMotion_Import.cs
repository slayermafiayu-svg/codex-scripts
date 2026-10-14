/*
  AIMotion_Import.cs - puts the latest AI Motion result on the VEGAS timeline.

  Run it from Tools > Scripting > Run Script... (VEGAS compiles it itself), or
  use the "AI Motion: Import Latest Result" command of the AIMotionBridge
  extension, which contains this same code.

  Reads <root>\outbox\latest.txt (root = %AIMOTION_ROOT% or D:\AIMotion), then
  that folder's vegas_import.txt (written by the AI Motion tool):

    pins   -> stage.png goes on a new video track at the cursor, the Puppet Pin
              effect is added, its pins are anchored on the character and every
              pin position is keyframed (one key per frame). The pins stay
              editable in VEGAS like hand-made ones.
    frames -> the PNG frame sequence goes on a new video track at the cursor.

  The take is named after the result's provenance ("AI: <model> seed=<n>",
  or "TEST - AI DEGIL" for test data), so it stays visible what made it.

  Written for C# 5 (the compiler VEGAS and .NET Framework 4 ship). Members
  of the scripting API that differ between VEGAS versions are reached by
  reflection so the script still compiles where they are missing.
*/

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Windows.Forms;
using ScriptPortal.Vegas;

public class AIMotionResult
{
    public string Folder;
    public string Kind = "";
    public string Source = "";
    public string Label = "";
    public double Fps;
    public int Frames;
    public double Duration;
    public int Width, Height;
    public string Stage = "";
    public bool MeshUseAlpha = true;
    public double AlphaThreshold = 0.05, Expansion = 3.0;
    public int Density = 48, Rigidity = 20;
    public List<int> PinNumbers = new List<int>();
    public Dictionary<int, double[]> Anchors = new Dictionary<int, double[]>();
    public Dictionary<int, string> PinJoints = new Dictionary<int, string>();
    public Dictionary<int, List<double[]>> Keys = new Dictionary<int, List<double[]>>();
    public List<string> LayerImages = new List<string>();  // back to front
    public List<List<int>> LayerPins = new List<List<int>>();
    public string FramesDir = "", FirstFrame = "";

    static double D(string s) { return double.Parse(s, NumberStyles.Float, CultureInfo.InvariantCulture); }
    static int I(string s) { return int.Parse(s, NumberStyles.Integer, CultureInfo.InvariantCulture); }

    public static AIMotionResult Load(string folder)
    {
        string path = Path.Combine(folder, "vegas_import.txt");
        if (!File.Exists(path)) throw new FileNotFoundException("vegas_import.txt yok: " + path);
        AIMotionResult r = new AIMotionResult();
        r.Folder = folder;
        string[] lines = File.ReadAllLines(path);
        if (lines.Length == 0 || lines[0].Trim() != "AIMOTION_VEGAS 1")
            throw new InvalidDataException("Tanınmayan dosya biçimi: " + path);
        bool ended = false;
        for (int i = 1; i < lines.Length; i++)
        {
            string line = lines[i].Trim();
            if (line.Length == 0) continue;
            int sp = line.IndexOf(' ');
            string tag = sp < 0 ? line : line.Substring(0, sp);
            string rest = sp < 0 ? "" : line.Substring(sp + 1);
            string[] f = rest.Split(' ');
            switch (tag)
            {
                case "kind": r.Kind = rest; break;
                case "source": r.Source = rest; break;
                case "label": r.Label = rest; break;
                case "fps": r.Fps = D(f[0]); break;
                case "frames": r.Frames = I(f[0]); break;
                case "duration": r.Duration = D(f[0]); break;
                case "size": r.Width = I(f[0]); r.Height = I(f[1]); break;
                case "stage": r.Stage = rest; break;
                case "mesh":
                    r.MeshUseAlpha = f[0] == "1"; r.AlphaThreshold = D(f[1]); r.Density = I(f[2]);
                    r.Expansion = D(f[3]); r.Rigidity = I(f[4]);
                    break;
                case "pin":
                {
                    int n = I(f[0]);
                    r.PinNumbers.Add(n);
                    r.Anchors[n] = new double[] { D(f[1]), D(f[2]) };
                    r.PinJoints[n] = f.Length > 3 ? f[3] : "";
                    r.Keys[n] = new List<double[]>();
                    break;
                }
                case "key":
                {
                    int n = I(f[0]);
                    r.Keys[n].Add(new double[] { I(f[1]), D(f[2]), D(f[3]) });
                    break;
                }
                case "layer":
                {
                    List<int> pins = new List<int>();
                    if (f[0] != "-")
                        foreach (string n in f[0].Split(',')) pins.Add(I(n));
                    r.LayerPins.Add(pins);
                    r.LayerImages.Add(rest.Substring(f[0].Length + 1));
                    break;
                }
                case "frames_dir": r.FramesDir = rest; break;
                case "first_frame": r.FirstFrame = rest; break;
                case "end": ended = true; break;
            }
        }
        if (!ended) throw new InvalidDataException("Dosya yarım kalmış (end satırı yok): " + path);
        if (r.Source != "ai_model" && r.Source != "test_fixture")
            throw new InvalidDataException("Kaynak bilgisi eksik; içe aktarılmadı.");
        if (r.Fps <= 0 || r.Frames <= 0) throw new InvalidDataException("fps/kare sayısı geçersiz");
        if (r.Kind == "pins")
        {
            if (r.PinNumbers.Count == 0 || r.PinNumbers.Count > 20) throw new InvalidDataException("1-20 pin olmalı");
            foreach (int n in r.PinNumbers)
                if (r.Keys[n].Count != r.Frames) throw new InvalidDataException("pin " + n + " anahtar sayısı yanlış");
        }
        return r;
    }
}

public class AIMotionImporter
{
    public const string PuppetPinName = "Puppet Pin";
    public const string PuppetPinOfxId = "io.github.slayermafiayu.PuppetPin";

    public static string Root()
    {
        string root = Environment.GetEnvironmentVariable("AIMOTION_ROOT");
        return string.IsNullOrEmpty(root) ? @"D:\AIMotion" : root;
    }

    public static string LatestFolder()
    {
        string latest = Path.Combine(Path.Combine(Root(), "outbox"), "latest.txt");
        if (!File.Exists(latest))
            throw new FileNotFoundException("Henüz VEGAS'a aktarılmış sonuç yok (" + latest + "). AI Motion arayüzünde 'VEGAS'a aktar' düğmesine basın.");
        string folder = File.ReadAllText(latest).Trim();
        if (!Directory.Exists(folder)) throw new DirectoryNotFoundException(folder);
        return folder;
    }

    // ------------------------------------------------------------------ entry

    public static string ImportLatest(Vegas vegas)
    {
        AIMotionResult r = AIMotionResult.Load(LatestFolder());
        if (r.Kind == "pins") return ImportPins(vegas, r);
        if (r.Kind == "frames") return ImportFrames(vegas, r);
        throw new InvalidDataException("Bilinmeyen sonuç türü: " + r.Kind);
    }

    static Timecode Ms(double ms) { return Timecode.FromMilliseconds(ms); }

    // New tracks go above the first selected track (or at the top). Every track of one import
    // is inserted at this same index, so a later one lands above the earlier ones.
    static int InsertIndex(Project project)
    {
        foreach (Track t in project.Tracks)
            if (t.Selected) return t.Index;
        return 0;
    }

    static VideoEvent AddMediaEvent(Vegas vegas, Media media, int trackIndex, string trackName, string takeName,
                                    double durationMs)
    {
        MediaStream stream = media.GetVideoStreamByIndex(0);
        if (stream == null) throw new InvalidOperationException("Medyada video akışı yok: " + media.FilePath);
        VideoTrack track = new VideoTrack(vegas.Project, trackIndex, trackName);
        vegas.Project.Tracks.Add(track);
        VideoEvent ev = track.AddVideoEvent(vegas.Transport.CursorPosition, Ms(durationMs));
        Take take = ev.AddTake(stream);
        take.Name = takeName;
        return ev;
    }

    // ------------------------------------------------------------------ pins

    public static string ImportPins(Vegas vegas, AIMotionResult r)
    {
        string warn = "";
        Project project = vegas.Project;
        double projW = project.Video.Width, projH = project.Video.Height;
        if (Math.Abs(projW / projH - (double)r.Width / r.Height) > 0.01)
            warn += string.Format(CultureInfo.InvariantCulture,
                "Uyarı: sahne {0}x{1}, proje {2}x{3}; en-boy oranı farklı, pinler kayabilir.\n", r.Width, r.Height, projW, projH);
        if (Math.Abs(project.Video.FrameRate - r.Fps) > 0.01)
            warn += string.Format(CultureInfo.InvariantCulture,
                "Not: sonuç {0:0.###} fps, proje {1:0.###} fps; anahtarlar zamana göre yerleştirildi.\n", r.Fps, project.Video.FrameRate);

        PlugInNode plug = FindPuppetPin(vegas.VideoFX);
        if (plug == null)
            throw new InvalidOperationException("Puppet Pin efekti bulunamadı. PuppetPinOFX'i kurup VEGAS'ı yeniden başlatın " +
                "(veya AI Motion'da 'PNG kareleri de yaz' seçip kare dizisini kullanın).");

        int anchoredAll = 0, events = 0;
        int at = InsertIndex(project);
        if (r.LayerImages.Count == 0)
        {
            anchoredAll += AddPuppetEvent(vegas, r, plug, r.Stage, r.PinNumbers, at, "AI Motion");
            events = 1;
        }
        else
        {
            // Back to front: each new track is inserted above the previous one, so the front part ends on top.
            for (int i = 0; i < r.LayerImages.Count; i++)
            {
                anchoredAll += AddPuppetEvent(vegas, r, plug, r.LayerImages[i], r.LayerPins[i], at,
                    string.Format(CultureInfo.InvariantCulture, "AI Motion parça {0}", i + 1));
                events++;
            }
        }
        if (anchoredAll < 0)
            warn += "Not: gizli anchor parametrelerine erişilemedi; pinler etkinleştirildikleri yere yapıştırıldı.\n";

        return string.Format(CultureInfo.InvariantCulture,
            "{0}\n{1} olay, {2} pin, {3} kare ({4:0.##} sn) Puppet Pin anahtarı eklendi.\n{5}", r.Label, events,
            r.PinNumbers.Count, r.Frames, r.Frames / r.Fps, warn);
    }

    // Adds one picture with its own Puppet Pin driven by the given pins. Returns -1 if anchors could not be set.
    static int AddPuppetEvent(Vegas vegas, AIMotionResult r, PlugInNode plug, string image, List<int> pins,
                              int trackIndex, string track)
    {
        Media media = new Media(Path.Combine(r.Folder, image));
        double durationMs = r.Frames / r.Fps * 1000.0;
        VideoEvent ev = AddMediaEvent(vegas, media, trackIndex, track, r.Label, durationMs);
        if (pins.Count == 0) return 0;  // a part without pins stays as drawn

        Effect fx = new Effect(plug);
        ev.Effects.Add(fx);
        OFXEffect ofx = fx.OFXEffect;
        if (ofx == null) throw new InvalidOperationException("Puppet Pin bir OFX efekti olarak açılamadı");

        // Part 1 - Mesh: settings, then every pin enabled at its anchor (anchors stick to the picture).
        SetChoice(ofx, "mode", 0);
        SetChoice(ofx, "meshArea", r.MeshUseAlpha ? 0 : 1);
        SetDouble(ofx, "alphaThreshold", r.AlphaThreshold);
        SetInt(ofx, "meshDensity", r.Density);
        SetDouble(ofx, "expansion", r.Expansion);
        SetInt(ofx, "rigidity", r.Rigidity);
        bool anchorsOk = true;
        foreach (int n in pins)
        {
            string p = string.Format(CultureInfo.InvariantCulture, "pin{0:00}", n);
            double[] a = r.Anchors[n];
            Set2D(ofx, p + "_pos", a[0], a[1]);
            SetBool(ofx, p + "_on", true);
            if (!Set2D(ofx, p + "_anchor", a[0], a[1])) anchorsOk = false;
        }

        // Part 2 - Animate: one key per frame on every pin position.
        SetChoice(ofx, "mode", 1);
        foreach (int n in pins)
        {
            string name = string.Format(CultureInfo.InvariantCulture, "pin{0:00}_pos", n);
            OFXDouble2DParameter pos = ofx.FindParameterByName(name) as OFXDouble2DParameter;
            if (pos == null) throw new InvalidOperationException(name + " parametresi yok (Puppet Pin sürümü?)");
            pos.IsAnimated = true;
            foreach (double[] k in r.Keys[n])
            {
                OFXDouble2D v;
                v.X = k[1];
                v.Y = k[2];
                pos.SetValueAtTime(Ms(k[0] / r.Fps * 1000.0), v);
            }
            Notify(pos);
        }
        CallIfExists(ofx, "AllParametersChanged");
        return anchorsOk ? 0 : -1;
    }

    // The plug-in tree API differs between VEGAS versions, so it is walked by reflection.
    static PlugInNode FindPuppetPin(PlugInNode root)
    {
        PlugInNode n = Invoke(root, "GetChildByName", PuppetPinName) as PlugInNode;
        if (n != null) return n;
        n = Invoke(root, "GetChildByUniqueID", "{Svfx:" + PuppetPinOfxId + "}") as PlugInNode;
        if (n != null) return n;
        return Search(root, 0);
    }

    static object Invoke(object target, string method, string arg)
    {
        MethodInfo m = target.GetType().GetMethod(method, new Type[] { typeof(string) });
        return m == null ? null : m.Invoke(target, new object[] { arg });
    }

    static string Prop(object target, string name)
    {
        PropertyInfo p = target.GetType().GetProperty(name);
        object v = p == null ? null : p.GetValue(target, null);
        return v == null ? "" : v.ToString();
    }

    static PlugInNode Search(PlugInNode node, int depth)
    {
        if (depth > 6) return null;
        PropertyInfo childrenProp = node.GetType().GetProperty("Children");
        System.Collections.IEnumerable children =
            (childrenProp != null ? childrenProp.GetValue(node, null) : node) as System.Collections.IEnumerable;
        if (children == null) return null;
        foreach (object o in children)
        {
            PlugInNode c = o as PlugInNode;
            if (c == null) continue;
            if (Prop(c, "Name") == PuppetPinName ||
                Prop(c, "UniqueID").IndexOf("PuppetPin", StringComparison.OrdinalIgnoreCase) >= 0) return c;
            PlugInNode d = Search(c, depth + 1);
            if (d != null) return d;
        }
        return null;
    }

    // ------------------------------------------------------------------ frames

    public static string ImportFrames(Vegas vegas, AIMotionResult r)
    {
        string first = Path.Combine(Path.Combine(r.Folder, r.FramesDir), r.FirstFrame);
        if (!File.Exists(first)) throw new FileNotFoundException(first);
        Media media = AddImageSequence(vegas.Project.MediaPool, first, r.Frames, r.Fps);
        if (media == null)
        {
            System.Diagnostics.Process.Start("explorer.exe", "\"" + Path.GetDirectoryName(first) + "\"");
            return "Bu VEGAS sürümünde betikten resim dizisi eklenemedi. Açılan klasördeki ilk kareyi " +
                   "File > Import > Media ile 'Open still image sequence' seçerek ekleyin.";
        }
        AddMediaEvent(vegas, media, InsertIndex(vegas.Project), "AI Motion (kareler)", r.Label, r.Frames / r.Fps * 1000.0);
        return string.Format(CultureInfo.InvariantCulture, "{0}\n{1} kare ({2:0.##} sn) resim dizisi olarak eklendi.",
            r.Label, r.Frames, r.Frames / r.Fps);
    }

    static Media AddImageSequence(MediaPool pool, string first, int count, double fps)
    {
        MethodInfo m = pool.GetType().GetMethod("AddImageSequence", new Type[] { typeof(string), typeof(int), typeof(double) });
        if (m == null) return null;
        return m.Invoke(pool, new object[] { first, count, fps }) as Media;
    }

    // ------------------------------------------------------------------ OFX helpers

    static bool Set2D(OFXEffect ofx, string name, double x, double y)
    {
        OFXDouble2DParameter p = ofx.FindParameterByName(name) as OFXDouble2DParameter;
        if (p == null) return false;
        OFXDouble2D v;
        v.X = x;
        v.Y = y;
        p.Value = v;
        Notify(p);
        return true;
    }

    static void SetBool(OFXEffect ofx, string name, bool value)
    {
        OFXBooleanParameter p = ofx.FindParameterByName(name) as OFXBooleanParameter;
        if (p == null) throw new InvalidOperationException(name + " parametresi yok");
        p.Value = value;
        Notify(p);
    }

    static void SetChoice(OFXEffect ofx, string name, int index)
    {
        OFXChoiceParameter p = ofx.FindParameterByName(name) as OFXChoiceParameter;
        if (p == null) throw new InvalidOperationException(name + " parametresi yok");
        p.Value = p.Choices[index];
        Notify(p);
    }

    static void SetDouble(OFXEffect ofx, string name, double value)
    {
        OFXDoubleParameter p = ofx.FindParameterByName(name) as OFXDoubleParameter;
        if (p == null) return;
        p.Value = value;
        Notify(p);
    }

    static void SetInt(OFXEffect ofx, string name, int value)
    {
        OFXParameter p = ofx.FindParameterByName(name);
        OFXIntegerParameter ip = p as OFXIntegerParameter;
        if (ip != null) { ip.Value = value; Notify(ip); return; }
        OFXDoubleParameter dp = p as OFXDoubleParameter;
        if (dp != null) { dp.Value = value; Notify(dp); }
    }

    static void Notify(object p) { CallIfExists(p, "ParameterChanged"); }

    static void CallIfExists(object target, string method)
    {
        MethodInfo m = target.GetType().GetMethod(method, Type.EmptyTypes);
        if (m != null) m.Invoke(target, null);
    }
}

public class EntryPoint
{
    public void FromVegas(Vegas vegas)
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
        catch (Exception e)
        {
            MessageBox.Show(e.Message, "AI Motion - içe aktarılamadı", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
