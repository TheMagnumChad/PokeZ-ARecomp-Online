// Dependency-free synthetic checks. Compile together with AnimationFraming.cs
// only when the shared build slot is available; no game or extracted assets needed.
#if ANIMATION_FRAMING_TESTS
using System;

internal static class AnimationFramingChecks
{
    static byte[] Frame(int x0, int y0, int x1, int y1)
    {
        var pixels = new byte[192 * 192 * 4];
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) pixels[(y * 192 + x) * 4 + 3] = 255;
        return pixels;
    }
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
    static FramingDecision Six(AnimationFraming f, byte[] frame, int start = 0)
    {
        FramingDecision result = default;
        for (int i = 0; i < 6; i++) result = f.Observe(frame, start + 201 * i);
        return result;
    }
    public static void Main()
    {
        var centered = Frame(24, 24, 167, 167);
        var f = new AnimationFraming(192, 192, 0, 1);
        Check(Six(f, centered).Action == FramingAction.Ready, "centered fit");
        Check(f.Observe(centered, 2000).Action == FramingAction.Ready, "stable camera");
        foreach (var clipped in new[] { Frame(0, 40, 90, 140), Frame(100, 40, 191, 140),
                                       Frame(40, 0, 140, 90), Frame(40, 100, 140, 191) }) {
            f = new AnimationFraming(192, 192, 0, 1);
            var d = f.Observe(clipped, 0);
            Check(d.Action == FramingAction.ScaleDistance && d.X > 1, "edge zooms out");
        }
        f = new AnimationFraming(192, 192, 0, 1);
        Check(Six(f, Frame(55, 36, 174, 155)).X > 0, "right pan");
        f = new AnimationFraming(192, 192, 0, 1);
        Check(Six(f, Frame(17, 36, 136, 155)).X < 0, "left pan");
        f = new AnimationFraming(192, 192, 0, 1);
        Check(Six(f, Frame(36, 55, 155, 174)).Y > 0, "up pan");
        f = new AnimationFraming(192, 192, 0, 1);
        Check(Six(f, Frame(36, 17, 155, 136)).Y < 0, "down pan");
        f = new AnimationFraming(192, 192, 0, 1);
        var zoom = Six(f, Frame(60, 60, 131, 131));
        Check(zoom.Action == FramingAction.ScaleDistance && zoom.X < 1, "small model zooms in");
        f = new AnimationFraming(192, 192, 0, 1);
        FramingDecision envelope = default;
        for (int i = 0; i < 6; i++)
            envelope = f.Observe(i % 2 == 0 ? Frame(25, 24, 95, 167) : Frame(96, 24, 166, 167), i * 201);
        Check(envelope.Action == FramingAction.Ready, "motion envelope includes both extremes");
        f = new AnimationFraming(192, 192, 0, 1);
        var blank = new byte[192 * 192 * 4];
        Check(f.Observe(blank, 0).Action == FramingAction.Wait, "blank waits");
        Check(f.Observe(blank, 8000).Action == FramingAction.Unavailable, "blank times out");
        f = new AnimationFraming(192, 192, 0, 1);
        for (int i = 0; i < 8; i++) Check(f.Observe(Frame(0, 40, 90, 140), i).Action == FramingAction.ScaleDistance, "bounded recovery");
        Check(f.Observe(Frame(0, 40, 90, 140), 8).Action == FramingAction.Unavailable, "persistent clipping fails");
        // Five legitimate adjustments consume over eight seconds at 1.5-second
        // envelopes. The final fresh envelope must still be allowed to converge.
        f = new AnimationFraming(192, 192, 0, 1.5);
        for (int phase = 0; phase < 5; phase++) {
            FramingDecision phaseDecision = default;
            for (int i = 0; i < 6; i++) phaseDecision = f.Observe(Frame(55,36,174,155), phase*1600+i*301);
            Check(phaseDecision.Action == FramingAction.Pan, "all allowed envelopes budgeted");
        }
        FramingDecision finalDecision = default;
        for (int i = 0; i < 6; i++) finalDecision = f.Observe(centered, 8000+i*301);
        Check(finalDecision.Action == FramingAction.Ready, "convergence beyond former eight-second deadline");
        f = new AnimationFraming(192,192,0,1.5);
        Check(f.Observe(centered,0).Action == FramingAction.Wait,"start bounded envelope");
        Check(f.Observe(centered,18501).Action == FramingAction.Unavailable,"finite total deadline");
        Console.WriteLine("Synthetic framing checks passed.");
    }
}
#endif
