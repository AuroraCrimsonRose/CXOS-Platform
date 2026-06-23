namespace CXEX.Core.FileTypes;

public interface ICXFile
{
    // Loads the file from raw bytes; throws exception on validation failure.
    void Load(byte[] data);

    // Returns the file's primary metadata if applicable.
    string GetDisplayName();
}