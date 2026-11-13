using OpenTK.Graphics.OpenGL4;
namespace GFTool.Renderer.Scene.GraphicsObjects;
internal static class MaterialRasterState {
 internal static void Apply(int state) {
  if (state == 0) GL.Disable(EnableCap.CullFace);
  else { GL.Enable(EnableCap.CullFace); GL.CullFace(state == 1 ? CullFaceMode.Front : CullFaceMode.Back); }
 }
 internal static void RestoreDefault() { GL.Enable(EnableCap.CullFace); GL.CullFace(CullFaceMode.Back); }
}
