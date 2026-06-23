namespace CXEX.Core.FileTypes;

public static class CXFileRegistry
{
    private static readonly Dictionary<string, Type> _registry = new(StringComparer.OrdinalIgnoreCase)
    {
        { ".xkpk", typeof(Types.XKPKFile) },
        { ".xksk", typeof(Types.XKSKFile) },
        { ".xkex", typeof(Types.XKEXFile) },
        { ".xoex", typeof(Types.XOEXFile) },
        { ".xbpt", typeof(Types.XBPTFile) }
    };

    public static ICXFile Create(string filePath)
    {
        string ext = Path.GetExtension(filePath);
        if (!_registry.TryGetValue(ext, out var type))
            throw new NotSupportedException($"No handler for extension: {ext}");

        var instance = (ICXFile)Activator.CreateInstance(type)!;
        instance.Load(File.ReadAllBytes(filePath));
        return instance;
    }
}