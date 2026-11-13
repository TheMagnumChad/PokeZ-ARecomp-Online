using System.Buffers.Binary;
using System.Text;
namespace Trinity.Core.Utils;

// Original bounded reader for fixed visibility tracks from player-owned TRACM.
// No asset values or species names are embedded. Unsupported dynamic tracks are
// rejected explicitly: caller must warn and preserve default visibility.
public static class StaticVisibility {
    const int MaxBytes = 16 * 1024 * 1024;
    public static IReadOnlyDictionary<string, bool> Read(string path) {
        using var stream = File.OpenRead(path);
        if (stream.Length > MaxBytes) throw new InvalidDataException("TRACM exceeds limit");
        byte[] data = new byte[checked((int)stream.Length)];
        stream.ReadExactly(data);
        return Read(data);
    }
    public static IReadOnlyDictionary<string, bool> Read(byte[] data) {
        if (data.Length > MaxBytes) throw new InvalidDataException("TRACM exceeds limit");
        var b = new Reader(data);
        int root = b.Offset(0);
        int tracks = b.Field(root, 1);
        var result = new Dictionary<string, bool>(StringComparer.Ordinal);
        if (tracks == 0) return result;
        var (start, count) = b.Vector(tracks, 4, 4096);
        for (int i = 0; i < count; i++) {
            int track = b.Offset(start + i * 4);
            int vis = b.Field(track, 5);
            if (vis == 0) continue;
            int timeline = b.Offset(vis);
            int infoField = b.Field(timeline, 2);
            if (infoField == 0) continue;
            int info = b.Offset(infoField);
            int kindField = b.Field(info, 0);
            int kind = kindField == 0 ? 0 : b.Byte(kindField);
            if (kind == 0) continue;
            string name = b.String(b.Field(track, 0));
            if (kind != 1) throw new NotSupportedException($"Dynamic TRACM visibility kind {kind} for {name}");
            int valueField = b.Field(info, 1);
            if (valueField == 0) throw new InvalidDataException("Missing fixed visibility payload");
            int value = b.Field(b.Offset(valueField), 0);
            int raw = value == 0 ? 0 : b.Byte(value);
            if (raw > 1) throw new InvalidDataException("Invalid visibility bool");
            if (!result.TryAdd(name, raw != 0)) throw new InvalidDataException("Duplicate visibility track");
        }
        return result;
    }
    sealed class Reader(byte[] data) {
        void Check(long p, long size) {
            if (p < 0 || size < 0 || p > data.Length || size > data.Length - p)
                throw new InvalidDataException("TRACM range invalid");
        }
        public int Byte(int p) { Check(p, 1); return data[p]; }
        int U16(int p) { Check(p, 2); return BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(p, 2)); }
        int I32(int p) { Check(p, 4); return BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(p, 4)); }
        uint U32(int p) { Check(p, 4); return BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(p, 4)); }
        public int Offset(int p) {
            long q = (long)p + U32(p); Check(q, 4); return (int)q;
        }
        public int Field(int table, int slot) {
            long vt = (long)table - I32(table); Check(vt, 4);
            int length = U16((int)vt), size = U16((int)vt + 2);
            if (length < 4 || length > 132 || (length & 1) != 0 || size < 4)
                throw new InvalidDataException("TRACM table invalid");
            Check(vt, length); Check(table, size);
            if (slot >= (length - 4) / 2) return 0;
            int off = U16((int)vt + 4 + slot * 2);
            if (off == 0) return 0;
            if (off < 4 || off >= size) throw new InvalidDataException("TRACM field invalid");
            return table + off;
        }
        public (int start, int count) Vector(int p, int width, int limit) {
            int v = Offset(p); uint count = U32(v);
            if (count > limit) throw new InvalidDataException("TRACM vector limit exceeded");
            Check((long)v + 4, (long)count * width); return (v + 4, (int)count);
        }
        public string String(int p) {
            if (p == 0) throw new InvalidDataException("TRACM track name missing");
            var (start, count) = Vector(p, 1, 1024);
            if (count == 0) throw new InvalidDataException("TRACM track name empty");
            try { return new UTF8Encoding(false, true).GetString(data, start, count); }
            catch (DecoderFallbackException e) { throw new InvalidDataException("TRACM track name invalid", e); }
        }
    }
}
