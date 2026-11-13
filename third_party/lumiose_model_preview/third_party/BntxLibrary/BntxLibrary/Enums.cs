using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace BntxLibrary
{
    public enum UserDataType : byte
    { 
        Int32,
        Single,
        String,
        Byte,
        WString,
    }

    public enum PlatformKind : uint
    {
        Ounce,
        NX,
        PC,
    }

    [Flags]
    public enum ResGfxTextureInfoFlags : byte
    {
        None = 0,
        IsPackedTexture = 1,
        IsSparseBinding = 2,
        IsSparse = 4,
        IsResTexture = 8,
    }

    public enum GfxChannelSource : byte
    {
        Zero = 0x0,
        One = 0x1,
        Red = 0x2,
        Green = 0x3,
        Blue = 0x4,
        Alpha = 0x5
    }

    public enum GfxImageStorageDimension : byte
    {
        Dim1D = 1,
        Dim2D,
        Dim3D,
        DimCube,
        Dim1DArray,
        Dim2DArray,
        Dim2DMsaa,
        Dim2DMsaaArray,
        DimCubeArray,
        DimRectangle,
    }
    public enum GfxTileMode : ushort
    {
        Optimal = 0,
        Linear = 1
    }

    public enum GfxChannelFormat
    {
        None = 0x1,
        R8 = 0x2,
        R4G4B4A4 = 0x3,
        R5G5B5A1 = 0x5,
        A1B5G5R5 = 0x6,
        R5G6B5 = 0x7,
        B5G6R5 = 0x8,
        R8G8 = 0x9,
        R16 = 0xA,
        R8G8B8A8 = 0xB,
        B8G8R8A8 = 0xC,
        R9G9B9E5F = 0xD,
        R10G10B10A2 = 0xE,
        R11G11B10F = 0xF,
        R16G16 = 0x12,
        D24S8 = 0x13,
        R32 = 0x14,
        R16G16B16A16 = 0x15,
        D32FS8 = 0x16,
        R32G32 = 0x17,
        R32G32B32 = 0x18,
        R32G32B32A32 = 0x19,
        BC1 = 0x1A,
        BC2 = 0x1B,
        BC3 = 0x1C,
        BC4 = 0x1D,
        BC5 = 0x1E,
        BC6H = 0x1F,
        BC7U = 0x20,
        ASTC_4x4 = 0x2D,
        ASTC_5x4 = 0x2E,
        ASTC_5x5 = 0x2F,
        ASTC_6x5 = 0x30,
        ASTC_6x6 = 0x31,
        ASTC_8x5 = 0x32,
        ASTC_8x6 = 0x33,
        ASTC_8x8 = 0x34,
        ASTC_10x5 = 0x35,
        ASTC_10x6 = 0x26,
        ASTC_10x8 = 0x37,
        ASTC_10x10 = 0x38,
        ASTC_12x10 = 0x39,
        ASTC_12x12 = 0x3A,
        B5G5R5A1 = 0x3B,
    }

    public enum ChannelSource : byte
    {
        Zero,
        One,
        Red,
        Green,
        Blue,
        Alpha
    }

    [Flags]
    public enum GfxGpuAccessFlags : uint
    {
        Reserve0 = (1 << 0),
        Reserve1 = (1 << 1),
        VertexBuffer = (1 << 2),
        IndexBuffer = (1 << 3),
        UniformBuffer = (1 << 4),
        Texture = (1 << 5),
        TransferDestination = (1 << 6),
        RenderTargetColor = (1 << 7),
        RenderTargetDepth = (1 << 8),
        IndirectDraw = (1 << 9),
        DisplayTexture = (1 << 10),
        Counter = (1 << 11),
    }

    public enum GfxTypeFormat
    {
        Unorm = 0x1,
        Snorm = 0x2,
        UInt = 0x3,
        SInt = 0x4,
        Float = 0x5,
        SRGB = 0x6,
        Depth = 0x7,
        UScaled = 0x8,
        SScaled = 0x9,
        UFloat = 0xa,
    }

    public enum GfxImageFormat : uint
    {
        R8_Unorm = (GfxChannelFormat.R8 << 8) | GfxTypeFormat.Unorm,
        R8_Snorm = (GfxChannelFormat.R8 << 8) | GfxTypeFormat.Snorm,
        R8_UInt = (GfxChannelFormat.R8 << 8) | GfxTypeFormat.UInt,
        R8_SInt = (GfxChannelFormat.R8 << 8) | GfxTypeFormat.SInt,
        R4G4B4A4_Unorm = (GfxChannelFormat.R4G4B4A4 << 8) | GfxTypeFormat.Unorm,
        R5G5B5A1_Unorm = (GfxChannelFormat.R5G5B5A1 << 8) | GfxTypeFormat.Unorm,
        A1B5G5R5_Unorm = (GfxChannelFormat.A1B5G5R5 << 8) | GfxTypeFormat.Unorm,
        R5G6B5_Unorm = (GfxChannelFormat.R5G6B5 << 8) | GfxTypeFormat.Unorm,
        B5G6R5_Unorm = (GfxChannelFormat.B5G6R5 << 8) | GfxTypeFormat.Unorm,
        R8G8_Unorm = (GfxChannelFormat.R8G8 << 8) | GfxTypeFormat.Unorm,
        R8G8_Snorm = (GfxChannelFormat.R8G8 << 8) | GfxTypeFormat.Snorm,
        R8G8_UInt = (GfxChannelFormat.R8G8 << 8) | GfxTypeFormat.UInt,
        R8G8_SInt = (GfxChannelFormat.R8G8 << 8) | GfxTypeFormat.SInt,
        R16_Unorm = (GfxChannelFormat.R16 << 8) | GfxTypeFormat.Unorm,
        R16_Snorm = (GfxChannelFormat.R16 << 8) | GfxTypeFormat.Snorm,
        R16_UInt = (GfxChannelFormat.R16 << 8) | GfxTypeFormat.UInt,
        R16_SInt = (GfxChannelFormat.R16 << 8) | GfxTypeFormat.SInt,
        R16_Float = (GfxChannelFormat.R16 << 8) | GfxTypeFormat.Float,
        Z16_Depth = (GfxChannelFormat.R16 << 8) | GfxTypeFormat.Depth,
        R8G8B8A8_Unorm = (GfxChannelFormat.R8G8B8A8 << 8) | GfxTypeFormat.Unorm,
        R8G8B8A8_Snorm = (GfxChannelFormat.R8G8B8A8 << 8) | GfxTypeFormat.Snorm,
        R8G8B8A8_UInt = (GfxChannelFormat.R8G8B8A8 << 8) | GfxTypeFormat.UInt,
        R8G8B8A8_SInt = (GfxChannelFormat.R8G8B8A8 << 8) | GfxTypeFormat.SInt,
        R8G8B8A8_SRGB = (GfxChannelFormat.R8G8B8A8 << 8) | GfxTypeFormat.SRGB,
        B8G8R8A8_Unorm = (GfxChannelFormat.B8G8R8A8 << 8) | GfxTypeFormat.Unorm,
        B8G8R8A8_SRGB = (GfxChannelFormat.B8G8R8A8 << 8) | GfxTypeFormat.SRGB,
        R9G9B9E5F_SharedFloat = (GfxChannelFormat.R9G9B9E5F << 8) | GfxTypeFormat.Float,
        R10G10B10A2_Unorm = (GfxChannelFormat.R10G10B10A2 << 8) | GfxTypeFormat.Unorm,
        R10G10B10A2_UInt = (GfxChannelFormat.R10G10B10A2 << 8) | GfxTypeFormat.UInt,
        R11G11B10F_Float = (GfxChannelFormat.R11G11B10F << 8) | GfxTypeFormat.Float,
        R16G16_Unorm = (GfxChannelFormat.R16G16 << 8) | GfxTypeFormat.Unorm,
        R16G16_Snorm = (GfxChannelFormat.R16G16 << 8) | GfxTypeFormat.Snorm,
        R16G16_UInt = (GfxChannelFormat.R16G16 << 8) | GfxTypeFormat.UInt,
        R16G16_SInt = (GfxChannelFormat.R16G16 << 8) | GfxTypeFormat.SInt,
        R16G16_Float = (GfxChannelFormat.R16G16 << 8) | GfxTypeFormat.Float,
        D24S8_Depth = (GfxChannelFormat.D24S8 << 8) | GfxTypeFormat.Depth,
        R32_UInt = (GfxChannelFormat.R32 << 8) | GfxTypeFormat.UInt,
        R32_SInt = (GfxChannelFormat.R32 << 8) | GfxTypeFormat.SInt,
        R32_Float = (GfxChannelFormat.R32 << 8) | GfxTypeFormat.Float,
        D32F_Depth = (GfxChannelFormat.R32 << 8) | GfxTypeFormat.Depth,
        R16G16B16A16_Unorm = (GfxChannelFormat.R16G16B16A16 << 8) | GfxTypeFormat.Unorm,
        R16G16B16A16_Snorm = (GfxChannelFormat.R16G16B16A16 << 8) | GfxTypeFormat.Snorm,
        R16G16B16A16_UInt = (GfxChannelFormat.R16G16B16A16 << 8) | GfxTypeFormat.UInt,
        R16G16B16A16_SInt = (GfxChannelFormat.R16G16B16A16 << 8) | GfxTypeFormat.SInt,
        R16G16B16A16_Float = (GfxChannelFormat.R16G16B16A16 << 8) | GfxTypeFormat.Float,
        D32FS8_Depth = (GfxChannelFormat.D32FS8 << 8) | GfxTypeFormat.Depth,
        R32G32_UInt = (GfxChannelFormat.R32G32 << 8) | GfxTypeFormat.UInt,
        R32G32_SInt = (GfxChannelFormat.R32G32 << 8) | GfxTypeFormat.SInt,
        R32G32_Float = (GfxChannelFormat.R32G32 << 8) | GfxTypeFormat.Float,
        R32G32B32_UInt = (GfxChannelFormat.R32G32B32 << 8) | GfxTypeFormat.UInt,
        R32G32B32_SInt = (GfxChannelFormat.R32G32B32 << 8) | GfxTypeFormat.SInt,
        R32G32B32_Float = (GfxChannelFormat.R32G32B32 << 8) | GfxTypeFormat.Float,
        R32G32B32A32_UInt = (GfxChannelFormat.R32G32B32A32 << 8) | GfxTypeFormat.UInt,
        R32G32B32A32_SInt = (GfxChannelFormat.R32G32B32A32 << 8) | GfxTypeFormat.SInt,
        R32G32B32A32_Float = (GfxChannelFormat.R32G32B32A32 << 8) | GfxTypeFormat.Float,
        BC1_Unorm = (GfxChannelFormat.BC1 << 8) | GfxTypeFormat.Unorm,
        BC1_SRGB = (GfxChannelFormat.BC1 << 8) | GfxTypeFormat.SRGB,
        BC2_Unorm = (GfxChannelFormat.BC2 << 8) | GfxTypeFormat.Unorm,
        BC2_SRGB = (GfxChannelFormat.BC2 << 8) | GfxTypeFormat.SRGB,
        BC3_Unorm = (GfxChannelFormat.BC3 << 8) | GfxTypeFormat.Unorm,
        BC3_SRGB = (GfxChannelFormat.BC3 << 8) | GfxTypeFormat.SRGB,
        BC4_Unorm = (GfxChannelFormat.BC4 << 8) | GfxTypeFormat.Unorm,
        BC4_Snorm = (GfxChannelFormat.BC4 << 8) | GfxTypeFormat.Snorm,
        BC5_Unorm = (GfxChannelFormat.BC5 << 8) | GfxTypeFormat.Unorm,
        BC5_Snorm = (GfxChannelFormat.BC5 << 8) | GfxTypeFormat.Snorm,
        BC6H_SF16 = (GfxChannelFormat.BC6H << 8) | GfxTypeFormat.Float,
        BC6H_UF16 = (GfxChannelFormat.BC6H << 8) | GfxTypeFormat.UFloat,
        BC7U_Unorm = (GfxChannelFormat.BC7U << 8) | GfxTypeFormat.Unorm,
        BC7U_SRGB = (GfxChannelFormat.BC7U << 8) | GfxTypeFormat.SRGB,
        ASTC_4X4_Unorm = (GfxChannelFormat.ASTC_4x4 << 8) | GfxTypeFormat.Unorm,
        ASTC_4X4_SRGB = (GfxChannelFormat.ASTC_4x4 << 8) | GfxTypeFormat.SRGB,
        ASTC_5X4_Unorm = (GfxChannelFormat.ASTC_5x4 << 8) | GfxTypeFormat.Unorm,
        ASTC_5X4_SRGB = (GfxChannelFormat.ASTC_5x4 << 8) | GfxTypeFormat.SRGB,
        ASTC_5X5_Unorm = (GfxChannelFormat.ASTC_5x5 << 8) | GfxTypeFormat.Unorm,
        ASTC_5X5_SRGB = (GfxChannelFormat.ASTC_5x5 << 8) | GfxTypeFormat.SRGB,
        ASTC_6X5_Unorm = (GfxChannelFormat.ASTC_6x5 << 8) | GfxTypeFormat.Unorm,
        ASTC_6X5_SRGB = (GfxChannelFormat.ASTC_6x5 << 8) | GfxTypeFormat.SRGB,
        ASTC_6X6_Unorm = (GfxChannelFormat.ASTC_6x6 << 8) | GfxTypeFormat.Unorm,
        ASTC_6X6_SRGB = (GfxChannelFormat.ASTC_6x6 << 8) | GfxTypeFormat.SRGB,
        ASTC_8X5_Unorm = (GfxChannelFormat.ASTC_8x5 << 8) | GfxTypeFormat.Unorm,
        ASTC_8X5_SRGB = (GfxChannelFormat.ASTC_8x5 << 8) | GfxTypeFormat.SRGB,
        ASTC_8X6_Unorm = (GfxChannelFormat.ASTC_8x6 << 8) | GfxTypeFormat.Unorm,
        ASTC_8X6_SRGB = (GfxChannelFormat.ASTC_8x6 << 8) | GfxTypeFormat.SRGB,
        ASTC_8X8_Unorm = (GfxChannelFormat.ASTC_8x8 << 8) | GfxTypeFormat.Unorm,
        ASTC_8X8_SRGB = (GfxChannelFormat.ASTC_8x8 << 8) | GfxTypeFormat.SRGB,
        ASTC_10X5_Unorm = (GfxChannelFormat.ASTC_10x5 << 8) | GfxTypeFormat.Unorm,
        ASTC_10X5_SRGB = (GfxChannelFormat.ASTC_10x5 << 8) | GfxTypeFormat.SRGB,
        ASTC_10X6_Unorm = (GfxChannelFormat.ASTC_10x6 << 8) | GfxTypeFormat.Unorm,
        ASTC_10X6_SRGB = (GfxChannelFormat.ASTC_10x6 << 8) | GfxTypeFormat.SRGB,
        ASTC_10X8_Unorm = (GfxChannelFormat.ASTC_10x8 << 8) | GfxTypeFormat.Unorm,
        ASTC_10X8_SRGB = (GfxChannelFormat.ASTC_10x8 << 8) | GfxTypeFormat.SRGB,
        ASTC_10X10_Unorm = (GfxChannelFormat.ASTC_10x10 << 8) | GfxTypeFormat.Unorm,
        ASTC_10X10_SRGB = (GfxChannelFormat.ASTC_10x10 << 8) | GfxTypeFormat.SRGB,
        ASTC_12X10_Unorm = (GfxChannelFormat.ASTC_12x10 << 8) | GfxTypeFormat.Unorm,
        ASTC_12X10_SRGB = (GfxChannelFormat.ASTC_12x10 << 8) | GfxTypeFormat.SRGB,
        ASTC_12X12_Unorm = (GfxChannelFormat.ASTC_12x12 << 8) | GfxTypeFormat.Unorm,
        ASTC_12X12_SRGB = (GfxChannelFormat.ASTC_12x12 << 8) | GfxTypeFormat.SRGB,
        B5G5R5A1_Unorm = (GfxChannelFormat.B5G5R5A1 << 8) | GfxTypeFormat.Unorm
    }
}
