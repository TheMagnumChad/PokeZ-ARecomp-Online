using System.Buffers.Binary;
using Trinity.Core.Utils;
byte[] Fixture(bool visible, byte kind = 1) {
    byte[] b = new byte[224];
    void I(int p, int v) => BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(p), v);
    void H(int p, ushort v) => BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(p), v);
    void Table(int vt, int table, ushort[] offsets, ushort size) {
        H(vt, (ushort)(4 + offsets.Length * 2)); H(vt + 2, size); I(table, table - vt);
        for (int i=0;i<offsets.Length;i++) H(vt+4+i*2,offsets[i]);
    }
    I(0,16); Table(8,16,new ushort[]{0,4},8); I(20,20); I(40,1); I(44,36);
    Table(60,80,new ushort[]{4,0,0,0,0,8},12); I(84,16); I(88,56);
    I(100,4); System.Text.Encoding.ASCII.GetBytes("mesh").CopyTo(b,104);
    Table(132,144,new ushort[]{0,0,4},8); I(148,28);
    Table(160,176,new ushort[]{4,8},12); b[180]=kind; I(184,24);
    Table(196,208,new ushort[]{visible?(ushort)4:(ushort)0},8); b[212]=visible?(byte)1:(byte)0;
    return b;
}
foreach(bool visible in new[]{false,true}) {
    var r=StaticVisibility.Read(Fixture(visible));
    if(r.Count!=1 || r["mesh"]!=visible)throw new Exception("fixed visibility mismatch");
}
bool unsupported=false;try{StaticVisibility.Read(Fixture(true,2));}catch(NotSupportedException){unsupported=true;}
if(!unsupported)throw new Exception("dynamic visibility silently accepted");
foreach(byte[] bad in new[]{Array.Empty<byte>(),new byte[3],Fixture(true)[..211]}) {
    bool rejected=false;try{StaticVisibility.Read(bad);}catch(InvalidDataException){rejected=true;}
    if(!rejected)throw new Exception("truncated data accepted");
}
var invalid=Fixture(true);invalid[212]=2;
try{StaticVisibility.Read(invalid);throw new Exception("invalid bool accepted");}catch(InvalidDataException){}
foreach(string path in args) {
    var r=StaticVisibility.Read(path);
    Console.WriteLine(path+": "+string.Join(", ",r.Select(x=>$"{x.Key}={x.Value}")));
}
Console.WriteLine("Static visibility tests PASS");
