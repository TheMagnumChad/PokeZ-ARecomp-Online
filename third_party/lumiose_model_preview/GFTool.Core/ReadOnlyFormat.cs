// Independently authored read-only FlatBuffers wire reader. No FlatSharp implementation.
// GPL-2.0-or-later; schema declarations retain their original GFTool license.
using System.Buffers.Binary;
using System.Collections;
using System.Reflection;
using System.Text;
namespace Lumiose.ReadOnlyFormat.Attributes {
 [AttributeUsage(AttributeTargets.Enum)] public sealed class FlatBufferEnumAttribute:Attribute {public FlatBufferEnumAttribute(Type underlying){}}
 [AttributeUsage(AttributeTargets.Class|AttributeTargets.Struct)] public sealed class FlatBufferTableAttribute:Attribute {}
 [AttributeUsage(AttributeTargets.Class|AttributeTargets.Struct)] public sealed class FlatBufferStructAttribute:Attribute {}
 [AttributeUsage(AttributeTargets.Property)] public sealed class FlatBufferItemAttribute:Attribute {
  public int Index {get;} public object? DefaultValue {get;set;} public FlatBufferItemAttribute(int index){Index=index;}
 }
}
namespace Lumiose.ReadOnlyFormat {
 public readonly struct FlatBufferUnion<A,B,C,D> {
  readonly object? value; public byte Discriminator {get;}
  public FlatBufferUnion(byte discriminator,object? item){Discriminator=discriminator;value=item;}
  public void Switch(Action defaultCase,Action<A> case1,Action<B> case2,Action<C> case3,Action<D> case4) {
   switch(Discriminator){case 1:case1((A)value!);break;case 2:case2((B)value!);break;case 3:case3((C)value!);break;case 4:case4((D)value!);break;default:defaultCase();break;}
  }
 }
 public sealed class Reader {
  readonly byte[] data; int objects; const int MaxObjects=1000000; const int MaxVector=16777216;
  public Reader(byte[] bytes){if(bytes.Length<4||bytes.Length>134217728)throw new InvalidDataException("FlatBuffer size");data=bytes;}
  void Check(int p,int n){if(p<0||n<0||p>data.Length-n)throw new InvalidDataException("FlatBuffer range");}
  ushort U16(int p){Check(p,2);return BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(p,2));}
  uint U32(int p){Check(p,4);return BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(p,4));}
  int I32(int p)=>unchecked((int)U32(p));
  int Follow(int p){uint n=U32(p);if(n<4||n>int.MaxValue-p)throw new InvalidDataException("FlatBuffer offset");int q=p+(int)n;Check(q,1);return q;}
  static PropertyInfo[] Fields(Type t)=>t.GetProperties().Where(p=>p.GetCustomAttribute<Attributes.FlatBufferItemAttribute>()!=null).OrderBy(p=>p.GetCustomAttribute<Attributes.FlatBufferItemAttribute>()!.Index).ToArray();
  static bool IsStruct(Type t)=>t.GetCustomAttribute<Attributes.FlatBufferStructAttribute>()!=null;
  static bool IsUnion(Type t)=>t.IsGenericType&&t.GetGenericTypeDefinition()==typeof(FlatBufferUnion<,,,>);
  static int ScalarSize(Type t){if(t.IsEnum)t=Enum.GetUnderlyingType(t);if(t==typeof(bool)||t==typeof(byte)||t==typeof(sbyte))return 1;if(t==typeof(short)||t==typeof(ushort))return 2;if(t==typeof(int)||t==typeof(uint)||t==typeof(float))return 4;if(t==typeof(long)||t==typeof(ulong)||t==typeof(double))return 8;return 0;}
  static int Align(int n,int a)=>checked((n+a-1)/a*a);
  static (int size,int alignment) Layout(Type t,int depth=0){if(depth>32)throw new InvalidDataException("Struct recursion");int scalar=ScalarSize(t);if(scalar!=0)return(scalar,scalar);if(!IsStruct(t))throw new InvalidDataException("Unsupported inline type");int size=0,alignment=1;foreach(var f in Fields(t)){var l=Layout(f.PropertyType,depth+1);size=checked(Align(size,l.alignment)+l.size);alignment=Math.Max(alignment,l.alignment);}return(Align(size,alignment),alignment);}
  int Field(int table,int index){int delta=I32(table);long v=(long)table-delta;if(v<0||v>int.MaxValue)throw new InvalidDataException("Vtable offset");int vt=(int)v;int len=U16(vt), objectSize=U16(vt+2);if(len<4||(len&1)!=0||objectSize<4)throw new InvalidDataException("Vtable length");Check(vt,len);Check(table,objectSize);int entry=checked(4+index*2);if(entry>=len)return 0;int off=U16(vt+entry);if(off==0)return 0;if(off>=objectSize)throw new InvalidDataException("Field range");return checked(table+off);}
  object Scalar(Type t,int p){Type raw=t.IsEnum?Enum.GetUnderlyingType(t):t;Check(p,ScalarSize(raw));object v;
   if(raw==typeof(byte))v=data[p];else if(raw==typeof(sbyte))v=unchecked((sbyte)data[p]);else if(raw==typeof(bool)){if(data[p]>1)throw new InvalidDataException("Boolean");v=data[p]!=0;}
   else if(raw==typeof(ushort))v=U16(p);else if(raw==typeof(short))v=unchecked((short)U16(p));else if(raw==typeof(uint))v=U32(p);else if(raw==typeof(int))v=I32(p);else if(raw==typeof(float))v=BitConverter.Int32BitsToSingle(I32(p));else {ulong q=BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(p,8));if(raw==typeof(ulong))v=q;else if(raw==typeof(long))v=unchecked((long)q);else v=BitConverter.Int64BitsToDouble(unchecked((long)q));}return t.IsEnum?Enum.ToObject(t,v):v;
  }
  object Value(Type t,int p,int depth){if(depth>64||++objects>MaxObjects)throw new InvalidDataException("FlatBuffer complexity");if(ScalarSize(t)!=0)return Scalar(t,p);
   if(t==typeof(string)){int q=Follow(p);uint n=U32(q);if(n>MaxVector)throw new InvalidDataException("String size");Check(q+4,checked((int)n+1));if(data[q+4+(int)n]!=0)throw new InvalidDataException("String terminator");return new UTF8Encoding(false,true).GetString(data,q+4,(int)n);}
   Type? element=t.IsArray?t.GetElementType():t.IsGenericType&&t.GetGenericTypeDefinition()==typeof(IList<>)?t.GetGenericArguments()[0]:null;
   if(element!=null){int q=Follow(p);uint n=U32(q);if(n>MaxVector)throw new InvalidDataException("Vector size");int stride=IsStruct(element)?Layout(element).size:Math.Max(ScalarSize(element),4);if(ScalarSize(element)>0)stride=ScalarSize(element);Check(q+4,checked((int)n*stride));if(element==typeof(byte)){var bytes=new byte[n];Buffer.BlockCopy(data,q+4,bytes,0,(int)n);return bytes;}Array result=Array.CreateInstance(element,(int)n);for(int i=0;i<n;i++)result.SetValue(Value(element,checked(q+4+i*stride),depth+1),i);return result;}
   if(!IsStruct(t)&&t.GetCustomAttribute<Attributes.FlatBufferTableAttribute>()==null)throw new InvalidDataException("Unknown schema type "+t);
   int at=IsStruct(t)?p:Follow(p);return ObjectAt(t,at,depth+1);
  }
  object ObjectAt(Type t,int at,int depth){if(depth>64||++objects>MaxObjects)throw new InvalidDataException("FlatBuffer complexity");object obj=Activator.CreateInstance(t)!;int offset=0;
   if(IsStruct(t))Check(at,Layout(t).size);
   foreach(var f in Fields(t)){var attr=f.GetCustomAttribute<Attributes.FlatBufferItemAttribute>()!;int p;
    if(IsStruct(t)){var l=Layout(f.PropertyType);offset=Align(offset,l.alignment);p=checked(at+offset);offset=checked(offset+l.size);}else p=Field(at,attr.Index);
    if(IsUnion(f.PropertyType)){byte tag=p==0?(byte)0:(byte)Scalar(typeof(byte),p);var choices=f.PropertyType.GetGenericArguments();if(tag>choices.Length)throw new InvalidDataException("Union tag");object? value=null;if(tag!=0){int payload=Field(at,attr.Index+1);if(payload==0)throw new InvalidDataException("Missing union value");value=Value(choices[tag-1],payload,depth+1);}f.SetValue(obj,Activator.CreateInstance(f.PropertyType,new object?[]{tag,value}));continue;}
    if(p!=0)f.SetValue(obj,Value(f.PropertyType,p,depth+1));else if(attr.DefaultValue!=null)f.SetValue(obj,attr.DefaultValue);
   }return obj;
  }
  public T Read<T>() where T:class => (T)ObjectAt(typeof(T),Follow(0),0);
 }
}
