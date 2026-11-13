using BntxLibrary.IO;
using BntxLibrary.Util;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace BntxLibrary
{
    public class BntxFile
    {
        public BinaryHeader BinHeader;
        public BntxHeader BntxHeader;
        internal RelocationTable _relocationTable = new();
        internal SortedDictionary<long, string> Strings = new SortedDictionary<long, string>();

        internal uint _hashSize;
        internal uint _hash;

        public bool HasFileSizeChanges
            => _hashSize != CalculateSizeHash();

        public bool HasAnyChanges
            => _hash != CalculateHash();

        /// <summary>
        /// The platform value.
        /// </summary>
        [Category("Header")]
        [DisplayName("Platform")]
        public PlatformKind Platform { get; set; } = PlatformKind.NX;

        /// <summary>
        /// The internal file name.
        /// </summary>
        [Category("Header")]
        [DisplayName("Name")]
        public string Name { get; set; }

        /// <summary>
        /// The data alignment.
        /// </summary>
        [Category("Header")]
        [DisplayName("Alignment")]
        public int Alignment
        {
            get => 1 << (int)BinHeader.Alignment;
            set
            {
                // Ensure value is a power of two
                if (value <= 0 || (value & (value - 1)) != 0)
                    throw new ArgumentException("Alignment must be a power of two.", nameof(value));
                BinHeader.Alignment = (byte)Math.Log(value, 2);
            }
        }

        /// <summary>
        /// Gets the version number of the binary.
        /// </summary>
        [Category("Versions")]
        [ReadOnly(true)]
        [DisplayName("Version Full")]
        public string VersionFull
        {
            get => string.Format("{0}.{1}.{2}",
                BinHeader.VersionMajor, 
                BinHeader.VersionMinor,
                BinHeader.VersionMicro);
        }

        [Category("Versions")]
        [DisplayName("Version Major")]
        public ushort VersionMajor
        {
            get => BinHeader.VersionMajor;
            set => BinHeader.VersionMajor = value;
        }

        [Category("Versions")]
        [DisplayName("Version Micro")]
        public byte VersionMicro
        {
            get => BinHeader.VersionMicro;
            set => BinHeader.VersionMicro = value;
        }

        [Category("Versions")]
        [DisplayName("Version Minor")]
        public byte VersionMinor
        {
            get => BinHeader.VersionMinor;
            set => BinHeader.VersionMinor = value;
        }

        /// <summary>
        /// Gets or sets the texture lookup.
        /// </summary>
        public Dictionary<string, BntxTexture> Textures { get; set; } = [];

        public BntxFile(string filename)
            => BntxReader.Read(this, new FileReader(filename));

        public BntxFile(Stream stream)
            => BntxReader.Read(this, new FileReader(stream));

        public void Save(string filename)
            => BntxWriter.Write(this, new FileWriter(filename));
        public void Save(Stream stream)
            => BntxWriter.Write(this, new FileWriter(stream));

        internal uint CalculateSizeHash() // Here we make a hash for detecting file size changes
        {
            // If names or data length changes, expect file size changes
            var nameLength = this.Textures.Values.Sum(x => x.Name.Length +
                  x.UserData.Sum(x => x.Value.Name.Length));

            var dataLength = this.Textures.Values.Sum(x =>
                x.ImageData.Length);

            var usedLength = this.Textures.Values.Sum(x =>
                x.UserData.Sum(x => x.Value.CalculateDataSize()));

            uint hash = 0;
            hash ^= HashHelper.ComputeHash32(this.Name.Length);
            hash ^= HashHelper.ComputeHash32(nameLength + this.Name.Length);
            hash ^= HashHelper.ComputeHash32(dataLength);
            hash ^= HashHelper.ComputeHash32(usedLength);
            hash ^= HashHelper.ComputeHash32(this.Alignment);
            return hash;
        }

        internal uint CalculateHash()
        {
            // Calculate hashes of the structs for any possible changes
            uint hash = 0;

            hash ^= HashHelper.ComputeHash32(this.BinHeader);
            hash ^= HashHelper.ComputeHash32(this.BntxHeader);
            hash ^= HashHelper.ComputeHash32(this.Name);
            hash ^= HashHelper.ComputeHash32((uint)this.Platform);
            foreach (var texture in this.Textures.Values)
            {
                hash ^= HashHelper.ComputeHash32(texture.Name);
                hash ^= HashHelper.ComputeHash32(texture.Header);
                hash ^= HashHelper.ComputeHash32(texture.ImageData.ToArray()); // Lumiose: hash bytes, not the Memory<T> wrapper.
                foreach (var usd in texture.UserData.Values)
                {
                    hash ^= HashHelper.ComputeHash32(usd.Name);
                    if (usd.Type == UserDataType.String || usd.Type == UserDataType.WString)
                    {
                        foreach (var s in usd.GetValueArray<string>())
                            hash ^= HashHelper.ComputeHash32(s);
                    }
                    else if (usd.Type == UserDataType.Single)
                    {
                        foreach (var v in usd.GetValueArray<float>())
                            hash ^= HashHelper.ComputeHash32(BitConverter.GetBytes(v));
                    }
                    else if (usd.Type == UserDataType.Int32)
                    {
                        foreach (var v in usd.GetValueArray<int>())
                            hash ^= HashHelper.ComputeHash32(BitConverter.GetBytes(v));
                    }
                    else if (usd.Type == UserDataType.Byte)
                        hash ^= HashHelper.ComputeHash32(usd.GetValueArray<byte>());
                }
            }
            return hash;
        }
    }

    public class BntxTexture : INotifyPropertyChanged
    {
        /// <summary>
        /// The texture header.
        /// </summary>
        public ResBntxTextureInfo Header;

        /// <summary>
        /// Gets or sets the image width.
        /// </summary>
        [ReadOnly(true)]
        [Description("Width of the image")]
        [Category("Image Info")]
        [DisplayName("Width")]
        public uint Width
        {
            get => Header.Width;
            set => Header.Width = value;
        }

        /// <summary>
        /// Gets or sets the image height.
        /// </summary>
        [ReadOnly(true)]
        [Description("Height of the image")]
        [Category("Image Info")]
        [DisplayName("Height")]
        public uint Height
        {
            get => Header.Height;
            set => Header.Height = value;
        }

        /// <summary>
        /// Gets or sets the image mip count.
        /// </summary>
        [ReadOnly(true)]
        [Description("Mip count of the image")]
        [Category("Image Info")]
        [DisplayName("Mip Count")]
        public ushort MipCount
        {
            get => Header.MipCount;
            set => Header.MipCount = value;
        }

        /// <summary>
        /// Gets or sets the image array layer count.
        /// </summary>
        [ReadOnly(true)]
        [Description("Array count of the image")]
        [Category("Image Info")]
        [DisplayName("Array Count")]
        public uint ArrayCount
        {
            get => Header.ArrayCount;
            set => Header.ArrayCount = value;
        }

        /// <summary>
        /// Gets or sets the image array layer count.
        /// </summary>
        [ReadOnly(true)]
        [Description("Depth of the image")]
        [Category("Image Info")]
        [DisplayName("Depth")]
        public uint Depth
        {
            get => Header.Depth;
            set => Header.Depth = value;
        }

        /// <summary>
        /// Gets or sets the image alignment.
        /// </summary>
        [ReadOnly(true)]
        [Description("Alignment")]
        [Category("Swizzle")]
        [DisplayName("Alignment")]
        public uint Alignment
        {
            get => Header.Alignment;
            set => Header.Alignment = value;
        }

        /// <summary>
        /// Gets or sets the image format.
        /// </summary>
        [ReadOnly(true)]
        [Description("The image format.")]
        [Category("Image Info")]
        [DisplayName("Image Format")]
        public GfxImageFormat ImageFormat
        {
            get => Header.ImageFormat;
            set => Header.ImageFormat = value;
        }

        /// <summary>
        /// Gets or sets the image dim.
        /// </summary>
        [Description("The image dimensions.")]
        [Category("Image Info")]
        [DisplayName("Dim")]
        public GfxImageStorageDimension Dim
        {
            get => Header.Dim;
            set => Header.Dim = value;
        }

        /// <summary>
        /// Gets or sets the gpu access flags.
        /// </summary>
        [Description("The gpu access flags.")]
        [Category("Image Info")]
        [DisplayName("Gpu Access Flags")]
        public GfxGpuAccessFlags GpuAccessFlags
        {
            get => Header.GpuAccessFlags;
            set => Header.GpuAccessFlags = value;
        }

        /// <summary>
        /// Gets or sets the tile mode.
        /// </summary>
        [Description("The tiling mode.")]
        [Category("Swizzle")]
        [DisplayName("Tile Mode")]
        public GfxTileMode TileMode
        {
            get => Header.TileMode;
            set => Header.TileMode = value;
        }

        /// <summary>
        /// Gets or sets the swizzle value.
        /// </summary>
        [Description("The swizzle value.")]
        [Category("Swizzle")]
        [DisplayName("Swizzle")]
        public ushort Swizzle
        {
            get => Header.Swizzle;
            set => Header.Swizzle = value;
        }

        /// <summary>
        /// Gets or sets the sample count used for multi sampled types.
        /// </summary>
        [Description("Sample count of the image")]
        [Category("Image Info")]
        [DisplayName("Sample Count")]
        public uint SampleCount
        {
            get => Header.SampleCount;
            set => Header.SampleCount = value;
        }

        /// <summary>
        /// Gets or sets the flags.
        /// </summary>
        [Description("Flags")]
        [Category("Image Info")]
        [DisplayName("Flags")]
        public ResGfxTextureInfoFlags Flags
        {
            get => Header.Flags;
            set => Header.Flags = value;
        }

        /// <summary>
        /// Gets or sets the red channel source.
        /// </summary>
        [Description("The source channel to map to the R (red) channel.")]
        [Category("Channels")]
        [DisplayName("Red Channel")]
        public GfxChannelSource GfxChannelSourceR
        {
            get => Header.GfxChannelSourceR;
            set
            {
                Header.GfxChannelSourceR = value;
                OnPropertyChanged(nameof(GfxChannelSourceR));
            }
        }

        /// <summary>
        /// Gets or sets the green channel source.
        /// </summary>
        [Description("The source channel to map to the G (green) channel.")]
        [Category("Channels")]
        [DisplayName("Green Channel")]
        public GfxChannelSource GfxChannelSourceG
        {
            get => Header.GfxChannelSourceG;
            set
            {
                Header.GfxChannelSourceG = value;
                OnPropertyChanged(nameof(GfxChannelSourceG));
            }
        }

        /// <summary>
        /// Gets or sets the blue channel source.
        /// </summary>
        [Description("The source channel to map to the B (blue) channel.")]
        [Category("Channels")]
        [DisplayName("Blue Channel")]
        public GfxChannelSource GfxChannelSourceB
        {
            get => Header.GfxChannelSourceB;
            set
            {
                Header.GfxChannelSourceB = value;
                OnPropertyChanged(nameof(GfxChannelSourceB));
            }
        }

        /// <summary>
        /// Gets or sets the alpha channel source.
        /// </summary>
        [Description("The source channel to map to the A (alpha) channel.")]
        [Category("Channels")]
        [DisplayName("Alpha Channel")]
        public GfxChannelSource GfxChannelSourceA
        {
            get => Header.GfxChannelSourceA;
            set
            {
                Header.GfxChannelSourceA = value;
                OnPropertyChanged(nameof(GfxChannelSourceA));
            }
        }

        /// <summary>
        /// Gets or sets the name of the texture.
        /// </summary>
        [Description("Name")]
        [Category("Image Info")]
        [DisplayName("Name")]
        public string Name { get; set; }

        /// <summary>
        /// Gets or sets the mip offsets relative to the image data.
        /// </summary>
        public ulong[] MipOffsets { get; set; }

        /// <summary>
        /// Gets or sets user data of the texture.
        /// </summary>
        public Dictionary<string, UserData> UserData { get; set; } = [];

        /// <summary>
        /// The image data.
        /// </summary>
        public Memory<byte> ImageData;

        public uint BlockHeightLog2
        {
            get
            {
                return this.Header.TextureLayout & 7;
            }
        }

        public event PropertyChangedEventHandler? PropertyChanged;
        protected void OnPropertyChanged(string propertyName) =>
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    }

    public class UserData
    {
        // Raw header struct
        private UserDataHeader Header;

        /// <summary>
        /// Gets or sets the name of the user data.
        /// </summary>
        public string Name { get; set; }

        /// <summary>
        /// Gets the raw value of the user data entry.
        /// </summary>
        public object Value { get; internal set; }

        /// <summary>
        /// The type of the user data.
        /// </summary>
        public UserDataType Type => Header.Type;

        /// <summary>
        /// Gets the user data entry array.
        /// </summary>
        /// <typeparam name="T"></typeparam>
        /// <returns></returns>
        public T[] GetValueArray<T>() => (T[])Value;

        /// <summary>
        /// Sets an array of strings as unicode.
        /// </summary>
        /// <param name="values"></param>
        public void SetValueWString(params string[] values) 
            => SetValueByType(values, UserDataType.WString);

        /// <summary>
        /// Sets an array of strings as utf8.
        /// </summary>
        /// <param name="values"></param>
        public void SetValue(params string[] values) 
            => SetValueByType(values, UserDataType.String);

        /// <summary>
        /// Sets an array of floats.
        /// </summary>
        /// <param name="values"></param>
        public void SetValue(params float[] values) 
            => SetValueByType(values, UserDataType.Single);

        /// <summary>
        /// Sets an array of int32s.
        /// </summary>
        /// <param name="values"></param>
        public void SetValue(params int[] values) 
            => SetValueByType(values, UserDataType.Int32);

        /// <summary>
        /// Sets an array of bytes.
        /// </summary>
        /// <param name="values"></param>
        public void SetValue(params byte[] values) 
            => SetValueByType(values, UserDataType.Byte);

        private void SetValueByType<T>(T[] values, UserDataType type)
        {
            Header.Type = type;
            Header.Count = (uint)values.Length;
            Value = values;
        }

        internal uint CalculateDataSize()
        {
            switch (this.Type)
            {
                case UserDataType.String: return (sizeof(byte) * Header.Count) + 1;
                case UserDataType.WString: return (sizeof(ushort) * Header.Count) + 1;
                case UserDataType.Single: return sizeof(float) * Header.Count;
                case UserDataType.Int32: return sizeof(int) * Header.Count;
                case UserDataType.Byte: return sizeof(byte) * Header.Count;
            }
            return 0;
        }

        public UserData() {
            Header = new UserDataHeader();
        }   
    }

    [StructLayout(LayoutKind.Sequential, Size = 0x10)]
    public struct BinaryHeader //A header shared between bntx and other formats
    {
        internal Magic Magic; // BNTX
        internal Magic MagicPad; // padding from magic

        public byte VersionMicro;
        public byte VersionMinor;
        public ushort VersionMajor;

        public ushort ByteOrder;
        public byte Alignment;
        public byte TargetAddressSize;
        public uint NameOffset;
        public ushort Flag;
        public ushort BlockOffset;
        public uint RelocationTableOffset;
        public uint FileSize;

        public BinaryHeader()
        {
            Magic = "BNTX";
            MagicPad = "    ";
            VersionMicro = 0;
            VersionMinor = 0;
            VersionMajor = 4;
            ByteOrder = 0xFFFE;
            Alignment = 0xC;
            TargetAddressSize = 0x40;
            Flag = 0;
            BlockOffset = 0xA001;
        }
    }

    [StructLayout(LayoutKind.Sequential, Size = 0x10)]
    public struct ResHeader
    {
        internal Magic Magic; // BRTI  
        internal uint BlockSize;
        internal ulong BlockPointer;
    }

    [StructLayout(LayoutKind.Sequential, Size = 0x10)]
    public struct BntxHeader 
    {
        internal Magic Magic; //NX  
        public uint TextureCount;
        public ulong TextureInfoPointer;
        public ulong TextureDataPointer;
        public ulong TextureDictionaryPointer;
        public ulong MemoryPoolPointer;
        public ulong Reserved0;
        public ulong Reserved1;

        public BntxHeader()
        {
            Magic = "NX  ";
            MemoryPoolPointer = 0x58;
        }
    }

    [StructLayout(LayoutKind.Sequential, Size = 0x10)]
    public struct ResBntxTextureInfo
    {
        // Base res header
        public ResHeader ResHeader;

        // ResGfxTextureInfo
        public ResGfxTextureInfoFlags Flags;
        public GfxImageStorageDimension Dim;
        public GfxTileMode TileMode;
        public ushort Swizzle;
        public ushort MipCount;
        public uint SampleCount;
        public GfxImageFormat ImageFormat;
        public GfxGpuAccessFlags GpuAccessFlags;
        public uint Width;
        public uint Height;
        public uint Depth;
        public uint ArrayCount;
        public uint TextureLayout;

        // ResBntxTextureInfo
        public uint TextureLayout2;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 20)]
        public byte[] Reserved;

        public uint ImageSize;
        public uint Alignment;
        public GfxChannelSource GfxChannelSourceR;
        public GfxChannelSource GfxChannelSourceG;
        public GfxChannelSource GfxChannelSourceB;
        public GfxChannelSource GfxChannelSourceA;
        public uint SurfaceDim;

        internal ulong NameOffset;
        internal ulong BntxHeaderOffset;
        internal ulong DataPointersOffset;
        internal ulong UserDataOffset;
        internal ulong TexturePointer;
        internal ulong TextureViewPointer;
        internal ulong DescSlotOffset;
        internal ulong UserDataDictionaryOffset;

        public ResBntxTextureInfo()
        {
            this.Reserved = new byte[20];
            this.Dim = GfxImageStorageDimension.Dim2D;
            this.TileMode = GfxTileMode.Optimal;
            this.SampleCount = 1;
            this.Alignment = 512;
            this.ImageFormat = GfxImageFormat.R8G8B8A8_Unorm;
            this.GpuAccessFlags = GfxGpuAccessFlags.Texture;
            this.Flags = ResGfxTextureInfoFlags.IsPackedTexture | ResGfxTextureInfoFlags.IsResTexture;
            this.GfxChannelSourceR = GfxChannelSource.Red;
            this.GfxChannelSourceG = GfxChannelSource.Green;
            this.GfxChannelSourceB = GfxChannelSource.Blue;
            this.GfxChannelSourceA = GfxChannelSource.Alpha;
        }
    }

    [StructLayout(LayoutKind.Sequential, Size = 0x10)]
    public struct UserDataHeader
    {
        public ulong NameOffset;
        public ulong DataOffset;
        public uint Count;
        public UserDataType Type;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 43)]
        public byte[] Reserved;

        public UserDataHeader()
        {
            Reserved = new byte[43];
        }
    }

    [StructLayout(LayoutKind.Sequential, Size = 0x10)]
    public struct DictEntry
    {
        public uint Key;
        public ushort IdxLeft;
        public ushort IdxRight;
        public ulong NameOffset;
    }
}
