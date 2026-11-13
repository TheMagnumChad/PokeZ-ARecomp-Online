byte[] p={12,34,56,0,64,32,0,128,10,20,30,255};
FramePixels.Unpremultiply(p);
if(!p.AsSpan(0,4).SequenceEqual(new byte[]{0,0,0,0}))throw new Exception("transparent background");
if(!p.AsSpan(4,4).SequenceEqual(new byte[]{128,64,0,128}))throw new Exception("partial coverage lost");
if(!p.AsSpan(8,4).SequenceEqual(new byte[]{10,20,30,255}))throw new Exception("opaque changed");
bool rejected=false;try{FramePixels.Unpremultiply(new byte[3]);}catch(ArgumentException){rejected=true;}
if(!rejected)throw new Exception("malformed RGBA accepted");
// Correct source-over coverage for two half-opacity layers is .75, not .5 or1.
float a=.5f+.5f*(1-.5f);if(Math.Abs(a-.75f)>1e-6)throw new Exception("coverage blend");
Console.WriteLine("Pixel alpha tests PASS");
// IEC sRGB output transfer, with unpremultiply before transfer and quantization.
void Check(float[] input, byte[] expected, string label) {
    byte[] actual = FramePixels.LinearPremultipliedToSrgbStraight(input);
    if (!actual.AsSpan().SequenceEqual(expected)) throw new Exception(label + ": " + string.Join(",", actual));
}
Check(new float[]{0,0.18f,1,1},new byte[]{0,118,255,255},"opaque linear-to-sRGB");
Check(new float[]{0.25f,0.09f,0,0.5f},new byte[]{188,118,0,128},"partial coverage");
Check(new float[]{1,1,1,0},new byte[]{0,0,0,0},"zero alpha clears RGB");
Check(new float[]{0.0031308f,0.003f,0.5f,1},new byte[]{10,10,188,255},"transfer breakpoint");
Check(new float[]{-1,float.NaN,float.PositiveInfinity,1},new byte[]{0,0,255,255},"invalid and HDR channels");
Check(new float[]{1,1,1,float.NaN},new byte[]{0,0,0,0},"invalid coverage");
bool invalidFloats=false;try { FramePixels.LinearPremultipliedToSrgbStraight(new float[3]); } catch(ArgumentException) { invalidFloats=true; }
if(!invalidFloats) throw new Exception("malformed float pixels accepted");
Console.WriteLine("Linear to sRGB pixel tests PASS");
