using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Text;
using System.Threading.Tasks;

namespace BntxLibrary.IO
{
    internal class FileWriter : BinaryWriter
    {
        /// <summary>
        /// The byte order of the file writer.
        /// </summary>
        public ByteOrder ByteOrder { get; set; } = ByteOrder.LittleEndian;

        /// <summary>
        /// Gets or sets the position of the file writer.
        /// </summary>
        public long Position
        {
            get => this.BaseStream.Position;
            set => this.BaseStream.Position = value;
        }

        public bool EndOfStream => this.BaseStream.EndOfStream();

        /// <summary>
        /// The default encoding for writing strings.
        /// </summary>
        public Encoding Encoding = Encoding.UTF8;

        public FileWriter(string filePath) : base(new FileStream(filePath, FileMode.Create, FileAccess.Write))
        {
        }
        public FileWriter(Stream input, bool leaveOpen = true) : base(input, Encoding.UTF8, leaveOpen)
        {
        }

        public void SetByteOrder(ushort v)
        {
            if (v == 0xFFFE)
                this.ByteOrder = ByteOrder.BigEndian;
            else
                this.ByteOrder = ByteOrder.LittleEndian;
        }

        public void SetByteOrder(bool isBigEndian)
        {
            this.ByteOrder = isBigEndian ? ByteOrder.BigEndian : ByteOrder.LittleEndian;
        }

        #region Signatures

        public void WriteSignature(string signature) => Write(Encoding.ASCII.GetBytes(signature));

        #endregion

        #region Offset/Sizes

        public void WriteSectionSizeU32(long position, long size)
        {
            using (TemporarySeek(position, System.IO.SeekOrigin.Begin))
            {
                Write((uint)(size));
            }
        }

        public void WriteSectionSizeU64(long position, long size)
        {
            using (TemporarySeek(position, System.IO.SeekOrigin.Begin))
            {
                Write((ulong)(size));
            }
        }

        public void WriteUint64Offset(long target, long relativePosition = 0)
        {
            long pos = Position;
            using (TemporarySeek(target, SeekOrigin.Begin))
            {
                Write((ulong)(pos - relativePosition));
            }
        }

        public void WriteUint32Offset(long target, long relativePosition = 0)
        {
            long pos = Position;
            using (TemporarySeek(target, SeekOrigin.Begin))
            {
                Write((uint)(pos - relativePosition));
            }
        } 

        public void WriteUint16Offset(long target, long relativePosition)
        {
            long pos = Position;
            using (TemporarySeek(target, SeekOrigin.Begin))
            {
                Write((ushort)(pos - relativePosition));
            }
        }

        #endregion

        #region Alignment

        /// <summary>
        /// Aligns the data by writing bytes (rather than seeking)
        /// </summary>
        /// <param name="alignment"></param>
        /// <param name="value"></param>
        public void Align(int alignment, byte value = 0x00)
        {
            var startPos = Position;
            long position = Seek((-Position % alignment + alignment) % alignment, SeekOrigin.Current);

            Seek(startPos, System.IO.SeekOrigin.Begin);
            while (Position != position)
            {
                Write(value);
            }
        }

        /// <summary>
        /// Aligns the data by writing bytes (rather than seeking)
        /// </summary>
        /// <param name="alignment"></param>
        /// <param name="value"></param>
        public void AlignBytesAtStartPos(long pos, int alignment, byte value = 0x00)
        {
            long currentPos = Position;
            long offsetFromStart = (currentPos - pos) % alignment;
            long paddingNeeded = (alignment - offsetFromStart) % alignment;

            while (paddingNeeded-- > 0)
            {
                Write(value);
            }
        }

        #endregion

        #region Structs

        public void WriteStruct<T>(T item) => Write(item.StructToBytes(ByteOrder == ByteOrder.BigEndian));

        public void WriteMultiStruct<T>(List<T> list)
        {
            foreach (T item in list)
                Write(item.StructToBytes(ByteOrder == ByteOrder.BigEndian));
        }

        #endregion

        #region Strings

        public void WriteZeroTerminatedString(string value, Encoding encoding_override = null)
        {
            var encoding = encoding_override ?? Encoding;
            Write(encoding.GetBytes(value));
            Write((byte)0);
        }

        public void WriteString(string value, Encoding encoding_override = null)
        {
            var encoding = encoding_override ?? Encoding;
            Write(encoding.GetBytes(value));
        }

        public void WriteFixedString(string value, int count)
        {
            var buffer = Encoding.UTF8.GetBytes(value);
            //clamp string
            if (buffer.Length > count)
            {
                buffer = buffer.AsSpan().Slice(0, count).ToArray();
                Console.WriteLine($"Warning! String {value} too long!");
            }

            Write(buffer);
            Write(new byte[count - buffer.Length]);
        }

        #endregion

        #region Seek

        public TemporarySeekHandle TemporarySeek(long offset, SeekOrigin origin)
            => this.BaseStream.TemporarySeek(offset, origin);

        public TemporarySeekHandle TemporarySeek()
    => this.BaseStream.TemporarySeek();

        public long Seek(long offset, SeekOrigin origin) => this.BaseStream.Seek(offset, origin);
        public long SeekBegin(long offset) => this.BaseStream.Seek(offset, SeekOrigin.Begin);

        #endregion

        #region Array Writing
        public void Write(sbyte[] value)
        {
            for (int i = 0; i < value.Length; i++)
                this.Write(value[i]);
        }
        public void Write(short[] value)
        {
            for (int i = 0; i < value.Length; i++)
                this.Write(value[i]);
        }
        public void Write(ushort[] value)
        {
            for (int i = 0; i < value.Length; i++)
                this.Write(value[i]);
        }
        public void Write(uint[] value)
        {
            for (int i = 0; i < value.Length; i++)
                this.Write(value[i]);
        }
        public void Write(int[] value)
        {
            for (int i = 0; i < value.Length; i++)
                this.Write(value[i]);
        }
        public void Write(float[] value)
        {
            for (int i = 0; i < value.Length; i++)
                this.Write(value[i]);
        }
        public void Write(double[] value)
        {
            for (int i = 0; i < value.Length; i++)
                this.Write(value[i]);
        }
        public void Write(ulong[] value)
        {
            for (int i = 0; i < value.Length; i++)
                this.Write(value[i]);
        }
        public void Write(long[] value)
        {
            for (int i = 0; i < value.Length; i++)
                this.Write(value[i]);
        }
        #endregion

        #region Standard Writing

        public override void Write(short value)
        {
            if (ByteOrder == ByteOrder.LittleEndian)
                base.Write(value);
            else
                WriteReversed(BitConverter.GetBytes(value));
        }

        public override void Write(ushort value)
        {
            if (ByteOrder == ByteOrder.LittleEndian)
                base.Write(value);
            else
                WriteReversed(BitConverter.GetBytes(value));
        }

        public override void Write(int value)
        {
            if (ByteOrder == ByteOrder.LittleEndian)
                base.Write(value);
            else
                WriteReversed(BitConverter.GetBytes(value));
        }

        public override void Write(uint value)
        {
            if (ByteOrder == ByteOrder.LittleEndian)
                base.Write(value);
            else
                WriteReversed(BitConverter.GetBytes(value));
        }

        public override void Write(long value)
        {
            if (ByteOrder == ByteOrder.LittleEndian)
                base.Write(value);
            else
                WriteReversed(BitConverter.GetBytes(value));
        }

        public override void Write(ulong value)
        {
            if (ByteOrder == ByteOrder.LittleEndian)
                base.Write(value);
            else
                WriteReversed(BitConverter.GetBytes(value));
        }

        public override void Write(float value)
        {
            if (ByteOrder == ByteOrder.LittleEndian)
                base.Write(value);
            else
                WriteReversed(BitConverter.GetBytes(value));
        }

        public override void Write(double value)
        {
            if (ByteOrder == ByteOrder.LittleEndian)
                base.Write(value);
            else
                WriteReversed(BitConverter.GetBytes(value));
        }

        private void WriteReversed(byte[] bytes)
        {
            Array.Reverse(bytes);
            base.Write(bytes);
        }

        #endregion

        #region Conversion

        private static int SingleToInt10(float value)
        {
            if (value < -512 || value > 511)
            {
                throw new ArgumentException($"{value} cannot be converted to Int10 (exceeds range -512 to 511).",
                    nameof(value));
            }
            return (int)(((uint)value << 22) >> 22) & 0b00000000_00000000_00000011_11111111;
        }

        private static int SingleToInt2(float value)
        {
            if (value < -1 || value > 1)
            {
                throw new ArgumentException($"{value} cannot be converted to Int2 (exceeds range -1 to 1).",
                    nameof(value));
            }
            return (int)(((uint)value << 30) >> 30) & 0b00000000_00000000_00000000_00000011;
        }

        #endregion
    }
}
