using System.Collections.Concurrent;
using System.Text.RegularExpressions;

namespace StrataForge.Core.Requirements;

/// <summary>
/// Represents a single formal requirement parsed from a StrictDoc (<c>.sdoc</c>) specification file.
/// </summary>
/// <param name="Uid">Unique requirement identifier (for example, <c>STRATA-SYS-001</c>).</param>
/// <param name="Status">Lifecycle implementation status (<c>Implemented</c>, <c>Partially Implemented</c>, or <c>Planned</c>).</param>
/// <param name="Title">Concise requirement title.</param>
/// <param name="FilePath">Absolute filesystem path to the defining <c>.sdoc</c> document.</param>
/// <param name="Artifact">Optional repository-relative path to the implementing source or verification artifact.</param>
/// <param name="Parents">Ordered list of parent requirement UIDs referenced in <c>RELATIONS</c>.</param>
public sealed record RequirementNode(
    string Uid,
    string Status,
    string Title,
    string FilePath,
    string? Artifact,
    IReadOnlyList<string> Parents);

/// <summary>
/// Aggregated result of validating a repository's StrictDoc requirements graph.
/// </summary>
/// <param name="SpecFileCount">Total number of <c>.sdoc</c> files parsed.</param>
/// <param name="RequirementCount">Total number of unique requirements validated.</param>
/// <param name="ImplementedCount">Count of requirements with status <c>Implemented</c>.</param>
/// <param name="PartiallyImplementedCount">Count of requirements with status <c>Partially Implemented</c>.</param>
/// <param name="PlannedCount">Count of requirements with status <c>Planned</c>.</param>
/// <param name="Errors">All syntax, traceability, artifact, or cycle errors discovered (empty on success).</param>
public sealed record StrictDocValidationReport(
    int SpecFileCount,
    int RequirementCount,
    int ImplementedCount,
    int PartiallyImplementedCount,
    int PlannedCount,
    IReadOnlyList<string> Errors)
{
    /// <summary>
    /// Gets a value indicating whether the entire StrictDoc specification tree passed all checks.
    /// </summary>
    /// <value><see langword="true"/> when <see cref="Errors"/> is empty; otherwise, <see langword="false"/>.</value>
    public bool IsSuccess => Errors.Count == 0;
}

/// <summary>
/// Multithreaded StrictDoc (<c>.sdoc</c>) parser, traceability graph auditor, and artifact existence verifier.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-VER-004</c> by parsing all <c>.sdoc</c> files concurrently using
/// <see cref="Parallel.ForEachAsync{TSource}(IEnumerable{TSource}, CancellationToken, Func{TSource, CancellationToken, ValueTask})"/>,
/// checking UID uniqueness, verifying non-orphan parent relations, detecting parent-child cycles via DFS,
/// and validating every declared <c>ARTIFACT</c> file path against the repository root.
/// </remarks>
public static partial class StrictDocVerifier
{
    private static readonly string[] RequiredFields =
        ["UID", "STATUS", "TITLE", "STATEMENT", "RATIONALE", "VERIFICATION_METHOD"];

    private static readonly HashSet<string> ValidStatuses = new(StringComparer.Ordinal)
    {
        "Implemented",
        "Partially Implemented",
        "Planned",
    };

    /// <summary>
    /// Parses a single StrictDoc (<c>.sdoc</c>) file into requirement nodes and syntax diagnostics.
    /// </summary>
    /// <param name="path">Absolute or relative path to the <c>.sdoc</c> file.</param>
    /// <param name="cancellationToken">Cancellation token for cooperative cancellation.</param>
    /// <returns>A tuple of parsed <see cref="RequirementNode"/> entries and any syntax errors.</returns>
    public static async Task<(IReadOnlyList<RequirementNode> Requirements, IReadOnlyList<string> Errors)> ParseFileAsync(
        string path,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);

        string[] lines = await File.ReadAllLinesAsync(path, cancellationToken).ConfigureAwait(false);
        List<RequirementNode> reqs = [];
        List<string> errors = [];

        if (lines.Length == 0 || lines[0].Trim() != "[DOCUMENT]")
        {
            errors.Add($"{path}: must start with [DOCUMENT]");
            return (reqs, errors);
        }

        int sectionDepth = 0;
        int i = 0;
        int n = lines.Length;
        while (i < n)
        {
            string trimmed = lines[i].Trim();
            if (trimmed == "[[SECTION]]")
            {
                sectionDepth++;
                i++;
                continue;
            }

            if (trimmed == "[[/SECTION]]")
            {
                sectionDepth--;
                if (sectionDepth < 0)
                {
                    errors.Add($"{path}:{i + 1}: unmatched [[/SECTION]]");
                }

                i++;
                continue;
            }

            if (trimmed == "[REQUIREMENT]")
            {
                i++;
                Dictionary<string, string> fields = new(StringComparer.Ordinal);
                List<string> parents = [];
                bool inRelations = false;

                while (i < n)
                {
                    string cur = lines[i];
                    string curTrim = cur.Trim();
                    if (curTrim.Length == 0 || curTrim.StartsWith('['))
                    {
                        break;
                    }

                    if (curTrim == "RELATIONS:")
                    {
                        inRelations = true;
                        i++;
                        continue;
                    }

                    if (inRelations)
                    {
                        Match relMatch = RelationValueRegex().Match(cur);
                        if (relMatch.Success)
                        {
                            parents.Add(relMatch.Groups[1].Value);
                        }

                        i++;
                        continue;
                    }

                    Match fieldMatch = FieldLineRegex().Match(cur);
                    if (fieldMatch.Success)
                    {
                        string key = fieldMatch.Groups[1].Value;
                        string val = fieldMatch.Groups[2].Value.Trim();
                        if (val == ">>>")
                        {
                            List<string> buf = [];
                            i++;
                            while (i < n && lines[i].Trim() != "<<<")
                            {
                                buf.Add(lines[i]);
                                i++;
                            }

                            if (i >= n)
                            {
                                errors.Add($"{path}: unterminated >>> block for {key}");
                                break;
                            }

                            fields[key] = string.Join('\n', buf).Trim();
                        }
                        else
                        {
                            fields[key] = val;
                        }
                    }

                    i++;
                }

                foreach (string reqField in RequiredFields)
                {
                    if (!fields.TryGetValue(reqField, out string? v) || string.IsNullOrWhiteSpace(v))
                    {
                        string uidLabel = fields.GetValueOrDefault("UID", "<unknown>");
                        errors.Add($"{path}: requirement {uidLabel} missing {reqField}");
                    }
                }

                if (fields.TryGetValue("UID", out string? uid) && !string.IsNullOrWhiteSpace(uid))
                {
                    fields.TryGetValue("ARTIFACT", out string? artifact);
                    reqs.Add(new RequirementNode(
                        Uid: uid,
                        Status: fields.GetValueOrDefault("STATUS", string.Empty),
                        Title: fields.GetValueOrDefault("TITLE", string.Empty),
                        FilePath: path,
                        Artifact: artifact,
                        Parents: parents));
                }

                continue;
            }

            i++;
        }

        if (sectionDepth != 0)
        {
            errors.Add($"{path}: unclosed [[SECTION]] blocks (depth={sectionDepth})");
        }

        return (reqs, errors);
    }

    /// <summary>
    /// Validates all <c>.sdoc</c> specifications under <paramref name="requirementsDirectory"/> in parallel.
    /// </summary>
    /// <param name="requirementsDirectory">Directory containing StrictDoc <c>.sdoc</c> files.</param>
    /// <param name="repoRoot">Repository root directory against which <c>ARTIFACT</c> paths are resolved.</param>
    /// <param name="cancellationToken">Cancellation token for cooperative cancellation.</param>
    /// <returns>A <see cref="StrictDocValidationReport"/> summarizing coverage and any discovered errors.</returns>
    public static async Task<StrictDocValidationReport> ValidateDirectoryAsync(
        string requirementsDirectory,
        string repoRoot,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(requirementsDirectory);
        ArgumentException.ThrowIfNullOrWhiteSpace(repoRoot);

        string fullReqDir = Path.GetFullPath(requirementsDirectory);
        string fullRepoRoot = Path.GetFullPath(repoRoot);
        string[] sdocFiles = Directory.GetFiles(fullReqDir, "*.sdoc", SearchOption.AllDirectories)
            .OrderBy(p => p, StringComparer.Ordinal)
            .ToArray();

        if (sdocFiles.Length == 0)
        {
            return new StrictDocValidationReport(0, 0, 0, 0, 0, [$"No .sdoc files found in {fullReqDir}"]);
        }

        ConcurrentDictionary<string, RequirementNode> allReqs = new(StringComparer.Ordinal);
        ConcurrentBag<string> errors = [];

        await Parallel.ForEachAsync(
            sdocFiles,
            cancellationToken,
            async (filePath, ct) =>
            {
                (IReadOnlyList<RequirementNode> reqs, IReadOnlyList<string> fileErrors) =
                    await ParseFileAsync(filePath, ct).ConfigureAwait(false);

                foreach (string err in fileErrors)
                {
                    errors.Add(err);
                }

                foreach (RequirementNode node in reqs)
                {
                    if (!allReqs.TryAdd(node.Uid, node))
                    {
                        RequirementNode existing = allReqs[node.Uid];
                        errors.Add($"Duplicate UID '{node.Uid}' in {filePath} and {existing.FilePath}");
                    }
                }
            }).ConfigureAwait(false);

        int implemented = 0;
        int partial = 0;
        int planned = 0;

        foreach ((string uid, RequirementNode node) in allReqs)
        {
            if (!ValidStatuses.Contains(node.Status))
            {
                errors.Add($"{node.FilePath}: invalid STATUS '{node.Status}' on {uid}");
            }
            else if (node.Status == "Implemented")
            {
                implemented++;
            }
            else if (node.Status == "Partially Implemented")
            {
                partial++;
            }
            else
            {
                planned++;
            }

            if (!uid.StartsWith("STRATA-SYS-", StringComparison.Ordinal) && node.Parents.Count == 0)
            {
                errors.Add($"{node.FilePath}: orphan requirement {uid} has no Parent relation");
            }

            foreach (string parent in node.Parents)
            {
                if (!allReqs.ContainsKey(parent))
                {
                    errors.Add($"{node.FilePath}: dangling Parent '{parent}' referenced by {uid}");
                }
            }

            if (!string.IsNullOrWhiteSpace(node.Artifact))
            {
                string artifactPath = Path.Combine(fullRepoRoot, node.Artifact);
                if (!File.Exists(artifactPath) && !Directory.Exists(artifactPath))
                {
                    errors.Add($"{node.FilePath}: missing ARTIFACT path '{node.Artifact}' on {uid}");
                }
            }
        }

        foreach (string cycleError in DetectCycles(allReqs))
        {
            errors.Add(cycleError);
        }

        return new StrictDocValidationReport(
            SpecFileCount: sdocFiles.Length,
            RequirementCount: allReqs.Count,
            ImplementedCount: implemented,
            PartiallyImplementedCount: partial,
            PlannedCount: planned,
            Errors: errors.OrderBy(e => e, StringComparer.Ordinal).ToList());
    }

    private static List<string> DetectCycles(IReadOnlyDictionary<string, RequirementNode> reqMap)
    {
        List<string> errors = [];
        Dictionary<string, int> state = new(StringComparer.Ordinal);

        void Dfs(string uid)
        {
            state[uid] = 1;
            foreach (string parent in reqMap[uid].Parents)
            {
                if (!reqMap.ContainsKey(parent))
                {
                    continue;
                }

                int st = state.GetValueOrDefault(parent, 0);
                if (st == 1)
                {
                    errors.Add($"Cycle detected involving {uid} -> {parent}");
                }
                else if (st == 0)
                {
                    Dfs(parent);
                }
            }

            state[uid] = 2;
        }

        foreach (string uid in reqMap.Keys)
        {
            if (state.GetValueOrDefault(uid, 0) == 0)
            {
                Dfs(uid);
            }
        }

        return errors;
    }

    [GeneratedRegex(@"^\s*VALUE:\s*(\S+)\s*$")]
    private static partial Regex RelationValueRegex();

    [GeneratedRegex(@"^([A-Z_]+):\s*(.*)$")]
    private static partial Regex FieldLineRegex();
}
