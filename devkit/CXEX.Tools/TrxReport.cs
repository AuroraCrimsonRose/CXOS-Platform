// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Xml.Linq;

namespace CXEX.Tools;

/// <summary>One test from a TRX file: its full name and outcome (Passed, Failed, NotExecuted...).</summary>
public sealed record TrxTest(string Name, string Outcome);

/// <summary>
/// The results <c>dotnet test --logger trx</c> writes, for <c>cxk test all</c>.
/// Read from the file, not the console: a console summary is missing when
/// the build failed or no test ran, and its absence was once read as a pass
/// (HANDOFF: <c>--no-build</c> in a fresh container ran zero tests and exited 0).
/// </summary>
public sealed record TrxReport(IReadOnlyList<TrxTest> Tests)
{
    public int Passed => Tests.Count(t => t.Outcome == "Passed");
    public int Failed => Tests.Count(t => t.Outcome is "Failed" or "Error" or "Timeout" or "Aborted");
    public int Skipped => Tests.Count - Passed - Failed;

    public static TrxReport? Parse(string xml)
    {
        try
        {
            var doc = XDocument.Parse(xml);
            var tests = doc.Descendants().Where(e => e.Name.LocalName == "UnitTestResult")
                .Select(e => new TrxTest((string?)e.Attribute("testName") ?? "", (string?)e.Attribute("outcome") ?? ""))
                .ToList();
            return new TrxReport(tests);
        }
        catch (System.Xml.XmlException)
        {
            return null;
        }
    }

    /// <summary>The tests <paramref name="keep"/> accepts.</summary>
    public TrxReport Where(Func<TrxTest, bool> keep) => new(Tests.Where(keep).ToList());
}
