using CXEX.FileType.Types;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// Every CXEX this repository actually produces must survive the validator.
///
/// <para>Tightening a parser is the easy half; not rejecting real output is the
/// half that breaks builds. The adversarial suite proves malformed images are
/// refused, and proves nothing about the ones that matter - a validator that
/// rejects everything passes it completely. This loads whatever the last build
/// left in <c>dist/</c>, which is the only set of images whose correctness is not
/// this suite's own opinion.</para>
///
/// <para>It skips, with its reason, when the OS has not been built. That is the
/// honest outcome: it has checked nothing, and says so, rather than reporting a
/// pass for an empty directory.</para>
/// </summary>
public class RealImageTests
{
    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_packaged_image_loads_and_validates()
    {
        string root = Requires.Repo();
        string packages = Path.Combine(root, "dist", "CXK_x86_32", "packages");

        if (!Directory.Exists(packages))
            Assert.Skip($"no packaged images at {packages}: run `cxk os build --dev` first.");

        string[] images = Directory.GetFiles(packages, "*.x*ex");
        if (images.Length == 0)
            Assert.Skip($"{packages} exists but holds no .x*ex images: run `cxk os build --dev` first.");

        foreach (string path in images)
        {
            var exe = new CXEXExecutable();
            byte[] bytes = File.ReadAllBytes(path);

            Exception? ex = Record.Exception(() => exe.Load(bytes));
            Assert.True(ex is null,
                $"{Path.GetFileName(path)} is real build output but the validator rejected it: {ex?.Message}");

            Assert.NotEmpty(exe.Sections);

            // Each section must hand back exactly the bytes it claims, which also
            // exercises the bounds check in GetSectionData against real offsets.
            foreach (var sec in exe.Sections)
            {
                if (sec.FileSize == 0) continue;
                Assert.Equal((int)sec.FileSize, exe.GetSectionData(sec.Name).Length);
            }
        }
    }
}
