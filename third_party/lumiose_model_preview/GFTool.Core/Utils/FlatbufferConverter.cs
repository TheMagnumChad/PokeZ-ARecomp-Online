namespace Trinity.Core.Utils {
public static class FlatBufferConverter {
 public static T DeserializeFrom<T>(string file) where T:class { var i=new FileInfo(file); if(i.Length>134217728)throw new InvalidDataException("Model too large"); return DeserializeFrom<T>(File.ReadAllBytes(file)); }
 public static T DeserializeFrom<T>(byte[] data) where T:class => new Lumiose.ReadOnlyFormat.Reader(data).Read<T>();
 public static byte[] SerializeFrom<T>(T obj) where T:class => throw new NotSupportedException("Read-only model viewer");
}}
