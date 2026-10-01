namespace CXEX.Core.Interfaces;

public interface ICXFile
{
    string GetDisplayName();
    void Load(byte[] data);
}