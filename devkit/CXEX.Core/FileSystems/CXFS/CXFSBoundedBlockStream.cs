using System;
using System.IO;

namespace CXEX.Core.FileSystems.CXFS;

public class CXFSBoundedBlockStream : Stream
{
    private readonly CXFSImage _image;
    private readonly Stream _baseStream;
    private readonly CxfsManifestEntry _entry;
    private long _position;

    public CXFSBoundedBlockStream(CXFSImage image, CxfsManifestEntry entry)
    {
        _image = image;
        _baseStream = FileHelpers.GetPrivateStreamField(image);
        _entry = entry;
    }

    public override bool CanRead => true;
    public override bool CanSeek => true;
    public override bool CanWrite => false;
    public override long Length => (long)_entry.Size;

    public override long Position
    {
        get => _position;
        set
        {
            if (value < 0 || value > Length) throw new ArgumentOutOfRangeException(nameof(value));
            _position = value;
        }
    }

    public override int Read(byte[] buffer, int offset, int count)
    {
        if (_position >= Length) return 0;

        long bytesRemaining = Length - _position;
        int toRead = (int)Math.Min(count, bytesRemaining);
        if (toRead <= 0) return 0;

        int totalRead = 0;
        long targetOffset = _position;

        unsafe
        {
            long currentExtentOffset = 0;
            for (int i = 0; i < 8; i++)
            {
                uint startBlock = _entry.ExtentStart[i];
                uint blockLen = _entry.ExtentLen[i];
                if (blockLen == 0) break;

                long extentByteSize = (long)blockLen * _image.Superblock.BlockSize;

                if (targetOffset >= currentExtentOffset && targetOffset < currentExtentOffset + extentByteSize)
                {
                    long offsetInExtent = targetOffset - currentExtentOffset;
                    long diskBlockNum = startBlock + (offsetInExtent / _image.Superblock.BlockSize);
                    long diskBlockOffset = offsetInExtent % _image.Superblock.BlockSize;

                    long baseDiskPos = _image.CalculateByteOffset(diskBlockNum) + diskBlockOffset;
                    long bytesAvailableInExtent = extentByteSize - offsetInExtent;
                    int chunkToRead = (int)Math.Min(toRead - totalRead, bytesAvailableInExtent);

                    _baseStream.Position = baseDiskPos;
                    int readThisPass = _baseStream.Read(buffer, offset + totalRead, chunkToRead);

                    totalRead += readThisPass;
                    targetOffset += readThisPass;
                    _position += readThisPass;

                    if (totalRead == toRead || readThisPass == 0) break;
                }
                currentExtentOffset += extentByteSize;
            }
        }

        return totalRead;
    }

    public override long Seek(long offset, SeekOrigin origin)
    {
        long newPos = origin switch
        {
            SeekOrigin.Begin => offset,
            SeekOrigin.Current => _position + offset,
            SeekOrigin.End => Length + offset,
            _ => throw new ArgumentException("Invalid processing origin marker.")
        };

        if (newPos < 0 || newPos > Length) throw new IOException("Attempted file seek outside structure allocation tables.");
        _position = newPos;
        return _position;
    }

    public override void Flush() { }
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}

// Global Helper context to safely fetch base stream operations without reflection breakage
internal static class FileHelpers
{
    public static Stream GetPrivateStreamField(CXFSImage img)
    {
        var field = typeof(CXFSImage).GetField("_stream", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        return (Stream)field!.GetValue(img)!;
    }
}