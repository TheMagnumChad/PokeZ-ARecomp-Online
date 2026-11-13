using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using GFTool.Renderer;
using GFTool.Renderer.Scene.GraphicsObjects;
using OpenTK.Mathematics;
using OpenTK.Windowing.Desktop;
using OpenTK.Graphics.OpenGL4;
using Trinity.Core.Utils;
using GfAnim = Trinity.Core.Flatbuffers.GF.Animation;

// Private, offscreen proof of concept. This is not integrated into the launcher.
internal static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        if (args.Length is < 3 or > 5) throw new ArgumentException("model.trmdl idle.tranm output-directory [--serve | --worker control-file]");
        bool worker = args.Length == 5 && args[3] == "--worker";
        bool serve = worker || (args.Length == 4 && args[3] == "--serve");
        Directory.CreateDirectory(args[2]);
        int size = serve ? 192 : 256;
        // GLControl shows its GLFW child during setup, stealing game focus.
        // A NativeWindow can create the context while remaining hidden forever.
        using var gl = new OpenTK.Windowing.Desktop.NativeWindow(new NativeWindowSettings {
            StartVisible = false, StartFocused = false, ClientSize = new Vector2i(size, size)
        });
        gl.MakeCurrent();
        RenderContext? renderer = null;
        var start = DateTime.UtcNow;
        int saved = 0;
        string output = args[2];
        string lastControl = worker ? $"{args[0]}\n{args[1]}\n{args[2]}\n" : "";
        bool paused = false;
        var framingClock = System.Diagnostics.Stopwatch.StartNew();
        AnimationFraming? framing = null;
        double nextCaptureMs = 0;
        void LoadModel(string modelPath, string idlePath, string outputPath, bool shiny) {
            // Failures must be attributed to the requested selection, not startup args.
            output = outputPath;
            Directory.CreateDirectory(output);
            renderer!.ClearModels();
            var model = renderer.AddModel(modelPath);
            string visibilityPath = Path.ChangeExtension(idlePath, ".tracm");
            if (File.Exists(visibilityPath)) model.ApplyFixedVisibility(StaticVisibility.Read(visibilityPath));

            if (shiny) model.UsePreviewMaterial(Path.Combine(Path.GetDirectoryName(modelPath)!, Path.GetFileNameWithoutExtension(modelPath) + "_rare.trmtr"));
            renderer.FramePreview(model);
            if (model.TryGetPreviewBounds(out var previewMin, out var previewMax))
                renderer.DollyCamera((previewMax-previewMin).Length * 0.10f);
            var raw = FlatBufferConverter.DeserializeFrom<GfAnim.Animation>(idlePath);
            var anim = new Animation(raw, Path.GetFileNameWithoutExtension(idlePath));
            if (Environment.GetEnvironmentVariable("LUMIOSE_PREVIEW_DIAGNOSTIC_BIND_POSE") != "1") renderer.PlayAnimation(anim);
            output = outputPath;
            Directory.CreateDirectory(output);
            File.WriteAllText(Path.Combine(output, "animation.txt"), $"{anim.Name}: {anim.FrameCount} frames at {anim.FrameRate} fps; {anim.TrackCount} tracks\n");
            start = DateTime.UtcNow;
            saved = serve ? 1 : 0;
            framing = new AnimationFraming(size, size, framingClock.Elapsed.TotalMilliseconds, anim.FrameRate > 0 ? anim.FrameCount / (double)anim.FrameRate : 1.0);
            nextCaptureMs = 0;
        }
        try {
            renderer = new RenderContext(new RenderSurface(gl.MakeCurrent, () => {
                if (framing == null) return;
                var coveragePixels = new byte[size * size * 4];
                GL.ReadPixels(0, 0, size, size, OpenTK.Graphics.OpenGL4.PixelFormat.Rgba, PixelType.UnsignedByte, coveragePixels);
                double nowMs = framingClock.Elapsed.TotalMilliseconds;
                var decision = Environment.GetEnvironmentVariable("LUMIOSE_PREVIEW_DIAGNOSTIC_SKIP_FRAMING") == "1" ? new FramingDecision(FramingAction.Ready) : framing.Observe(coveragePixels, nowMs);
                if (decision.Action != FramingAction.Wait && decision.Action != FramingAction.Ready) Console.Error.WriteLine($"[framing] output={output} elapsed={nowMs:F1} action={decision.Action} x={decision.X:F4} y={decision.Y:F4} {framing.DiagnosticState}");
                if (decision.Action == FramingAction.Wait) return;
                if (decision.Action == FramingAction.Pan) { renderer!.PanCamera(decision.X, decision.Y); return; }
                if (decision.Action == FramingAction.ScaleDistance) { renderer!.ScalePreviewDistance(decision.X); return; }
                if (decision.Action == FramingAction.Unavailable) throw new InvalidDataException("Animated preview framing could not establish a nonempty unclipped envelope.");
                if (serve) { WriteLiveFrame(output, size, size); saved = 1; }
                else if (saved < 3 && nowMs >= nextCaptureMs) { SaveFrame(output, saved++, size, size); nextCaptureMs = nowMs + 1000; }
            }), size, size);
            renderer.RotateCamera(-0.25f, -0.25f);
            if (int.TryParse(Environment.GetEnvironmentVariable("LUMIOSE_PREVIEW_DIAGNOSTIC_ORBIT_STEPS"), out var orbitSteps) && orbitSteps >= 0 && orbitSteps <= 12)
                for (int step = 0; step < orbitSteps; step++) renderer.RotateCamera(0.25f, 0);

            var initialText = worker ? File.ReadAllText(args[4]) : "";
            var initial = initialText.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            bool matches = initial.Length >= 3 && initial[0] == args[0] && initial[1] == args[1] && initial[2] == args[2];
            if (worker) lastControl = matches ? initialText : "";
            LoadModel(args[0], args[1], args[2], matches && initial.Length == 4 && initial[3] == "1");
            while (serve || saved < 3) {
                if (worker) {
                    string control;
                    try { control = File.ReadAllText(args[4]); } catch (IOException) { control = lastControl; }
                    if (control != lastControl) {
                        lastControl = control;
                        var lines = control.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
                        paused = lines.Length == 1 && lines[0] == "pause";
                        if (lines.Length is 3 or 4) { LoadModel(lines[0], lines[1], lines[2], lines.Length == 4 && lines[3] == "1"); paused = false; }
                    }
                }
                if (!paused) renderer.Update();
                Thread.Sleep(serve ? 20 : 33);
            }
        } catch (Exception ex) {
            File.WriteAllText(Path.Combine(output, "error.txt"), ex.ToString());
            throw;
        } finally { renderer?.Dispose(); }
    }

    static (float coverage, float centerX, float centerY, bool clipped) VisibleBounds(int width, int height)
    {
        var color = new byte[width * height * 4];
        GL.ReadPixels(0, 0, width, height, OpenTK.Graphics.OpenGL4.PixelFormat.Rgba, PixelType.UnsignedByte, color);
        int x0 = width, y0 = height, x1 = -1, y1 = -1;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            if (color[(y * width + x) * 4 + 3] == 0) continue;
            x0 = Math.Min(x0, x); x1 = Math.Max(x1, x);
            y0 = Math.Min(y0, y); y1 = Math.Max(y1, y);
        }
        if (x1 < x0) return (0, 0, 0, false);
        return (Math.Max((x1-x0+1)/(float)width, (y1-y0+1)/(float)height),
                (x0+x1+1)/(2f*width)-0.5f, (y0+y1+1)/(2f*height)-0.5f,
                x0 <= 1 || y0 <= 1 || x1 >= width-2 || y1 >= height-2);
    }

    static void WriteLiveFrame(string folder, int width, int height)
    {
        if (width != 192 || height != 192) return;
        var linear = new float[width * height * 4];
        GL.ReadPixels(0, 0, width, height, OpenTK.Graphics.OpenGL4.PixelFormat.Rgba, PixelType.Float, linear);
        var color = FramePixels.LinearPremultipliedToSrgbStraight(linear);
        var frame = new byte[color.Length];
        for (int y = 0; y < height; y++) {
            int source = (height - 1 - y) * width;
            System.Buffer.BlockCopy(color, source * 4, frame, y * width * 4, width * 4);

        }
        string temporary = Path.Combine(folder, "frame.pending");
        string final = Path.Combine(folder, "frame.rgba");
        try { File.WriteAllBytes(temporary, frame); File.Move(temporary, final, true); }
        catch (IOException) { /* A reader may briefly hold the previous frame on Windows. */ }
        catch (UnauthorizedAccessException) { /* Windows may report the same sharing conflict this way. */ }
    }

    static void SaveFrame(string folder, int number, int width, int height)
    {
        var linear = new float[width * height * 4];
        GL.ReadPixels(0, 0, width, height, OpenTK.Graphics.OpenGL4.PixelFormat.Bgra, PixelType.Float, linear);
        var color = FramePixels.LinearPremultipliedToSrgbStraight(linear);
        using var bitmap = new Bitmap(width, height, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        var bits = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, bitmap.PixelFormat);
        for (int y = 0; y < height; y++) {
            var row = new byte[width * 4];
            int source = (height - 1 - y) * width;
            System.Buffer.BlockCopy(color, source * 4, row, 0, row.Length);

            Marshal.Copy(row, 0, bits.Scan0 + y * bits.Stride, row.Length);
        }
        bitmap.UnlockBits(bits);
        bitmap.Save(Path.Combine(folder, $"frame-{number}.png"), ImageFormat.Png);
    }
}
