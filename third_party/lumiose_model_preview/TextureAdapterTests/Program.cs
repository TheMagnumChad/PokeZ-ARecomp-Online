using GFTool.Renderer.Scene.GraphicsObjects;
static void Check(bool b, string name) { if (!b) throw new Exception(name); }
// Distinct encoded values at manually specified Tegra GOB boundary addresses.
byte[] src = new byte[1024];
var points = new[] {(0,0,0), (16,0,32), (32,0,256), (0,1,16), (0,2,64), (0,8,512), (48,7,496)};
for(int i=0;i<points.Length;i++) src[points[i].Item3]=(byte)(i+1);
var dst=LocalBntxTexture.Untile(src,16,16,4,1,false);
for(int i=0;i<points.Length;i++) Check(dst[points[i].Item2*64+points[i].Item1]==i+1,"GOB address "+i);
byte[] linear=new byte[64]; linear[0]=1; linear[11]=2; linear[32]=3; linear[43]=4;
var lined=LocalBntxTexture.Untile(linear,3,2,4,0,true);
Check(lined.Length==24 && lined[0]==1 && lined[11]==2 && lined[12]==3 && lined[23]==4,"Linear row padding");
bool caught=false; try{LocalBntxTexture.Untile(new byte[1],4,4,16,0,false);}catch(InvalidDataException){caught=true;}
Check(caught,"Truncated texture rejected");
caught=false; try{LocalBntxTexture.Untile(src,0,16,4,0,false);}catch(InvalidDataException){caught=true;}
Check(caught,"Invalid dimensions rejected");
Console.WriteLine("Texture adapter synthetic tests passed (7 GOB offsets, padded linear, truncation, invalid dimensions). No GL context or game assets used.");
