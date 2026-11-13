// Standalone integration proposal: no game assets, renderer dependency or GL calls.
// Feed RGBA pixels in OpenGL's bottom-up order from one unchanged camera view.
using System;

internal enum FramingAction { Wait, Pan, ScaleDistance, Ready, Unavailable }
internal readonly record struct FramingDecision(FramingAction Action, float X = 0, float Y = 0);

internal sealed class AnimationFraming
{
    // At least one idle segment is sampled; very long idles are bounded and cannot
    // guarantee visibility over poses outside that segment. Never move a fitted camera.
    const int Samples = 6, MaxAdjustments = 8;
    const float TanHalfVerticalFov = 0.41421356f; // caller must pass actual FOV if changed
    readonly double sampleIntervalMs;
    readonly double deadlineMs, emptyDeadlineMs;
    bool everVisible;
    readonly int width, height;
    int count, changes, x0, y0, x1, y1;
    double nextSample;
    bool done;
    public string DiagnosticState => $"samples={count}/{Samples} changes={changes}/{MaxAdjustments} bounds=({x0},{y0})-({x1},{y1}) next={nextSample:F1} deadline={deadlineMs:F1} everVisible={everVisible}";

    public AnimationFraming(int width, int height, double startMs, double idleDurationSeconds)
    {
        if (width < 8 || height < 8) throw new ArgumentOutOfRangeException(nameof(width));
        this.width = width; this.height = height;
        double span = double.IsFinite(idleDurationSeconds) && idleDurationSeconds > 0
            ? Math.Clamp(idleDurationSeconds, 0.6, 1.5) : 1.0;
        sampleIntervalMs = span * 1000.0 / (Samples - 1);
        // Every allowed camera change starts a fresh six-pose envelope.
        // Budget all eight changes plus the final envelope and bounded render slack.
        deadlineMs = startMs + (MaxAdjustments + 1) * (span * 1000.0 + 500.0) + 500.0;
        emptyDeadlineMs = startMs + 8000;
        ResetEnvelope(startMs);
    }

    void ResetEnvelope(double nowMs)
    {
        x0 = width; y0 = height; x1 = y1 = -1; count = 0; nextSample = nowMs;
    }

    // ScaleDistance uses X as a multiplier (>1 means move farther away).
    // Pan uses X/Y as camera pan arguments (OpenGL bottom-up coordinates).
    // After Pan/ScaleDistance, apply it then draw a fresh frame before Observe.
    public FramingDecision Observe(ReadOnlySpan<byte> rgba, double nowMs)
    {
        if (done) return new(FramingAction.Ready);
        if (nowMs >= deadlineMs || (!everVisible && nowMs >= emptyDeadlineMs)) return new(FramingAction.Unavailable);
        if (rgba.Length != width * height * 4) throw new ArgumentException("Invalid frame size");
        if (nowMs < nextSample) return new(FramingAction.Wait);
        nextSample = nowMs + sampleIntervalMs;
        int ax0 = width, ay0 = height, ax1 = -1, ay1 = -1;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (rgba[(y * width + x) * 4 + 3] > 8) {
                    ax0 = Math.Min(ax0, x); ax1 = Math.Max(ax1, x);
                    ay0 = Math.Min(ay0, y); ay1 = Math.Max(ay1, y);
                }
        if (ax1 < 0) return new(FramingAction.Wait);
        everVisible = true;
        x0 = Math.Min(x0, ax0); y0 = Math.Min(y0, ay0);
        x1 = Math.Max(x1, ax1); y1 = Math.Max(y1, ay1);
        bool clipped = x0 <= 1 || y0 <= 1 || x1 >= width - 2 || y1 >= height - 2;
        if (clipped) {
            if (++changes > MaxAdjustments) return new(FramingAction.Unavailable);
            ResetEnvelope(nowMs);
            return new(FramingAction.ScaleDistance, 1.4f);
        }
        if (++count < Samples) return new(FramingAction.Wait);
        float cx = (x0 + x1 + 1) / (2f * width) - 0.5f;
        float cy = (y0 + y1 + 1) / (2f * height) - 0.5f;
        if (Math.Abs(cx) > 0.035f || Math.Abs(cy) > 0.035f) {
            if (++changes > MaxAdjustments) return new(FramingAction.Unavailable);
            ResetEnvelope(nowMs);
            return new(FramingAction.Pan, cx * TanHalfVerticalFov * width / height,
                       cy * TanHalfVerticalFov);
        }
        float coverage = Math.Max((x1 - x0 + 1f) / width, (y1 - y0 + 1f) / height);
        // Fit a common 75% square with room for thin edges and motion. Do not
        // magnify barely visible objects by an unbounded amount in one step.
        if (coverage < 0.60f || coverage > 0.86f) {
            if (++changes > MaxAdjustments) return new(FramingAction.Unavailable);
            ResetEnvelope(nowMs);
            return new(FramingAction.ScaleDistance, Math.Clamp(coverage / 0.75f, 0.7f, 1.4f));
        }
        done = true;
        return new(FramingAction.Ready);
    }
}
