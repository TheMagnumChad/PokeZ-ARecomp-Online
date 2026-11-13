using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace BntxLibrary
{
    public class RelocationTable
    {
        public List<Section> Sections = new List<Section>();
        public List<Entry> Entries = new List<Entry>();

        [StructLayout(LayoutKind.Sequential, Size = 0x10)]
        public struct Section // Section where pointers are pointing to
        {
            public ulong Pointer; // 0
            public uint Offset; // section position
            public int Size; // section size
            public int FirstEntryIndex; // entry index 
            public int EntryCount; // entry count
        }

        [StructLayout(LayoutKind.Sequential, Size = 8)]
        public struct Entry
        {
            public int PointersOffset; // start of pointers
            public ushort ArrayCount; // structure count in array
            public byte PointerCount; // total pointer count as uint64
            public byte PaddingCount; // padding count as uint64. Reads to the next structure offset
        }

        public void Build(List<Pointer> pointers, List<(long, long)> sectionRanges, long sectionEnd)
        {
            this.Sections.Clear();
            this.Entries.Clear();

            // A section marker where a section starts for processing relocation data 
            for (int s = 0; s < sectionRanges.Count; s++)
            {
                var rangeStart = sectionRanges[s].Item1;
                var rangeEnd = sectionRanges[s].Item2;
                // Use pointers that target within the section we want to edit
                var sectionPointers = pointers.Where(x =>
                    x.Target >= rangeStart &&
                    x.Target <= rangeEnd).OrderBy(x => x.Position).ToList();
                var entries = Build(sectionPointers);
                this.Sections.Add(new Section
                {
                    Pointer = 0, // Runtime pointer
                    Offset = (uint)rangeStart,
                    Size = (int)(rangeEnd - rangeStart),
                    FirstEntryIndex = this.Entries.Count,
                    EntryCount = entries.Count
                });
                this.Entries.AddRange(entries);
            }

            // Ensure pointers are all in range of the sections marked
            foreach (var p in pointers)
            {
                bool isRelocated = false;
                foreach (var section in this.Sections)
                {
                    var rangeStart = section.Offset;
                    var rangeEnd = section.Offset + section.Size;

                    if (p.Target >= rangeStart && p.Target <= rangeEnd)
                        isRelocated = true;
                }
                if (!isRelocated)
                    throw new Exception($"Failed to find pointer target in relocation table {p.Target}!");
            }


            // Final check, ensure all entries are relocated
            const int POINTER_SIZE = 8;

            List<long> relocatedPositions = new List<long>();
            foreach (var entry in this.Entries)
            {
                var ofs = entry.PointersOffset;
                for (int a = 0; a < entry.ArrayCount; a++)
                {
                    for (int p = 0; p < entry.PointerCount; p++)
                    {
                        relocatedPositions.Add(ofs);
                        ofs += POINTER_SIZE;
                    }
                    ofs += entry.PaddingCount * POINTER_SIZE;
                }
            }

            foreach (var p in pointers)
            {
                if (!relocatedPositions.Contains(p.Position))
                    throw new Exception($"Failed to find pointer in relocation table {p.Position}!");
            }

            Console.WriteLine($"Built relocation table with {this.Entries.Count} entries.");
        }


        // Example
        // DictionaryEntry entries[18]
        //
        // DictionaryEntry
        // {
        //      public int LeftIndex;
        //      public ushort LeftIndex;
        //      public ushort RightIndex;
        //      public ulong NameOffset; // Pointer here
        // }
        // Relocation becomes
        // PointersOffset = 8 (offset to first pointer)
        // ArrayCount = 18 ( we have 18 dictionaries)
        // PointerCount = 1 (we have only 1 offset)
        // PaddingCount = 1 (we read 1 uint64 until next offset from the next DictionaryEntry entry. All sections are aligned by 8.
        public List<Entry> Build(List<Pointer> sectionPointers)
        {
            const int PTR_SIZE = 8;

            List<Entry> entries = new List<Entry>();
            int i = 0;
            while (i < sectionPointers.Count)
            {
                long basePos = sectionPointers[i].Position;

                // Find stride to next pointer
                long stride = 0;
                if (i + 1 < sectionPointers.Count)
                    stride = sectionPointers[i + 1].Position - sectionPointers[i].Position;

                // Consecutive pointers with same stride
                int arrayCount = 1;
                int j = i + 1;
                while (j < sectionPointers.Count &&
                       sectionPointers[j].Position - sectionPointers[j - 1].Position == stride)
                {
                    arrayCount++;
                    j++;
                }

                // Check the padding region between our stride and pointers
                int pointerCount = 1;
                int paddingCount = 0;
                if (stride > PTR_SIZE)
                    paddingCount = (int)((stride - PTR_SIZE) / PTR_SIZE);

                // If there is no padding between pointers, but there are arrays, each pointer is consecutive
                if (paddingCount == 0 && arrayCount > 1)
                {
                    pointerCount = arrayCount;
                    arrayCount = 1;
                }

                // Add entry
                entries.Add(new Entry
                {
                    PointersOffset = (int)basePos,
                    ArrayCount = (ushort)arrayCount,
                    PointerCount = (byte)pointerCount,
                    PaddingCount = (byte)paddingCount
                });
                i = j;
            }

            // Verify
            HashSet<long> relocatedPositions = new HashSet<long>();
            foreach (var entry in entries)
            {
                var ofs = entry.PointersOffset;
                for (int a = 0; a < entry.ArrayCount; a++)
                {
                    for (int p = 0; p < entry.PointerCount; p++)
                    {
                        bool added = relocatedPositions.Add(ofs);
                        if (!added)
                            throw new Exception($"Pointer offset {ofs} was relocated more than once!");
                        ofs += PTR_SIZE;
                    }
                    ofs += entry.PaddingCount * PTR_SIZE;
                }
            }
            foreach (var p in sectionPointers)
            {
                if (!relocatedPositions.Contains(p.Position))
                    throw new Exception($"Failed to find pointer in relocation table {p.Position}!");
            }
            return entries;
        }
    }

    public class Pointer
    {
        public long Position; // Where the offset is stored
        public long Target; // Where the offset pointers to
    }
}
