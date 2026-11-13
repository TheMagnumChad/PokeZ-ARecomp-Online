using BntxLibrary;
using OpenTK.Graphics.OpenGL4;

namespace GFTool.Renderer.Scene.GraphicsObjects;

// Original adapter. No NxMiddleware implementation is used.
// Only locally supplied textures are read, including full mip chains and cube probes.
internal static class LocalBntxTexture
{
    internal static byte[] Untile(ReadOnlySpan<byte> src, int widthBlocks, int heightBlocks, int bytesPerBlock, int blockHeightLog2, bool linear)
    {
        if (widthBlocks <= 0 || heightBlocks <= 0 || blockHeightLog2 < 0 || blockHeightLog2 > 5)
            throw new InvalidDataException("Invalid texture layout");
        int row = checked(widthBlocks * bytesPerBlock);
        byte[] output = new byte[checked(row * heightBlocks)];
        int gobHeight = 8 << blockHeightLog2;
        long gobsPerRow = (row + 63L) / 64;
        for (int y = 0; y < heightBlocks; y++)
        for (int x = 0; x < row; x++)
        {
            // Tegra block-linear: 64-byte x 8-row GOBs grouped vertically.
            long pos = linear ? (long)y * ((row + 31L) & ~31L) + x :
                ((long)(y / gobHeight) * gobsPerRow + x / 64) * gobHeight * 64 +
                (y % gobHeight / 8) * 512L + (x % 64 / 32) * 256L +
                (y % 8 / 2) * 64L + (x % 32 / 16) * 32L + (y % 2) * 16L + x % 16;
            if (pos < 0 || pos >= src.Length) throw new InvalidDataException("Truncated texture data");
            output[y * row + x] = src[(int)pos];
        }
        return output;
    }

    internal static int Create(string path, string preferredName, out TextureTarget target)
    {
        target = TextureTarget.Texture2D;
        var file = new BntxFile(path);
        BntxTexture? t = file.Textures.Values.FirstOrDefault(v => string.Equals(v.Name, preferredName, StringComparison.OrdinalIgnoreCase));
        if (t == null && file.Textures.Count == 1) t = file.Textures.Values.Single();
        if (t == null) throw new InvalidDataException($"Texture {preferredName} not found in BNTX");
        bool cube = t.ArrayCount == 6 && t.Header.SurfaceDim == 3 && t.Width == t.Height;
        target = cube ? TextureTarget.TextureCubeMap : TextureTarget.Texture2D;
        if (file.Platform != PlatformKind.NX || t.Width == 0 || t.Height == 0 || t.Width > 8192 || t.Height > 8192 ||
            (!cube && t.ArrayCount != 1) || t.Depth != 1 || t.Dim != GfxImageStorageDimension.Dim2D || t.MipCount < 1)
            throw new NotSupportedException("Only bounded NX 2D textures or six-face cubes are supported");
        int w = (int)t.Width, h = (int)t.Height, f = (int)t.ImageFormat;
        int channel = f >> 8, type = f & 255;
        bool srgb = type == 6;
        int format, bytes, block;
        switch (channel)
        {
            case 0x1a when type is 1 or 6: format = srgb ? 0x8c4d : 0x83f1; bytes = 8; block = 4; break;
            case 0x1b when type is 1 or 6: format = srgb ? 0x8c4e : 0x83f2; bytes = 16; block = 4; break;
            case 0x1c when type is 1 or 6: format = srgb ? 0x8c4f : 0x83f3; bytes = 16; block = 4; break;
            case 0x1d when type is 1 or 2: format = type == 2 ? 0x8dbc : 0x8dbb; bytes = 8; block = 4; break;
            case 0x1e when type is 1 or 2: format = type == 2 ? 0x8dbe : 0x8dbd; bytes = 16; block = 4; break;
            case 0x1f when type is 2 or 5 or 10: format = type == 2 ? 0x8e8e : 0x8e8f; bytes = 16; block = 4; break;
            case 0x20 when type is 1 or 6: format = srgb ? 0x8e8d : 0x8e8c; bytes = 16; block = 4; break;
            case 0x15 when type == 5: format = 0x881a; bytes = 8; block = 1; break; // RGBA16F
            case 0xb or 0xc when type is 1 or 6: format = srgb ? 0x8c43 : 0x8058; bytes = 4; block = 1; break;
            default: throw new NotSupportedException($"Unsupported BNTX format 0x{f:X}");
        }
        if (t.TileMode != GfxTileMode.Linear && t.TileMode != GfxTileMode.Optimal)
            throw new NotSupportedException("Unsupported texture tile mode");
        int faces = cube ? 6 : 1;
        if (t.ImageData.Length % faces != 0 || t.MipOffsets.Length != t.MipCount)
            throw new InvalidDataException("Invalid texture face/mip layout");
        int stride = t.ImageData.Length / faces;
        int levels = t.MipCount;
        if (levels > 1 + (int)Math.Floor(Math.Log2(Math.Max(w,h))))
            throw new InvalidDataException("Invalid texture mip count");
        int id = GL.GenTexture();
        try
        {
            GL.BindTexture(target, id);
            for (int face = 0; face < faces; face++)
            for (int mip = 0; mip < levels; mip++)
            {
                int mw = Math.Max(1,w >> mip), mh = Math.Max(1,h >> mip);
                int wb = (mw + block - 1) / block, hb = (mh + block - 1) / block;
                int bh = checked((int)t.BlockHeightLog2);
                while (bh > 0 && hb <= (8 << (bh - 1))) bh--;
                int offset = checked((int)t.MipOffsets[mip]);
                int end = mip+1 < levels ? checked((int)t.MipOffsets[mip+1]) : stride;
                if (offset < 0 || end <= offset || end > stride)
                    throw new InvalidDataException("Texture mip exceeds face bounds");
                byte[] pixels = Untile(t.ImageData.Span.Slice(checked(face*stride+offset),end-offset), wb,hb,bytes,bh,t.TileMode == GfxTileMode.Linear);
                TextureTarget upload = cube ? (TextureTarget)((int)TextureTarget.TextureCubeMapPositiveX+face) : target;
                if (block == 4)
                    GL.CompressedTexImage2D(upload,mip,(InternalFormat)format,mw,mh,0,pixels.Length,pixels);
                else
                    GL.TexImage2D(upload,mip,(PixelInternalFormat)format,mw,mh,0,
                        channel == 0xc ? PixelFormat.Bgra : PixelFormat.Rgba,
                        channel == 0x15 ? PixelType.HalfFloat : PixelType.UnsignedByte,pixels);
                var error = GL.GetError();
                if (error != ErrorCode.NoError) throw new NotSupportedException($"Texture upload rejected: {error}, face {face}, mip {mip}, format 0x{f:X}");
            }            var channels = new[] { t.GfxChannelSourceR, t.GfxChannelSourceG, t.GfxChannelSourceB, t.GfxChannelSourceA };
            for (int i = 0; i < channels.Length; i++)
            {
                int value = channels[i] switch {
                    GfxChannelSource.Zero => 0, GfxChannelSource.One => 1,
                    GfxChannelSource.Red => 0x1903, GfxChannelSource.Green => 0x1904,
                    GfxChannelSource.Blue => 0x1905, GfxChannelSource.Alpha => 0x1906,
                    _ => throw new InvalidDataException("Invalid texture channel source") };
                GL.TexParameter(target, (TextureParameterName)(0x8e42 + i), value);
            }
            GL.TexParameter(target, TextureParameterName.TextureMaxLevel, levels-1);
            return id;
        }
        catch { GL.DeleteTexture(id); throw; }
    }
}

