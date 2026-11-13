using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace BntxLibrary.IO
{
    internal class FileReader : BinaryReader
    {
        /// <summary>
        /// The byte order of the file reader.
        /// </summary>
        public ByteOrder ByteOrder { get; set; } = ByteOrder.LittleEndian;

        /// <summary>
        /// Gets or sets the position of the file reader.
        /// </summary>
        public long Position
        {
            get => this.BaseStream.Position;
            set => this.BaseStream.Position = value;
        }

        public bool EndOfStream => this.BaseStream.EndOfStream();

        /// <summary>
        /// The default encoding for reading strings.
        /// </summary>
        public Encoding Encoding = Encoding.UTF8;

        public FileReader(string filePath, bool leaveOpen = false) : base(
            new FileStream(filePath, FileMode.Open, FileAccess.Read), Encoding.UTF8, leaveOpen)
        {

        }
        public FileReader(Stream input, bool leaveOpen = false) : base(input, Encoding.UTF8, leaveOpen)
        {
        }
        public FileReader(byte[] input) : base(new MemoryStream(input), Encoding.UTF8)
        {
        }
        public FileReader(Stream input, Encoding encoding, bool leaveOpen = false) : base(input, encoding, leaveOpen)
        {
            this.Encoding = encoding;
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
        public void SetByteOrder(ByteOrder bom)
        {
            this.ByteOrder = bom;
        }

        public string ReadSignature(string expected_magic)
        {
            string magic = Encoding.ASCII.GetString(ReadBytes(expected_magic.Length));
            if (expected_magic != magic)
                throw new Exception($"Expected {expected_magic} but got {magic} instead.");

            return magic;
        }

        public void CheckSignature(string magic, string expectedMagic)
        {
            if (expectedMagic != magic)
                throw new Exception($"Expected {expectedMagic} but got {magic} instead.");
        }
        public void CheckSignature(string magic, string[] expectedMagic)
        {
            if (!expectedMagic.Contains(magic))
                throw new Exception($"Expected either magics {string.Join(",", expectedMagic)} but got {magic} instead.");
        }

        #region Align

        public void Align(int alignment)
        {
            var startPos = Position;
            long position = Seek((-Position % alignment + alignment) % alignment, SeekOrigin.Current);
            if (startPos >= this.BaseStream.Length)
                return;

            Seek(startPos, System.IO.SeekOrigin.Begin);
            while (Position != position)
            {
                ReadByte();
            }
        }

        #endregion

        #region Strings

        public string ReadStringZeroTerminated(Encoding encoding = null)
        {
            List<byte> values = new List<byte>();
            while (this.BaseStream.Position < this.BaseStream.Length)
            {
                byte v = this.ReadByte();
                if (v == 0)
                    break;

                values.Add(v);
            }
            return (encoding ?? Encoding).GetString(values.ToArray());
        }

        #endregion

        #region Seeking

        public long SeekBegin(long offset) => this.BaseStream.Seek(offset, SeekOrigin.Begin);
        public long Seek(long offset, SeekOrigin seekOrigin) => this.BaseStream.Seek(offset, seekOrigin);
        public TemporarySeekHandle TemporarySeek(long offset, SeekOrigin origin)
            => this.BaseStream.TemporarySeek(offset, origin);

        #endregion

        #region Structs

        public static Span<byte> AsSpan<T>(ref T val) where T : unmanaged
        {
            Span<T> valSpan = MemoryMarshal.CreateSpan(ref val, 1);
            return MemoryMarshal.Cast<T, byte>(valSpan);
        }

        //From kuriimu https://github.com/IcySon55/Kuriimu/blob/master/src/Kontract/IO/BinaryReaderX.cs#L40
        public T ReadStruct<T>() => ReadBytes(Marshal.SizeOf<T>()).BytesToStruct<T>(ByteOrder == ByteOrder.BigEndian);
        public List<T> ReadMultipleStructs<T>(int count) => Enumerable.Range(0, count).Select(_ => ReadStruct<T>()).ToList();
        public List<T> ReadMultipleStructs<T>(uint count) => Enumerable.Range(0, (int)count).Select(_ => ReadStruct<T>()).ToList();

        #endregion

        #region Array Reading

        public sbyte[] ReadSBytes(int count)
        {
            sbyte[] values = new sbyte[count];
            for (int i = 0; i < count; i++)
                values[i] = this.ReadSByte();
            return values;
        }

        public bool[] ReadBooleans(int count)
        {
            bool[] values = new bool[count];
            for (int i = 0; i < count; i++)
                values[i] = this.ReadBoolean();
            return values;
        }

        public short[] ReadInt16s(int count)
        {
            short[] result = new short[count];
            for (int i = 0; i < count; i++)
                result[i] = this.ReadInt16();
            return result;
        }
        public ushort[] ReadUInt16s(int count)
        {
            ushort[] result = new ushort[count];
            for (int i = 0; i < count; i++)
                result[i] = this.ReadUInt16();
            return result;
        }
        public uint[] ReadUInt32s(int count)
        {
            uint[] result = new uint[count];
            for (int i = 0; i < count; i++)
                result[i] = this.ReadUInt32();
            return result;
        }
        public int[] ReadInt32s(int count)
        {
            int[] result = new int[count];
            for (int i = 0; i < count; i++)
                result[i] = this.ReadInt32();
            return result;
        }
        public ulong[] ReadUInt64s(int count)
        {
            ulong[] result = new ulong[count];
            for (int i = 0; i < count; i++)
                result[i] = this.ReadUInt64();
            return result;
        }
        public long[] ReadInt64s(int count)
        {
            long[] result = new long[count];
            for (int i = 0; i < count; i++)
                result[i] = this.ReadInt64();
            return result;
        }
        public float[] ReadSingles(int count)
        {
            float[] result = new float[count];
            for (int i = 0; i < count; i++)
                result[i] = this.ReadSingle();
            return result;
        }
        public decimal[] ReadDecimals(int count)
        {
            decimal[] result = new decimal[count];
            for (int i = 0; i < count; i++)
                result[i] = this.ReadDecimal();
            return result;
        }
        public double[] ReadDoubles(int count)
        {
            double[] result = new double[count];
            for (int i = 0; i < count; i++)
                result[i] = this.ReadDouble();
            return result;
        }
        #endregion

        #region Standard Reading

        public override short ReadInt16()
        {
            return ByteOrder == ByteOrder.LittleEndian
                ? base.ReadInt16()
                : BitConverter.ToInt16(ReadReversedBytes(2), 0);
        }

        public override ushort ReadUInt16()
        {
            return ByteOrder == ByteOrder.LittleEndian
                ? base.ReadUInt16()
                : BitConverter.ToUInt16(ReadReversedBytes(2), 0);
        }

        public override int ReadInt32()
        {
            return ByteOrder == ByteOrder.LittleEndian
                ? base.ReadInt32()
                : BitConverter.ToInt32(ReadReversedBytes(4), 0);
        }

        public override uint ReadUInt32()
        {
            return ByteOrder == ByteOrder.LittleEndian
                ? base.ReadUInt32()
                : BitConverter.ToUInt32(ReadReversedBytes(4), 0);
        }

        public uint ReadUInt24()
        {
            var bytes = base.ReadBytes(3);
            if (ByteOrder == ByteOrder.LittleEndian)
                return (uint)(bytes[0] | (bytes[1] << 8) | (bytes[2] << 16));
            else
                return (uint)(bytes[2] | (bytes[1] << 8) | (bytes[0] << 16));
        }

        public override long ReadInt64()
        {
            return ByteOrder == ByteOrder.LittleEndian
                ? base.ReadInt64()
                : BitConverter.ToInt64(ReadReversedBytes(8), 0);
        }

        public override ulong ReadUInt64()
        {
            return ByteOrder == ByteOrder.LittleEndian
                ? base.ReadUInt64()
                : BitConverter.ToUInt64(ReadReversedBytes(8), 0);
        }

        public override float ReadSingle()
        {
            return ByteOrder == ByteOrder.LittleEndian
                ? base.ReadSingle()
                : BitConverter.ToSingle(ReadReversedBytes(4), 0);
        }

        public override double ReadDouble()
        {
            return ByteOrder == ByteOrder.LittleEndian
                ? base.ReadDouble()
                : BitConverter.ToDouble(ReadReversedBytes(8), 0);
        }

        private byte[] ReadReversedBytes(int count)
        {
            var data = base.ReadBytes(count);
            Array.Reverse(data);
            return data;
        }

        #endregion
    }
}
