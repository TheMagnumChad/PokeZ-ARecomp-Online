using Lumiose.ReadOnlyFormat.Attributes;
namespace Trinity.Core.Flatbuffers.TR.Model;
// Only the schema fields required by the Gfx2 adapter. Other legacy fields
// do not describe Gfx2 data and must not be eagerly interpreted as strings.
[FlatBufferTable]
public class MaterialSamplerProjection {
 [FlatBufferItem(0)] public string Name {get;set;} = "";
 [FlatBufferItem(14)] public TRMaterialIntExtra IntExtra {get;set;}
 [FlatBufferItem(3)] public TRSampler[] Samplers {get;set;} = Array.Empty<TRSampler>();
}
[FlatBufferTable]
public class MaterialSamplerRoot {
 [FlatBufferItem(1)] public MaterialSamplerProjection[] Materials {get;set;} = Array.Empty<MaterialSamplerProjection>();
}
