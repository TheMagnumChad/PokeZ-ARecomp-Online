
using GFTool.Core.Utils;
using GFTool.Renderer.Core;
using OpenTK.Graphics.OpenGL4;
using System.IO;


using Trinity.Core.Flatbuffers.TR.Model;

namespace GFTool.Renderer.Scene.GraphicsObjects
{
    public class Texture : IDisposable
    {
        private class CachedTexture
        {
            public int TextureId; public TextureTarget Target;
            public int RefCount;
        }

        private static readonly Dictionary<string, CachedTexture> cache = new Dictionary<string, CachedTexture>(StringComparer.OrdinalIgnoreCase);
        private static readonly object cacheLock = new object();

        public string Name { get; private set; }
        public string SourceFile { get; private set; }
        public uint Slot { get; private set; }
        public int textureId { get; private set; }
        public TextureTarget Target { get; private set; } = TextureTarget.Texture2D;

        private readonly string cacheKey;
        private readonly string texturePath;
        private readonly string altTexturePath;
        private readonly string preferredName;

        public Texture(PathString modelPath, TRTexture img)
        {
            Name = img.Name;
            SourceFile = img.File;
            Slot = img.Slot;
            string texturePath;
            try
            {
                texturePath = Path.GetFullPath(modelPath.Combine(img.File));
            }
            catch
            {
                texturePath = modelPath.Combine(img.File);
            }
            var preferredName = Path.GetFileNameWithoutExtension(img.File);
            this.texturePath = texturePath;
            try
            {
                altTexturePath = Path.GetFullPath(modelPath.Combine(Path.GetFileName(img.File)));
            }
            catch
            {
                altTexturePath = modelPath.Combine(Path.GetFileName(img.File));
            }
            this.preferredName = preferredName;
            var keyPath = File.Exists(texturePath) ? texturePath : (File.Exists(altTexturePath) ? altTexturePath : texturePath);
            cacheKey = $"{keyPath}|{preferredName}";

            lock (cacheLock)
            {
                if (cache.TryGetValue(cacheKey, out var cached))
                {
                    cached.RefCount++;
                    textureId = cached.TextureId; Target = cached.Target;
                    return;
                }
            }
        }

        public void EnsureLoaded()
        {
            if (textureId > 0)
            {
                return;
            }

            // Materials are created before their first draw. Another wrapper may
            // have populated this key since this wrapper's constructor ran.
            lock (cacheLock)
            {
                if (cache.TryGetValue(cacheKey, out var cached))
                {
                    cached.RefCount++;
                    textureId = cached.TextureId; Target = cached.Target;
                    return;
                }
            }

            if (!TryGetResolvedSourcePath(out var source))
                throw new FileNotFoundException($"Required local texture missing: {SourceFile}");
            int id = LocalBntxTexture.Create(source, preferredName, out var target); Target = target;
            // Set texture parameters
            GL.TexParameter(Target, TextureParameterName.TextureMinFilter, (int)TextureMinFilter.Linear);
            GL.TexParameter(Target, TextureParameterName.TextureMagFilter, (int)TextureMagFilter.Linear);
            GL.TexParameter(Target, TextureParameterName.TextureWrapS, (int)TextureWrapMode.ClampToEdge);
            GL.TexParameter(Target, TextureParameterName.TextureWrapT, (int)TextureWrapMode.ClampToEdge);


            lock (cacheLock)
            {
                cache[cacheKey] = new CachedTexture { TextureId = id, Target = Target, RefCount = 1 };
            }

            textureId = id;
        }

        public bool TryGetResolvedSourcePath(out string path)
        {
            if (File.Exists(texturePath))
            {
                path = texturePath;
                return true;
            }

            if (File.Exists(altTexturePath))
            {
                path = altTexturePath;
                return true;
            }

            path = texturePath;
            return false;
        }

        public void Dispose()
        {
            if (textureId <= 0)
            {
                return;
            }

            lock (cacheLock)
            {
                if (!cache.TryGetValue(cacheKey, out var cached) || cached.TextureId != textureId)
                {
                    GL.DeleteTexture(textureId);
                    textureId = 0;
                    return;
                }

                cached.RefCount--;
                if (cached.RefCount <= 0)
                {
                    GL.DeleteTexture(cached.TextureId);
                    cache.Remove(cacheKey);
                }
            }
            textureId = 0;
        }
    }
}


