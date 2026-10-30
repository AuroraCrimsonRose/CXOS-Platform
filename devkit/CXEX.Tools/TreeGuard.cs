// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Security.Cryptography;
using System.Text;

namespace CXEX.Tools;

/// <summary>
/// The backup and restore under <c>cxk test sabotage</c>, owning what every
/// hand-written matrix script got wrong at least once (HANDOFF's mistakes
/// list): each file is backed up once per run, before its first edit, so two
/// edits to one file restore the original and not the half-sabotaged copy;
/// the backup is on disk with a manifest written before the file is touched,
/// so a run that dies mid-way is restored by the next one; a restore checks
/// each file's hash against the original's; and the tree's state (every
/// change git sees) is compared before and after.
/// </summary>
public sealed class TreeGuard
{
    private readonly string _repo;
    private readonly string _dir;
    private string Manifest => Path.Combine(_dir, "manifest.txt");
    private readonly HashSet<string> _backed = new(StringComparer.Ordinal);

    public TreeGuard(string repo, string stateDir)
    {
        _repo = repo;
        _dir = stateDir;
        Directory.CreateDirectory(_dir);
    }

    /// <summary>Whether an earlier run left files sabotaged (it died before restoring them).</summary>
    public bool Pending => File.Exists(Manifest);

    /// <summary>
    /// Applies a run's edits in order, backing each file up first. Null on
    /// success; otherwise why, with the tree already restored.
    /// </summary>
    public string? Apply(IEnumerable<SabotageEdit> edits)
    {
        foreach (var e in edits)
        {
            string path = Path.Combine(_repo, e.File);
            if (!File.Exists(path)) { Restore(); return $"{e.File} does not exist"; }
            string content = File.ReadAllText(path);
            var (result, problem) = SabotageFile.Apply(content, e);
            if (problem is not null) { Restore(); return problem; }
            Backup(e.File, path);
            File.WriteAllText(path, result);
        }
        return null;
    }

    private void Backup(string rel, string path)
    {
        if (!_backed.Add(rel)) return;   // once per run: the original, never a half-edited copy
        byte[] bytes = File.ReadAllBytes(path);
        string name = $"{_backed.Count:D3}.bak";
        File.WriteAllBytes(Path.Combine(_dir, name), bytes);
        // the manifest line goes to disk before the file is touched
        using var f = new FileStream(Manifest, FileMode.Append, FileAccess.Write, FileShare.None);
        byte[] line = Encoding.UTF8.GetBytes($"{name}\t{Hash(bytes)}\t{rel}\n");
        f.Write(line);
        f.Flush(flushToDisk: true);
    }

    /// <summary>
    /// Puts back every file the manifest names and checks each against its
    /// original's hash. Returns the problems (empty when all went back).
    /// </summary>
    public IReadOnlyList<string> Restore()
    {
        var problems = new List<string>();
        if (File.Exists(Manifest))
        {
            foreach (string line in File.ReadAllLines(Manifest))
            {
                string[] f = line.Split('\t', 3);
                if (f.Length != 3) { problems.Add($"a manifest line that is not 'backup hash path': {line}"); continue; }
                string bak = Path.Combine(_dir, f[0]), dst = Path.Combine(_repo, f[2]);
                if (!File.Exists(bak)) { problems.Add($"the backup of {f[2]} is missing"); continue; }
                byte[] bytes = File.ReadAllBytes(bak);
                if (Hash(bytes) != f[1]) { problems.Add($"the backup of {f[2]} is not the file it backed up"); continue; }
                File.WriteAllBytes(dst, bytes);
                if (Hash(File.ReadAllBytes(dst)) != f[1]) { problems.Add($"{f[2]} did not go back"); continue; }
                File.Delete(bak);
            }
            if (problems.Count == 0) File.Delete(Manifest);   // kept, with its backups, if anything stayed out
        }
        _backed.Clear();
        return problems;
    }

    /// <summary>
    /// Every change git sees in the tree (tracked diffs and untracked files,
    /// not ignored ones, so build output does not count), as one hash. Null
    /// if git could not say.
    /// </summary>
    public string? Fingerprint()
    {
        var status = ToolProcess.Run("git", ["-C", _repo, "status", "--porcelain=v1", "--untracked-files=all"]);
        var diff = ToolProcess.Run("git", ["-C", _repo, "diff", "--binary", "HEAD"]);
        if (!status.Ok || !diff.Ok) return null;
        var sb = new StringBuilder(status.StandardOutput).Append('\0').Append(diff.StandardOutput);
        // untracked files' contents too: a status line says only that one exists
        foreach (string l in status.StandardOutput.Split('\n'))
            if (l.StartsWith("?? ", StringComparison.Ordinal))
            {
                string p = Path.Combine(_repo, l[3..].Trim().Trim('"'));
                if (File.Exists(p)) sb.Append('\0').Append(Hash(File.ReadAllBytes(p)));
            }
        return Hash(Encoding.UTF8.GetBytes(sb.ToString()));
    }

    private static string Hash(byte[] b) => Convert.ToHexString(SHA256.HashData(b));
}
