using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace BntxLibrary.IO
{
    internal static class IOExtensions
    {
        //Structs can be a bit faster and more memory efficent
        //From https://github.com/IcySon55/Kuriimu/blob/master/src/Kontract/IO/Extensions.cs
        //Read
        public static unsafe T BytesToStruct<T>(this byte[] buffer, bool isBigEndian = false, int offset = 0)
        {
            if (offset < 0) throw new ArgumentOutOfRangeException(nameof(offset));

            AdjustBigEndianByteOrder(typeof(T), buffer, isBigEndian);

            fixed (byte* pBuffer = buffer)
                return Marshal.PtrToStructure<T>((IntPtr)pBuffer + offset);
        }

        // Write
        public static unsafe byte[] StructToBytes<T>(this T item, bool isBigEndian)
        {
            var buffer = new byte[Marshal.SizeOf(typeof(T))];

            fixed (byte* pBuffer = buffer)
                Marshal.StructureToPtr(item, (IntPtr)pBuffer, false);

            AdjustBigEndianByteOrder(typeof(T), buffer, isBigEndian);

            return buffer;
        }

        //Adjust byte order for big endian
        private static void AdjustBigEndianByteOrder(Type type, byte[] buffer, bool isBigEndian, int startOffset = 0)
        {
            if (!isBigEndian)
                return;

            if (type.IsPrimitive)
            {
                if (type == typeof(short) || type == typeof(ushort) ||
                 type == typeof(int) || type == typeof(uint) ||
                 type == typeof(long) || type == typeof(ulong) ||
                  type == typeof(double) || type == typeof(float))
                {
                    Array.Reverse(buffer);
                    return;
                }
            }

            foreach (var field in type.GetFields())
            {
                var fieldType = field.FieldType;

                // Ignore static fields
                if (field.IsStatic) continue;

                if (fieldType.BaseType == typeof(Enum) && fieldType != typeof(ByteOrder))
                    fieldType = fieldType.GetFields()[0].FieldType;

                var offset = Marshal.OffsetOf(type, field.Name).ToInt32();
                var effectiveOffset = startOffset + offset;

                // Enums
                if (fieldType.IsEnum)
                    fieldType = Enum.GetUnderlyingType(fieldType);

                var marshalAttr = field.GetCustomAttribute<MarshalAsAttribute>();
                if (marshalAttr != null && fieldType.IsArray && marshalAttr.Value == UnmanagedType.ByValArray)
                {
                    var elementType = fieldType.GetElementType();
                    int elementSize = Marshal.SizeOf(elementType);
                    int count = marshalAttr.SizeConst;

                    for (int i = 0; i < count; i++)
                    {
                        int elementOffset = effectiveOffset + i * elementSize;

                        if (elementType == typeof(short) || elementType == typeof(ushort))
                            Array.Reverse(buffer, elementOffset, 2);
                        else if (elementType == typeof(int) || elementType == typeof(uint))
                            Array.Reverse(buffer, elementOffset, 4);
                        else if (elementType == typeof(long) || elementType == typeof(ulong))
                            Array.Reverse(buffer, elementOffset, 8);
                        else if (elementType == typeof(float))
                            Array.Reverse(buffer, elementOffset, 4);
                        else if (elementType == typeof(double))
                            Array.Reverse(buffer, elementOffset, 8);
                        // Skip byte/boolean/etc.
                    }

                    // Check for sub-fields to recurse if necessary
                    var subFields = fieldType.GetFields().Where(subField => subField.IsStatic == false).ToArray();

                    if (fieldType == typeof(short) || fieldType == typeof(ushort) ||
                    fieldType == typeof(int) || fieldType == typeof(uint) ||
                    fieldType == typeof(long) || fieldType == typeof(ulong) ||
                    fieldType == typeof(double) || fieldType == typeof(float))
                    {
                        if (subFields.Length == 0)
                            Array.Reverse(buffer, effectiveOffset, Marshal.SizeOf(fieldType));

                    }

                    if (subFields.Length > 0)
                        AdjustBigEndianByteOrder(fieldType, buffer, isBigEndian, effectiveOffset);
                }
                else
                {
                    // Check for sub-fields to recurse if necessary
                    var subFields = fieldType.GetFields().Where(subField => subField.IsStatic == false).ToArray();

                    if (fieldType == typeof(short) || fieldType == typeof(ushort) ||
                        fieldType == typeof(int) || fieldType == typeof(uint) ||
                        fieldType == typeof(long) || fieldType == typeof(ulong) ||
                        fieldType == typeof(double) || fieldType == typeof(float))
                    {
                        if (subFields.Length == 0)
                            Array.Reverse(buffer, effectiveOffset, Marshal.SizeOf(fieldType));

                    }

                    if (subFields.Length > 0)
                        AdjustBigEndianByteOrder(fieldType, buffer, isBigEndian, effectiveOffset);
                }
            }
        }
    }
}
