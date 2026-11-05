// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Globalization;

namespace CXEX.Tools;

/// <summary>One report line as <c>cxk hw</c> names it: what it is about, and its fields by name.</summary>
public sealed record HwFact(string Section, string Key, IReadOnlyList<(string Name, string Value)> Fields);

/// <summary>
/// A difference between two reports. <see cref="Field"/> is null when the whole
/// line is in one report only (<see cref="A"/> or <see cref="B"/> null).
/// <see cref="Expected"/> is a difference two machines, or two boots of one,
/// always show, with <see cref="Why"/> saying which kind; the rest are findings.
/// </summary>
public sealed record HwDifference(string Key, string? Field, string? A, string? B, bool Expected, string Why);

/// <summary>
/// Reading and comparing hardware reports (roadmap 1.8b,
/// docs/hardware/CX_HW_REPORT.md §5): each line named by what it describes
/// (a PCI function by its address, a table by its signature, a MADT entry by
/// its CPU or bus), so two reports are compared fact by fact rather than line
/// by line, and each difference judged expected or a finding.
///
/// <para>Expected: addresses the firmware chooses (the RSDP and every table,
/// SMBIOS, the I/O and local APIC, a BAR whose type bits agree, a PCI
/// interrupt line), anything the hypervisor says about itself, and a measured
/// rate within the tolerance. Everything else is a finding, unknown sections
/// included: a field this tool cannot name still differs.</para>
/// </summary>
public static class HwDiff
{
    /// <summary>How far two measured rates may differ and still be the same clock: 1%, the
    /// bound the kernel's own TSC check uses (ktest_platform_hw_report).</summary>
    public const double DefaultRateTolerance = 0.01;

    private static readonly string[] PciFields =
        ["vendor", "device", "class", "subclass", "prog-if", "header", "subvendor", "subdevice",
         "irq-line", "irq-pin", "bar0", "bar1", "bar2", "bar3", "bar4", "bar5"];

    /// <summary>The report's lines as facts, begin and end left out, a repeated key numbered (<c>#2</c>).</summary>
    public static IReadOnlyList<HwFact> Facts(HwReport r)
    {
        var seen = new Dictionary<string, int>();
        var facts = new List<HwFact>();
        foreach (var l in r.Lines)
        {
            if (l.Section is "begin" or "end") continue;
            var (key, names, skip) = Shape(l);
            int n = seen[key] = seen.GetValueOrDefault(key) + 1;
            if (n > 1) key += " #" + n;
            var fields = new List<(string, string)>();
            for (int i = skip; i < l.Fields.Count; i++)
                fields.Add((i - skip < names.Length ? names[i - skip] : $"f{i}", l.Fields[i]));
            facts.Add(new HwFact(l.Section, key, fields));
        }
        return facts;
    }

    /// <summary>A line's key, its fields' names, and how many leading fields the key consumed.</summary>
    private static (string Key, string[] Names, int Skip) Shape(HwLine l)
    {
        var f = l.Fields;
        string F(int i) => i < f.Count ? f[i] : "-";
        switch (l.Section)
        {
            case "boot": return ("boot", ["firmware", "flags", "smbios"], 0);
            case "cpu": return ("cpu", ["vendor", "family", "model", "stepping", "brand"], 0);
            case "cpuid": return ($"cpuid {F(0)}.{F(1)}", ["eax", "ebx", "ecx", "edx"], 2);
            case "hypervisor": return ("hypervisor", ["signature", "max-leaf"], 0);
            case "apic": return ("apic", ["ioapic-on", "lapic-on", "lapic-id"], 0);
            case "mem": return ($"mem {F(0)}", ["length", "type", "attr"], 1);
            case "memsum": return ("memsum", ["entries", "dropped", "usable"], 0);
            case "pcisum": return ("pcisum", ["functions", "max"], 0);
            case "pci": return ($"pci {F(0)}", PciFields, 1);
            case "clock": return ($"clock {F(0)}", F(0) == "pmtimer" ? ["port", "bits"] : ["value"], 1);
            case "truncated": return ("truncated", [], 0);
            case "acpi":
                return F(0) switch
                {
                    "rsdp" => ("acpi rsdp", ["phys", "rev", "oem", "rsdt", "xsdt", "sum", "xsum"], 1),
                    "table" => ($"acpi {F(1)} {F(2)}", ["phys", "len", "rev", "oem", "oem-table", "oem-rev", "state"], 3),
                    _ => ($"acpi {F(0)}", ["phys"], 1),
                };
            case "madt":
                return F(0) switch
                {
                    "head" => ("madt head", ["lapic-addr", "flags"], 1),
                    "cpu" => ($"madt cpu {F(1)}", ["apic-id", "flags"], 2),
                    "x2cpu" => ($"madt x2cpu {F(1)}", ["x2apic-id", "flags"], 2),
                    "ioapic" => ($"madt ioapic {F(1)}", ["addr", "gsi-base"], 2),
                    "iso" => ($"madt iso {F(1)}.{F(2)}", ["gsi", "flags"], 3),
                    // nmi <uid> <flags> <lint>: one per CPU and pin
                    "nmi" or "x2nmi" => ($"madt {F(0)} {F(1)} lint{F(3)}", ["flags", "lint"], 2),
                    "lapicaddr" => ("madt lapicaddr", ["addr"], 1),
                    "other" => ($"madt other {F(1)}", ["len"], 2),
                    "bad" => ($"madt bad {F(1)}", ["type", "len"], 2),
                    _ => ($"madt {F(0)}", [], 1),
                };
            default: return (l.Section, [], 0);
        }
    }

    /// <summary>Every difference between <paramref name="a"/> and <paramref name="b"/>, in A's order, then B's extra lines.</summary>
    public static IReadOnlyList<HwDifference> Compare(HwReport a, HwReport b, double rateTolerance = DefaultRateTolerance)
    {
        var fa = Facts(a);
        var fb = Facts(b).ToDictionary(x => x.Key);
        var inA = new HashSet<string>();
        var diffs = new List<HwDifference>();
        foreach (var x in fa)
        {
            inA.Add(x.Key);
            if (!fb.TryGetValue(x.Key, out var y))
            {
                diffs.Add(Whole(x, inA: true));
                continue;
            }
            int n = Math.Max(x.Fields.Count, y.Fields.Count);
            for (int i = 0; i < n; i++)
            {
                string name = i < x.Fields.Count ? x.Fields[i].Name : y.Fields[i].Name;
                string? va = i < x.Fields.Count ? x.Fields[i].Value : null;
                string? vb = i < y.Fields.Count ? y.Fields[i].Value : null;
                if (va == vb) continue;
                var (expected, why) = Judge(x, name, va, vb, i, rateTolerance);
                diffs.Add(new HwDifference(x.Key, name, va, vb, expected, why));
            }
        }
        foreach (var y in Facts(b))
            if (!inA.Contains(y.Key)) diffs.Add(Whole(y, inA: false));
        return diffs;
    }

    private static HwDifference Whole(HwFact f, bool inA)
    {
        string text = string.Join(' ', f.Fields.Select(x => x.Value));
        bool hv = IsHypervisor(f);
        return new HwDifference(f.Key, null, inA ? text : null, inA ? null : text, hv,
                                hv ? "the hypervisor's own leaves" : inA ? "only in A" : "only in B");
    }

    private static bool IsHypervisor(HwFact f) =>
        f.Section == "hypervisor" || (f.Section == "cpuid" && f.Key.StartsWith("cpuid 4", StringComparison.Ordinal));

    private static (bool Expected, string Why) Judge(HwFact f, string name, string? a, string? b, int index, double tol)
    {
        if (a is null || b is null) return (false, "a field in one report only");
        if (IsHypervisor(f)) return (true, "the hypervisor's own leaves");
        switch (f.Section)
        {
            case "boot" when name == "smbios":
            case "acpi" when name is "phys" or "rsdt" or "xsdt":
            case "madt" when name is "lapic-addr" or "addr":
                return (true, "an address the firmware chose");
            case "pci" when name == "irq-line":
                return (true, "an interrupt line the firmware routed");
            case "pci" when name.StartsWith("bar", StringComparison.Ordinal):
                return Bar(f, index, a, b);
            case "cpuid" when Hex(a) is ulong x && Hex(b) is ulong y:
                return (false, $"bits {x ^ y:x8} differ");
            case "clock" when f.Key.EndsWith("-hz", StringComparison.Ordinal):
                if (!ulong.TryParse(a, out ulong ra) || !ulong.TryParse(b, out ulong rb) || ra == 0 || rb == 0)
                    return (false, "measured in one report only");
                double off = Math.Abs((double)ra - rb) / Math.Max(ra, rb);
                return off <= tol
                    ? (true, $"a measured rate, {Percent(off)} apart")
                    : (false, $"a measured rate, {Percent(off)} apart (over {Percent(tol)})");
        }
        return (false, "differs");
    }

    /// <summary>A BAR's address is the firmware's choice; its type bits (I/O or memory,
    /// 32 or 64-bit, prefetchable) are the device's. The dword after a 64-bit memory
    /// BAR is that BAR's high half: all address.</summary>
    private static (bool, string) Bar(HwFact f, int index, string a, string b)
    {
        if (Hex(a) is not ulong x || Hex(b) is not ulong y) return (false, "differs");
        int bar = index - Array.IndexOf(PciFields, "bar0");
        if (bar > 0 && IsMem64(f, bar - 1)) return (true, "an address the firmware chose (a 64-bit BAR's high half)");
        if (x == 0 || y == 0) return (false, "a BAR set in one report only");
        ulong mask = (x & 1) == 1 ? 0x3u : 0xFu;
        return (x & mask) == (y & mask)
            ? (true, "an address the firmware chose")
            : (false, $"the BAR's type bits differ ({x & mask:x} vs {y & mask:x})");
    }

    private static bool IsMem64(HwFact f, int bar)
    {
        var v = f.Fields.FirstOrDefault(x => x.Name == $"bar{bar}").Value;
        return v is not null && Hex(v) is ulong x && (x & 1) == 0 && ((x >> 1) & 3) == 2;
    }

    private static ulong? Hex(string s) =>
        ulong.TryParse(s, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out ulong v) ? v : null;

    private static string Percent(double v) => (v * 100).ToString("0.###", CultureInfo.InvariantCulture) + "%";

    /// <summary>
    /// What one report says is wrong with its own machine, without a second to
    /// compare it with: a cut report, a checksum that fails, a table not read,
    /// a MADT entry skipped, memory entries dropped, a full PCI table, and the
    /// TSC measured against the tick and against the PM timer disagreeing.
    /// </summary>
    public static IReadOnlyList<string> Notes(HwReport r, double rateTolerance = DefaultRateTolerance)
    {
        var notes = new List<string>();
        if (r.Truncated) notes.Add("the report was cut: it ran out of room in the kernel");
        string? tick = null, pm = null;
        foreach (var f in Facts(r))
        {
            string V(string name) => f.Fields.FirstOrDefault(x => x.Name == name).Value ?? "-";
            switch (f.Section)
            {
                case "acpi" when f.Key == "acpi rsdp":
                    if (V("sum") != "ok") notes.Add($"the RSDP's checksum is {V("sum")}");
                    if (V("xsum") is not ("ok" or "-")) notes.Add($"the RSDP's extended checksum is {V("xsum")}");
                    break;
                case "acpi" when f.Key is "acpi none" or "acpi rsdp-bad":
                    notes.Add(f.Key == "acpi none" ? "no RSDP found" : $"the RSDP at {V("phys")} fails its checksum");
                    break;
                case "acpi":
                    if (V("state") is not ("ok" or "nosum")) notes.Add($"{f.Key} at {V("phys")} is {V("state")}");
                    break;
                case "madt" when f.Key.StartsWith("madt bad", StringComparison.Ordinal):
                    notes.Add($"a MADT entry at offset {f.Key[9..]} (type {V("type")}, length {V("len")}) was not read");
                    break;
                case "memsum" when V("dropped") != "0":
                    notes.Add($"the loader dropped {V("dropped")} memory-map entries");
                    break;
                case "pcisum" when V("functions") == V("max"):
                    notes.Add($"the PCI table is full ({V("max")}): functions past it are missing");
                    break;
                case "clock" when f.Key == "clock tsc-tick-hz": tick = V("value"); break;
                case "clock" when f.Key == "clock tsc-pm-hz": pm = V("value"); break;
            }
        }
        if (tick is not null && pm is not null)
        {
            if (pm == "-") notes.Add("the TSC was not measured against the PM timer");
            else if (ulong.TryParse(tick, out ulong t) && ulong.TryParse(pm, out ulong p) && t > 0 && p > 0)
            {
                double off = Math.Abs((double)t - p) / Math.Max(t, p);
                if (off > rateTolerance)
                    notes.Add($"the TSC by the tick ({t} Hz) and by the PM timer ({p} Hz) are {Percent(off)} apart: one clock is wrong");
            }
        }
        return notes;
    }
}
