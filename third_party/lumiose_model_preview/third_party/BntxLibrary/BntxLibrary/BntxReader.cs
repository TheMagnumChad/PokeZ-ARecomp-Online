using BntxLibrary.IO;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace BntxLibrary
{
    internal class BntxReader
    {
        public static void Read(BntxFile bntx, FileReader reader)
        {
            // Peek for byte order mark
            using (reader.TemporarySeek(12, SeekOrigin.Begin)) {
                reader.ByteOrder = ByteOrder.BigEndian;
                reader.SetByteOrder((ByteOrder)reader.ReadUInt16());
            }

            bntx.BinHeader = reader.ReadStruct<BinaryHeader>();
            reader.CheckSignature(bntx.BinHeader.Magic, "BNTX");

            using (reader.TemporarySeek(bntx.BinHeader.RelocationTableOffset, SeekOrigin.Begin)) {
                bntx._relocationTable = ReadRelocationTable(reader);
            }

            bntx.BntxHeader = reader.ReadStruct<BntxHeader>();
            // Switch 1, Switch 2, PC
            reader.CheckSignature(bntx.BntxHeader.Magic, new[] { "NX  ", "Ounc", "PC  " });
            switch (bntx.BntxHeader.Magic)
            {
                case "PC  ": bntx.Platform = PlatformKind.PC; break;
                case "NX  ": bntx.Platform = PlatformKind.NX; break;
                case "Ounc": bntx.Platform = PlatformKind.Ounce; break;
            }
            // Name (not relative to length so subtract 2)
            bntx.Name = ReadNameOffset(bntx.BinHeader.NameOffset - 2, reader, bntx);

            reader.SeekBegin((long)bntx.BntxHeader.TextureDictionaryPointer);
            var textureDict = ReadDictionary(reader, bntx);

            reader.SeekBegin((long)bntx.BntxHeader.TextureInfoPointer);
            ulong[] offsets = reader.ReadUInt64s((int)bntx.BntxHeader.TextureCount);
            for (int i  = 0; i < offsets.Length; i++)
            {
                reader.SeekBegin((long)offsets[i]);
                var tex = new BntxTexture();

                tex.Header = reader.ReadStruct<ResBntxTextureInfo>();
                reader.CheckSignature(tex.Header.ResHeader.Magic, "BRTI");
                // Name offset
                tex.Name = ReadNameOffset(tex.Header.NameOffset, reader, bntx);
                // Data offset
                if (tex.Header.DataPointersOffset != 0) {
                    using (reader.TemporarySeek((long)tex.Header.DataPointersOffset, SeekOrigin.Begin)) {
                        var mipOffsets = reader.ReadUInt64s((int)tex.Header.MipCount);
                        // Points to data directly from BRTD buffer
                        reader.SeekBegin((long)mipOffsets[0]);
                        tex.ImageData = reader.ReadBytes((int)tex.Header.ImageSize);
                        // Set mip offsets relative to first
                        tex.MipOffsets = new ulong[mipOffsets.Length];
                        for (int j = 0; j < mipOffsets.Length; j++)
                            tex.MipOffsets[j] = mipOffsets[j] - mipOffsets[0];
                    }
                }
                // User data
                if (tex.Header.UserDataDictionaryOffset != 0) {
                    using (reader.TemporarySeek((long)tex.Header.UserDataDictionaryOffset, SeekOrigin.Begin)) {
                        var dict = ReadDictionary(reader, bntx);
                        reader.SeekBegin((long)tex.Header.UserDataOffset);
                        for (int j = 0; j < dict.Count; j++)
                            tex.UserData.Add(dict[j], ReadUserData(reader, bntx));
                    }
                }
                bntx.Textures.Add(textureDict[i], tex);
            }
            bntx._hashSize = bntx.CalculateSizeHash();
            bntx._hash = bntx.CalculateHash();
        }

        static RelocationTable ReadRelocationTable(FileReader reader)
        {
            reader.ReadSignature("_RLT");
            uint ofs = reader.ReadUInt32();
            uint sectionCount = reader.ReadUInt32();
            uint padding = reader.ReadUInt32();
            // 288
            RelocationTable table = new();
            table.Sections = reader.ReadMultipleStructs<RelocationTable.Section>(sectionCount);
            table.Entries = reader.ReadMultipleStructs<RelocationTable.Entry>(table.Sections.Sum(x => x.EntryCount));
            return table;
        }

        static UserData ReadUserData(FileReader reader, BntxFile bntx)
        {
            var usd = new UserData();
            var header = reader.ReadStruct<UserDataHeader>();
            usd.Name = ReadNameOffset(header.NameOffset, reader, bntx);

            if (header.DataOffset  != 0) {
                using (reader.TemporarySeek((long)header.DataOffset, SeekOrigin.Begin)) {
                    switch (header.Type)
                    {
                        case UserDataType.String:
                            {
                                List<string> values = new List<string>();
                                for (int i = 0; i < header.Count; i++)
                                    values.Add(ReadNameOffset(reader, bntx, Encoding.UTF8));
                                usd.Value = values.ToArray();
                            }
                            break;
                        case UserDataType.WString:
                            {
                                List<string> values = new List<string>();
                                for (int i = 0; i < header.Count; i++)
                                    values.Add(ReadNameOffset(reader, bntx, Encoding.Unicode));
                                usd.Value = values.ToArray();
                            }
                            break;
                        case UserDataType.Int32:
                            {
                                List<int> values = new List<int>();
                                for (int i = 0; i < header.Count; i++)
                                    values.Add(reader.ReadInt32());
                                usd.Value = values.ToArray();
                            }
                            break;
                        case UserDataType.Single:
                            {
                                List<float> values = new List<float>();
                                for (int i = 0; i < header.Count; i++)
                                    values.Add(reader.ReadSingle());
                                usd.Value = values.ToArray();
                            }
                            break;
                        case UserDataType.Byte:
                            {
                                List<byte> values = new List<byte>();
                                for (int i = 0; i < header.Count; i++)
                                    values.Add(reader.ReadByte());
                                usd.Value = values.ToArray();
                            }
                            break;
                    }
                }
            }
            return usd;
        }

        // Currently just gather strings
        static List<string> ReadDictionary(FileReader reader, BntxFile bntx)
        {
            List<string> strings = new List<string>();

            reader.ReadSignature("_DIC");
            uint count = reader.ReadUInt32();
            for (int i = 0; i < count + 1; i++)
            {
                uint key = reader.ReadUInt32();
                ushort leftIdx = reader.ReadUInt16();
                ushort rightIdx = reader.ReadUInt16();
                ulong nameOffset = reader.ReadUInt64();
                if (i == 0) // Root with no values
                    continue;

                strings.Add(ReadNameOffset(nameOffset, reader, bntx));
            }
            return strings;
        }

        static string ReadNameOffset(FileReader reader, BntxFile bntx, Encoding encoding = null) {
            return ReadNameOffset(reader.ReadUInt64(), reader, bntx);
        }

        static string ReadNameOffset(ulong nameOffset, FileReader reader, BntxFile bntx, Encoding encoding = null)
        {
            if (nameOffset == 0) return "";

            encoding = encoding ?? Encoding.UTF8;

            using (reader.TemporarySeek((long)nameOffset, SeekOrigin.Begin)) {
                ushort length = reader.ReadUInt16();
                string value = reader.ReadStringZeroTerminated(encoding);
                // Store string into a table so we can ensure original order
                // Keep original order for 1 to 1 recreation incase sizes need to stay the same
                bntx.Strings.TryAdd((long)nameOffset, value);
                return value;
            }
        }
    }
}
