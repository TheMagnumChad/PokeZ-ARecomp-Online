// Original, asset-free pixel conversion used by live RGBA and PNG exports.
internal static class FramePixels {
    internal static void Unpremultiply(Span<byte> pixels) {
        if ((pixels.Length & 3) != 0) throw new ArgumentException("RGBA byte count");
        for (int i=0;i<pixels.Length;i+=4) {
            int a=pixels[i+3];
            for(int c=0;c<3;c++) pixels[i+c]=a==0?(byte)0:(byte)Math.Min(255,(pixels[i+c]*255+a/2)/a);
        }
    }
    // Input is linear-light premultiplied RGBA or BGRA; output preserves channel
    // order and is straight-alpha sRGB, suitable for PNG and UI image upload.
    // Apply exactly once, after all linear-light compositing has completed.
    internal static byte[] LinearPremultipliedToSrgbStraight(ReadOnlySpan<float> pixels) {
        if ((pixels.Length & 3) != 0) throw new ArgumentException("RGBA float count");
        byte[] result = new byte[pixels.Length];
        for (int i = 0; i < pixels.Length; i += 4) {
            float alpha = float.IsFinite(pixels[i + 3]) ? Math.Clamp(pixels[i + 3], 0f, 1f) : 0f;
            result[i + 3] = Quantize(alpha);
            if (result[i + 3] == 0) continue;
            for (int c = 0; c < 3; c++) {
                float linear = pixels[i + c] / alpha;
                linear = float.IsNaN(linear) ? 0f : Math.Clamp(linear, 0f, 1f);
                float srgb = linear <= 0.0031308f ? 12.92f * linear : 1.055f * MathF.Pow(linear, 1f / 2.4f) - 0.055f;
                result[i + c] = Quantize(srgb);
            }
        }
        return result;
    }
    private static byte Quantize(float value) => (byte)Math.Clamp((int)MathF.Floor(value * 255f + 0.5f), 0, 255);
}

