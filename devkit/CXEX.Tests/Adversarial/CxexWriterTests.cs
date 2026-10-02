using CXEX.Build.Emitters;
using CXEX.Build.Engines;
using CXEX.Build.Layout;
using CXEX.Build.Parsers;
using CXEX.Core.Constants;
using CXEX.FileType.Types;
using Xunit;

namespace CXEX.Tests.Adversarial;

/// <summary>
/// The write side of the boundary. The reader refuses malformed images, but
/// nothing stopped the writer from producing one - and an image this repository
/// emitted and then signed would be trusted by whoever holds the key.
///
/// <para>The reviews ask the two sides to validate independently (security §11),
/// so these check the writer on its own terms rather than assuming the layout
/// engine ran first.</para>
/// </summary>
public class CxexWriterTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("cxexw_").FullName;

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { /* best effort */ }
        GC.SuppressFinalize(this);
    }

    private string Out(string name) => Path.Combine(_dir, name);

    private static CxexMemoryLayout ValidLayout()
    {
        byte[] code = Enumerable.Range(0, 64).Select(i => (byte)i).ToArray();
        return new CxexMemoryLayout
        {
            TypeCode = CXFlags.TYPE_USER,
            EntryPoint = 0x00400000,
            LoadBase = 0x00400000,
            ImageMin = 0x00400000,
            ImageMax = 0x00400040,
            PhysBase = 0x00400000,
            Flags = CXFlags.FLAG_EXECUTABLE,
            Sections =
            {
                new SectionLayout
                {
                    Name = ".text",
                    VirtualAddress = 0x00400000,
                    FileOffset = 56 + 28,
                    FileSize = 64,
                    MemSize = 64,
                    Flags = CXFlags.SEC_READ | CXFlags.SEC_EXEC,
                    Payload = code,
                },
            },
        };
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_valid_layout_writes_an_image_the_reader_accepts()
    {
        // The writer and the reader have to agree, or one of these suites is
        // testing a format the other does not produce.
        string path = Out("ok.xuex");
        CXEXWriter.WriteExecutable(path, ValidLayout());

        var exe = new CXEXExecutable();
        exe.Load(File.ReadAllBytes(path));

        var only = Assert.Single(exe.Sections);
        Assert.Equal(".text", only.Name);
        Assert.Equal(64, exe.GetSectionData(".text").Length);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_writable_executable_section_is_refused()
    {
        CxexMemoryLayout layout = ValidLayout();
        layout.Sections[0].Flags = CXFlags.SEC_READ | CXFlags.SEC_WRITE | CXFlags.SEC_EXEC;

        var ex = Assert.Throws<InvalidDataException>(() => CXEXWriter.WriteExecutable(Out("wx.xuex"), layout));
        Assert.Contains("W^X", ex.Message);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_payload_disagreeing_with_file_size_is_refused()
    {
        CxexMemoryLayout layout = ValidLayout();
        layout.Sections[0].FileSize = 128;   // payload is still 64 bytes

        Assert.Throws<InvalidDataException>(() => CXEXWriter.WriteExecutable(Out("short.xuex"), layout));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_section_offset_inside_the_section_table_is_refused()
    {
        CxexMemoryLayout layout = ValidLayout();
        layout.Sections[0].FileOffset = 60;   // inside header+table

        Assert.Throws<InvalidDataException>(() => CXEXWriter.WriteExecutable(Out("overlap.xuex"), layout));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void An_empty_layout_is_refused() =>
        Assert.Throws<InvalidDataException>(
            () => CXEXWriter.WriteExecutable(Out("empty.xuex"), new CxexMemoryLayout()));

    /// <summary>
    /// A failed write must not leave a partial image where the target belongs. The
    /// output is produced via a temporary file and moved into place, so an
    /// interrupted build cannot leave a truncated .xkex that still parses - and
    /// gets signed by the next step.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void A_refused_write_leaves_the_previous_image_intact()
    {
        string path = Out("keep.xuex");
        CXEXWriter.WriteExecutable(path, ValidLayout());
        byte[] good = File.ReadAllBytes(path);

        CxexMemoryLayout bad = ValidLayout();
        bad.Sections[0].Flags |= CXFlags.SEC_WRITE;   // now W+X, refused

        Assert.Throws<InvalidDataException>(() => CXEXWriter.WriteExecutable(path, bad));

        Assert.Equal(good, File.ReadAllBytes(path));
        Assert.Empty(Directory.GetFiles(_dir, "*.tmp*"));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void The_layout_engine_refuses_a_writable_executable_segment()
    {
        var seg = new ElfSegment
        {
            Vaddr = 0x00400000,
            Paddr = 0x00400000,
            FileSize = 16,
            MemSize = 16,
            Flags = 0x7,                       // R + W + X in ELF terms
            Data = new byte[16],
        };

        Assert.Throws<InvalidDataException>(
            () => CXEXLayoutEngine.CreateLayout(0x00400000, new[] { seg }, CXFlags.TYPE_USER));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Adversarial)]
    public void The_layout_engine_refuses_a_segment_leaving_the_address_space()
    {
        var seg = new ElfSegment
        {
            Vaddr = 0xFFFF_F000,
            Paddr = 0xFFFF_F000,
            FileSize = 0,
            MemSize = 0x8000,
            Flags = 0x6,
            Data = Array.Empty<byte>(),
        };

        Assert.Throws<InvalidDataException>(
            () => CXEXLayoutEngine.CreateLayout(0xFFFF_F000, new[] { seg }, CXFlags.TYPE_USER));
    }
}
