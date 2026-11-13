using System;
using System.Collections.Generic;
using System.Text;
using System.IO;

namespace BntxLibrary.IO
{
    internal static class StreamExtension
    {
        public static TemporarySeekHandle TemporarySeek(this Stream stream, long offset, SeekOrigin origin)
        {
            long ret = stream.Position;
            stream.Seek(offset, origin);
            return new TemporarySeekHandle(stream, ret);
        }

        public static TemporarySeekHandle TemporarySeek(this Stream stream)
        {
            long ret = stream.Position;
            return new TemporarySeekHandle(stream, ret);
        }

        public static bool EndOfStream(this Stream stream)
        {
            return stream.Position >= stream.Length;
        }
    }

    internal readonly ref struct TemporarySeekHandle
    {
        private readonly Stream Stream;
        private readonly long RetPos;

        public TemporarySeekHandle(Stream stream, long retpos)
        {
            this.Stream = stream;
            this.RetPos = retpos;
        }

        public readonly void Dispose()
        {
            Stream.Seek(RetPos, SeekOrigin.Begin);
        }
    }
}
