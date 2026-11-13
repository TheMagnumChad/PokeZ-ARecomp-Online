using System.Buffers.Binary;
using Lumiose.ReadOnlyFormat;
using Lumiose.ReadOnlyFormat.Attributes;
[FlatBufferStruct] public class Pair { [FlatBufferItem(0)] public int A{get;set;} [FlatBufferItem(1)] public int B{get;set;} }
[FlatBufferTable] public class Number { [FlatBufferItem(0)] public int Value{get;set;} }
[FlatBufferTable] public class Root {
 [FlatBufferItem(0)] public int Number{get;set;}
 [FlatBufferItem(1)] public Pair Pair{get;set;}=new();
 [FlatBufferItem(2)] public string Name{get;set;}="";
 [FlatBufferItem(3)] public byte[] Bytes{get;set;}=Array.Empty<byte>();
 [FlatBufferItem(4)] public FlatBufferUnion<Number,Number,Number,Number> Channel{get;set;}
}
public static class Program {
 static byte[] Fixture(){var b=new byte[120];void I(int p,int v)=>BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(p),v);void S(int p,ushort v)=>BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(p),v);
 I(0,32);S(16,16);S(18,32);for(int i=0;i<6;i++)S(20+2*i,(ushort)new[]{4,8,16,20,24,28}[i]);I(32,16);I(36,123);I(40,7);I(44,9);I(48,32);I(52,44);b[56]=1;I(60,52);I(80,5);System.Text.Encoding.UTF8.GetBytes("hello").CopyTo(b,84);I(96,3);b[100]=4;b[101]=5;b[102]=6;S(104,6);S(106,8);S(108,4);I(112,8);I(116,456);return b; }
 public static void Main(){int count=0;void Assert(bool b){if(!b)throw new Exception("Test failed #"+count);count++;}var bytes=Fixture();var r=new Reader(bytes).Read<Root>();Assert(r.Number==123);Assert(r.Pair.A==7&&r.Pair.B==9);Assert(r.Name=="hello");Assert(r.Bytes.SequenceEqual(new byte[]{4,5,6}));int u=0;r.Channel.Switch(()=>{},n=>u=n.Value,n=>{},n=>{},n=>{});Assert(u==456);
 void Reject(Action<byte[]> mutate){var b=Fixture();mutate(b);try{new Reader(b).Read<Root>();throw new Exception("Malformed buffer accepted");}catch(System.IO.InvalidDataException){count++;}catch(OverflowException){count++;}catch(System.Text.DecoderFallbackException){count++;}}
 Reject(b=>BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(0),uint.MaxValue));Reject(b=>b[56]=5);Reject(b=>BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(96),uint.MaxValue));Reject(b=>b[89]=1);Reject(b=>BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(32),int.MaxValue));Reject(b=>BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(20),33));
 Console.WriteLine($"PASS {count} synthetic parser assertions"); }
}
