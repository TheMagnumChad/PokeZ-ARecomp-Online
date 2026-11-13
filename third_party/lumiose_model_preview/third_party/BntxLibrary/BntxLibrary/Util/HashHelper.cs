using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace BntxLibrary.Util
{
    public class HashHelper
    {
        public static uint ComputeHash32(Stream stream)
        {
            const uint fnvOffset = 2166136261;
            const uint fnvPrime = 16777619;

            uint hash = fnvOffset;
            byte[] buffer = new byte[4096];
            int bytesRead;

            stream.Position = 0;
            while ((bytesRead = stream.Read(buffer, 0, buffer.Length)) > 0)
            {
                for (int i = 0; i < bytesRead; i++)
                {
                    hash ^= buffer[i];
                    hash *= fnvPrime;
                }
            }
            // Reset back to 0
            stream.Position = 0;

            return hash;
        }

        public static uint ComputeHash32<T>(T strct) where T : struct
        {
            int size = Marshal.SizeOf(typeof(T));
            byte[] bytes = new byte[size];
            IntPtr ptr = Marshal.AllocHGlobal(size);
            try
            {
                // Copy struct to raw bytes
                Marshal.StructureToPtr(strct, ptr, false);
                Marshal.Copy(ptr, bytes, 0, size);

                // Simple FNV-1a 32-bit hash
                const uint fnvOffset = 2166136261;
                const uint fnvPrime = 16777619;
                uint hash = fnvOffset;

                foreach (byte b in bytes)
                {
                    hash ^= b;
                    hash *= fnvPrime;
                }

                return hash;
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }

        public static uint ComputeHash32(byte[] bytes) 
        {
            // Simple FNV-1a 32-bit hash
            const uint fnvOffset = 2166136261;
            const uint fnvPrime = 16777619;
            uint hash = fnvOffset;

            foreach (byte b in bytes)
            {
                hash ^= b;
                hash *= fnvPrime;
            }
            return hash;
        }

        public static uint ComputeHash32(string text)
        {
            if (text == null) return 0;
            return ComputeHash32(System.Text.Encoding.UTF8.GetBytes(text));
        }
    }
}
