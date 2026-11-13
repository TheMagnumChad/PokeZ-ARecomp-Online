using OpenTK.Mathematics;
namespace GFTool.Renderer.Scene.GraphicsObjects;
// Row-vector convention: compensation changes axes, not the authored joint offset.
public static class SegmentScaleTransform {
    public static Matrix4 Compose(Vector3 scale, Quaternion rotation, Vector3 translation, Vector3 parentAuthoredScale) {
        var inverse = new Vector3(parentAuthoredScale.X != 0 ? 1 / parentAuthoredScale.X : 1,
                                  parentAuthoredScale.Y != 0 ? 1 / parentAuthoredScale.Y : 1,
                                  parentAuthoredScale.Z != 0 ? 1 / parentAuthoredScale.Z : 1);
        return Matrix4.CreateScale(scale) * Matrix4.CreateFromQuaternion(rotation)
            * Matrix4.CreateScale(inverse) * Matrix4.CreateTranslation(translation);
    }
}
