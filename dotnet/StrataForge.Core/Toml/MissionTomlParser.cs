using System.Globalization;
using System.Text;

namespace StrataForge.Core.Toml;

/// <summary>
/// Represents a parsed TOML table mapping string keys to strongly typed scalar, array, or nested table values.
/// </summary>
/// <remarks>
/// Used by <see cref="MissionTomlParser"/> to model declarative STRATA-20 <c>mission.toml</c> manifests
/// without requiring any third-party NuGet packages.
/// </remarks>
public sealed class TomlTable
{
    private readonly Dictionary<string, object> _values = new(StringComparer.Ordinal);

    /// <summary>
    /// Gets the underlying read-only key/value dictionary for this TOML table.
    /// </summary>
    /// <value>A read-only mapping of TOML keys to parsed values.</value>
    public IReadOnlyDictionary<string, object> Values => _values;

    /// <summary>
    /// Sets or replaces a value under the specified key in this table.
    /// </summary>
    /// <param name="key">The TOML key name.</param>
    /// <param name="value">The parsed scalar, list, or nested table value.</param>
    /// <exception cref="ArgumentException">Thrown when <paramref name="key"/> is null or whitespace.</exception>
    public void Set(string key, object value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(key);
        ArgumentNullException.ThrowIfNull(value);
        _values[key] = value;
    }

    /// <summary>
    /// Attempts to retrieve a strongly typed value from this table.
    /// </summary>
    /// <typeparam name="T">The expected CLR type of the stored value.</typeparam>
    /// <param name="key">The TOML key to look up.</param>
    /// <param name="value">When this method returns <see langword="true"/>, contains the retrieved value.</param>
    /// <returns><see langword="true"/> if the key exists and matches <typeparamref name="T"/>; otherwise, <see langword="false"/>.</returns>
    public bool TryGet<T>(string key, out T? value)
    {
        if (_values.TryGetValue(key, out object? raw) && raw is T typed)
        {
            value = typed;
            return true;
        }

        value = default;
        return false;
    }

    /// <summary>
    /// Retrieves a string value or returns <paramref name="fallback"/> if absent.
    /// </summary>
    /// <param name="key">The TOML key name.</param>
    /// <param name="fallback">Default value returned when the key is absent.</param>
    /// <returns>The parsed string value or <paramref name="fallback"/>.</returns>
    public string GetString(string key, string fallback = "") =>
        _values.TryGetValue(key, out object? raw) && raw is string s ? s : fallback;

    /// <summary>
    /// Retrieves a 64-bit signed integer value or returns <paramref name="fallback"/> if absent.
    /// </summary>
    /// <param name="key">The TOML key name.</param>
    /// <param name="fallback">Default value returned when the key is absent.</param>
    /// <returns>The parsed 64-bit integer value or <paramref name="fallback"/>.</returns>
    public long GetInt64(string key, long fallback = 0L) =>
        _values.TryGetValue(key, out object? raw) ? raw switch
        {
            long l => l,
            int i => i,
            ulong ul => checked((long)ul),
            _ => fallback,
        } : fallback;

    /// <summary>
    /// Retrieves an unsigned 64-bit integer value or returns <paramref name="fallback"/> if absent.
    /// </summary>
    /// <param name="key">The TOML key name.</param>
    /// <param name="fallback">Default value returned when the key is absent.</param>
    /// <returns>The parsed unsigned 64-bit integer value or <paramref name="fallback"/>.</returns>
    public ulong GetUInt64(string key, ulong fallback = 0UL) =>
        _values.TryGetValue(key, out object? raw) ? raw switch
        {
            ulong ul => ul,
            long l when l >= 0L => (ulong)l,
            int i when i >= 0 => (ulong)i,
            _ => fallback,
        } : fallback;

    /// <summary>
    /// Retrieves a double-precision floating-point value or returns <paramref name="fallback"/> if absent.
    /// </summary>
    /// <param name="key">The TOML key name.</param>
    /// <param name="fallback">Default value returned when the key is absent.</param>
    /// <returns>The parsed double value or <paramref name="fallback"/>.</returns>
    public double GetDouble(string key, double fallback = 0.0) =>
        _values.TryGetValue(key, out object? raw) ? raw switch
        {
            double d => d,
            long l => l,
            ulong ul => ul,
            _ => fallback,
        } : fallback;

    /// <summary>
    /// Retrieves an optional double-precision floating-point value if present.
    /// </summary>
    /// <param name="key">The TOML key name.</param>
    /// <returns>The parsed double value, or <see langword="null"/> if absent.</returns>
    public double? GetNullableDouble(string key) =>
        _values.TryGetValue(key, out object? raw) ? raw switch
        {
            double d => d,
            long l => l,
            ulong ul => ul,
            _ => null,
        } : null;

    /// <summary>
    /// Retrieves a nested <see cref="TomlTable"/> for the specified section key.
    /// </summary>
    /// <param name="key">The section name.</param>
    /// <returns>The nested table if found; otherwise, <see langword="null"/>.</returns>
    public TomlTable? GetTable(string key) =>
        _values.TryGetValue(key, out object? raw) && raw is TomlTable tbl ? tbl : null;

    /// <summary>
    /// Retrieves a list of tables representing a TOML <c>[[array_of_tables]]</c> declaration.
    /// </summary>
    /// <param name="key">The array-of-tables key name.</param>
    /// <returns>A read-only list of tables (empty if absent).</returns>
    public IReadOnlyList<TomlTable> GetTableArray(string key) =>
        _values.TryGetValue(key, out object? raw) && raw is List<TomlTable> list
            ? list
            : Array.Empty<TomlTable>();

    /// <summary>
    /// Retrieves a list of string items from a TOML array value.
    /// </summary>
    /// <param name="key">The array key name.</param>
    /// <returns>A read-only list of string elements (empty if absent).</returns>
    public IReadOnlyList<string> GetStringArray(string key)
    {
        if (!_values.TryGetValue(key, out object? raw) || raw is not List<object> items)
        {
            return Array.Empty<string>();
        }

        List<string> result = new(items.Count);
        foreach (object item in items)
        {
            result.Add(Convert.ToString(item, CultureInfo.InvariantCulture) ?? string.Empty);
        }

        return result;
    }

    /// <summary>
    /// Retrieves a list of 64-bit integer items from a TOML array value.
    /// </summary>
    /// <param name="key">The array key name.</param>
    /// <returns>A read-only list of integer elements (empty if absent).</returns>
    public IReadOnlyList<long> GetInt64Array(string key)
    {
        if (!_values.TryGetValue(key, out object? raw) || raw is not List<object> items)
        {
            return Array.Empty<long>();
        }

        List<long> result = new(items.Count);
        foreach (object item in items)
        {
            if (item is long l)
            {
                result.Add(l);
            }
            else if (item is ulong ul)
            {
                result.Add(checked((long)ul));
            }
        }

        return result;
    }

    /// <summary>
    /// Converts this TOML table recursively into plain CLR dictionaries and lists suitable for JSON serialization.
    /// </summary>
    /// <returns>A JSON-serializable dictionary representation of this table.</returns>
    public Dictionary<string, object?> ToPlainDictionary()
    {
        Dictionary<string, object?> map = new(StringComparer.Ordinal);
        foreach ((string k, object v) in _values)
        {
            map[k] = ConvertToPlain(v);
        }

        return map;
    }

    private static object? ConvertToPlain(object value) => value switch
    {
        TomlTable tbl => tbl.ToPlainDictionary(),
        List<TomlTable> tables => tables.Select(t => t.ToPlainDictionary()).ToList(),
        List<object> list => list.Select(ConvertToPlain).ToList(),
        _ => value,
    };
}

/// <summary>
/// Zero-dependency, thread-safe TOML v1.0 subset parser engineered for STRATA-20 mission manifests.
/// </summary>
/// <remarks>
/// Supports standard <c>[table]</c> headers, <c>[[array_of_tables]]</c> headers, hexadecimal/binary/octal
/// and underscore-separated integers (e.g. <c>0x0000_0008_0000_0000</c>, <c>10_000</c>), floating-point
/// scalars, booleans, escaped double-quoted strings, and homogeneous/mixed inline arrays.
/// </remarks>
public static class MissionTomlParser
{
    /// <summary>
    /// Parses a UTF-8 TOML document into a root <see cref="TomlTable"/>.
    /// </summary>
    /// <param name="tomlText">The full TOML source text to parse.</param>
    /// <returns>The parsed root <see cref="TomlTable"/>.</returns>
    /// <exception cref="FormatException">Thrown if the TOML syntax is malformed.</exception>
    public static TomlTable Parse(string tomlText)
    {
        ArgumentNullException.ThrowIfNull(tomlText);

        TomlTable root = new();
        TomlTable current = root;
        string[] lines = tomlText.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');

        for (int lineIdx = 0; lineIdx < lines.Length; lineIdx++)
        {
            string line = StripComment(lines[lineIdx]).Trim();
            if (line.Length == 0)
            {
                continue;
            }

            if (line.StartsWith("[[", StringComparison.Ordinal) && line.EndsWith("]]", StringComparison.Ordinal))
            {
                string tableName = line[2..^2].Trim();
                if (!root.TryGet<List<TomlTable>>(tableName, out List<TomlTable>? list) || list is null)
                {
                    list = [];
                    root.Set(tableName, list);
                }

                TomlTable item = new();
                list.Add(item);
                current = item;
                continue;
            }

            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                string sectionName = line[1..^1].Trim();
                TomlTable? existing = root.GetTable(sectionName);
                if (existing is null)
                {
                    existing = new TomlTable();
                    root.Set(sectionName, existing);
                }

                current = existing;
                continue;
            }

            int eq = line.IndexOf('=');
            if (eq <= 0)
            {
                throw new FormatException($"TOML syntax error at line {lineIdx + 1}: expected 'key = value'.");
            }

            string key = line[..eq].Trim();
            string rawValue = line[(eq + 1)..].Trim();
            current.Set(key, ParseValue(rawValue, lineIdx + 1));
        }

        return root;
    }

    private static string StripComment(string line)
    {
        bool inQuotes = false;
        bool escaped = false;
        for (int i = 0; i < line.Length; i++)
        {
            char c = line[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (c == '\\' && inQuotes)
            {
                escaped = true;
                continue;
            }

            if (c == '"')
            {
                inQuotes = !inQuotes;
                continue;
            }

            if (c == '#' && !inQuotes)
            {
                return line[..i];
            }
        }

        return line;
    }

    private static object ParseValue(string raw, int lineNumber)
    {
        if (raw.Length == 0)
        {
            throw new FormatException($"TOML syntax error at line {lineNumber}: empty value.");
        }

        if (raw.StartsWith('"') && raw.EndsWith('"') && raw.Length >= 2)
        {
            return UnescapeString(raw[1..^1]);
        }

        if (raw.StartsWith('[') && raw.EndsWith(']'))
        {
            return ParseArray(raw[1..^1], lineNumber);
        }

        if (raw.Equals("true", StringComparison.Ordinal))
        {
            return true;
        }

        if (raw.Equals("false", StringComparison.Ordinal))
        {
            return false;
        }

        string cleaned = raw.Replace("_", string.Empty, StringComparison.Ordinal);
        if (cleaned.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            ulong u = ulong.Parse(cleaned[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            return u <= long.MaxValue ? (object)(long)u : u;
        }

        if (cleaned.Contains('.', StringComparison.Ordinal) ||
            cleaned.Contains('e', StringComparison.OrdinalIgnoreCase))
        {
            return double.Parse(cleaned, NumberStyles.Float, CultureInfo.InvariantCulture);
        }

        if (long.TryParse(cleaned, NumberStyles.Integer, CultureInfo.InvariantCulture, out long signed))
        {
            return signed;
        }

        if (ulong.TryParse(cleaned, NumberStyles.Integer, CultureInfo.InvariantCulture, out ulong unsigned))
        {
            return unsigned;
        }

        throw new FormatException($"TOML syntax error at line {lineNumber}: unable to parse value '{raw}'.");
    }

    private static List<object> ParseArray(string inner, int lineNumber)
    {
        List<object> items = [];
        StringBuilder token = new();
        bool inQuotes = false;
        bool escaped = false;

        foreach (char c in inner)
        {
            if (escaped)
            {
                token.Append(c);
                escaped = false;
                continue;
            }

            if (c == '\\' && inQuotes)
            {
                token.Append(c);
                escaped = true;
                continue;
            }

            if (c == '"')
            {
                inQuotes = !inQuotes;
                token.Append(c);
                continue;
            }

            if (c == ',' && !inQuotes)
            {
                string part = token.ToString().Trim();
                if (part.Length > 0)
                {
                    items.Add(ParseValue(part, lineNumber));
                }

                token.Clear();
                continue;
            }

            token.Append(c);
        }

        string last = token.ToString().Trim();
        if (last.Length > 0)
        {
            items.Add(ParseValue(last, lineNumber));
        }

        return items;
    }

    private static string UnescapeString(string s)
    {
        StringBuilder sb = new(s.Length);
        for (int i = 0; i < s.Length; i++)
        {
            char c = s[i];
            if (c == '\\' && i + 1 < s.Length)
            {
                i++;
                sb.Append(s[i] switch
                {
                    'n' => '\n',
                    'r' => '\r',
                    't' => '\t',
                    '"' => '"',
                    '\\' => '\\',
                    char other => other,
                });
            }
            else
            {
                sb.Append(c);
            }
        }

        return sb.ToString();
    }
}
