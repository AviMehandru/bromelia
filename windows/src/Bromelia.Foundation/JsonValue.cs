using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace Bromelia.Foundation;

/// <summary>
/// A JSON value for params and records. Objects keep their keys in the order given, so a value written with
/// <see cref="EncodeCanonical"/> comes out the same on every platform.
/// </summary>
public abstract record JsonValue
{
    public sealed record Null : JsonValue
    {
        public static readonly Null Instance = new();
    }

    public sealed record Bool(bool Value) : JsonValue;

    /// <summary>A number written without a fraction or exponent that fits in 64 bits.</summary>
    public sealed record Integer(long Value) : JsonValue;

    /// <summary>Any other number.</summary>
    public sealed record Number(double Value) : JsonValue;

    public sealed record String(string Value) : JsonValue;

    public sealed record Array(IReadOnlyList<JsonValue> Items) : JsonValue
    {
        public bool Equals(Array? other) => other is not null && Items.SequenceEqual(other.Items);
        public override int GetHashCode() => Items.Count;
    }

    public sealed record Object(IReadOnlyList<KeyValuePair<string, JsonValue>> Members) : JsonValue
    {
        public bool Equals(Object? other) =>
            other is not null && Members.Count == other.Members.Count
            && Members.Zip(other.Members).All(p => p.First.Key == p.Second.Key && p.First.Value.Equals(p.Second.Value));
        public override int GetHashCode() => Members.Count;
    }

    // ---- access ---------------------------------------------------------------------------------

    /// <summary>The member called <paramref name="key"/> of an object; null for anything else.</summary>
    public JsonValue? this[string key] =>
        this is Object o ? o.Members.LastOrDefault(m => m.Key == key).Value : null;

    public string? AsString => (this as String)?.Value;
    public bool? AsBool => (this as Bool)?.Value;
    public long? AsInteger => this switch { Integer i => i.Value, Number n when n.Value == Math.Floor(n.Value) && Math.Abs(n.Value) < 9.2e18 => (long)n.Value, _ => null };
    public double? AsNumber => this switch { Integer i => i.Value, Number n => n.Value, _ => null };
    public IReadOnlyList<JsonValue>? AsArray => (this as Array)?.Items;
    public IReadOnlyList<KeyValuePair<string, JsonValue>>? AsObject => (this as Object)?.Members;
    public bool IsNull => this is Null;

    public static JsonValue Of(string? s) => s is null ? Null.Instance : new String(s);
    public static JsonValue Of(long n) => new Integer(n);
    public static JsonValue Of(bool b) => new Bool(b);
    public static JsonValue Of(IEnumerable<JsonValue> items) => new Array(items.ToList());
    public static JsonValue Of(params (string Key, JsonValue Value)[] members) =>
        new Object(members.Select(m => new KeyValuePair<string, JsonValue>(m.Key, m.Value)).ToList());

    // ---- parsing ---------------------------------------------------------------------------------

    /// <summary>Strict JSON (RFC 8259). Null when the text isn't JSON. A repeated key keeps its first
    /// position and its last value. An integer outside the 64-bit range isn't accepted: as a double it would lose
    /// digits.</summary>
    public static JsonValue? Parse(string text)
    {
        var p = new Parser(text);
        try
        {
            p.SkipSpace();
            var v = p.Value(0);
            p.SkipSpace();
            return p.AtEnd ? v : null;
        }
        catch (FormatException)
        {
            return null;
        }
    }

    public static JsonValue? Parse(byte[] utf8)
    {
        try
        {
            var text = new UTF8Encoding(false, true).GetString(utf8);
            if (text.Length > 0 && text[0] == '﻿') text = text.Substring(1);
            return Parse(text);
        }
        catch (ArgumentException)
        {
            return null;
        }
    }

    private sealed class Parser
    {
        private readonly string _s;
        private int _i;
        public Parser(string s) { _s = s; }
        public bool AtEnd => _i == _s.Length;

        public void SkipSpace()
        {
            while (_i < _s.Length && (_s[_i] == ' ' || _s[_i] == '\t' || _s[_i] == '\n' || _s[_i] == '\r')) _i++;
        }

        private char Peek() => _i < _s.Length ? _s[_i] : throw new FormatException();

        private void Expect(string word)
        {
            if (string.CompareOrdinal(_s, _i, word, 0, word.Length) != 0) throw new FormatException();
            _i += word.Length;
        }

        public JsonValue Value(int depth)
        {
            if (depth > 512) throw new FormatException();
            switch (Peek())
            {
                case '{': return ParseObject(depth);
                case '[': return ParseArray(depth);
                case '"': return new String(ParseString());
                case 't': Expect("true"); return new Bool(true);
                case 'f': Expect("false"); return new Bool(false);
                case 'n': Expect("null"); return Null.Instance;
                default: return ParseNumber();
            }
        }

        private JsonValue ParseObject(int depth)
        {
            _i++;
            var members = new List<KeyValuePair<string, JsonValue>>();
            SkipSpace();
            if (Peek() == '}') { _i++; return new Object(members); }
            while (true)
            {
                SkipSpace();
                if (Peek() != '"') throw new FormatException();
                var key = ParseString();
                SkipSpace();
                Expect(":");
                SkipSpace();
                var value = Value(depth + 1);
                var at = members.FindIndex(m => m.Key == key);
                if (at >= 0) members[at] = new(key, value);
                else members.Add(new(key, value));
                SkipSpace();
                var c = Peek();
                _i++;
                if (c == '}') return new Object(members);
                if (c != ',') throw new FormatException();
            }
        }

        private JsonValue ParseArray(int depth)
        {
            _i++;
            var items = new List<JsonValue>();
            SkipSpace();
            if (Peek() == ']') { _i++; return new Array(items); }
            while (true)
            {
                SkipSpace();
                items.Add(Value(depth + 1));
                SkipSpace();
                var c = Peek();
                _i++;
                if (c == ']') return new Array(items);
                if (c != ',') throw new FormatException();
            }
        }

        private string ParseString()
        {
            _i++;
            var sb = new StringBuilder();
            while (true)
            {
                var c = Peek();
                _i++;
                if (c == '"') return sb.ToString();
                if (c < 0x20) throw new FormatException();
                if (c != '\\') { sb.Append(c); continue; }
                var e = Peek();
                _i++;
                switch (e)
                {
                    case '"': sb.Append('"'); break;
                    case '\\': sb.Append('\\'); break;
                    case '/': sb.Append('/'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'u':
                        if (_i + 4 > _s.Length) throw new FormatException();
                        if (!int.TryParse(_s.AsSpan(_i, 4), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var code))
                            throw new FormatException();
                        _i += 4;
                        sb.Append((char)code);
                        break;
                    default: throw new FormatException();
                }
            }
        }

        private JsonValue ParseNumber()
        {
            int start = _i;
            bool integral = true;
            if (Peek() == '-') _i++;
            if (Peek() == '0') _i++;
            else if (Peek() >= '1' && Peek() <= '9') while (_i < _s.Length && char.IsAsciiDigit(_s[_i])) _i++;
            else throw new FormatException();
            if (_i < _s.Length && _s[_i] == '.')
            {
                integral = false;
                _i++;
                if (!char.IsAsciiDigit(Peek())) throw new FormatException();
                while (_i < _s.Length && char.IsAsciiDigit(_s[_i])) _i++;
            }
            if (_i < _s.Length && (_s[_i] == 'e' || _s[_i] == 'E'))
            {
                integral = false;
                _i++;
                if (Peek() == '+' || Peek() == '-') _i++;
                if (!char.IsAsciiDigit(Peek())) throw new FormatException();
                while (_i < _s.Length && char.IsAsciiDigit(_s[_i])) _i++;
            }
            var text = _s.Substring(start, _i - start);
            // An integer that doesn't fit in 64 bits would lose digits as a double: not accepted.
            if (integral) return long.TryParse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var n) ? new Integer(n) : throw new FormatException();
            return new Number(double.Parse(text, NumberStyles.Float, CultureInfo.InvariantCulture));
        }
    }

    // ---- writing ---------------------------------------------------------------------------------

    /// <summary>UTF-8 JSON with two-space indentation, keys in the order given and a final newline: the same
    /// bytes as Python's <c>json.dumps(value, indent=2, ensure_ascii=False) + "\n"</c>.</summary>
    public static byte[] EncodeCanonical(JsonValue value)
    {
        var sb = new StringBuilder();
        Write(sb, value, 0);
        sb.Append('\n');
        return Encoding.UTF8.GetBytes(sb.ToString());
    }

    /// <summary><see cref="EncodeCanonical"/> as text.</summary>
    public sealed override string ToString() => Encoding.UTF8.GetString(EncodeCanonical(this));

    private static void Write(StringBuilder sb, JsonValue v, int indent)
    {
        switch (v)
        {
            case Null: sb.Append("null"); break;
            case Bool b: sb.Append(b.Value ? "true" : "false"); break;
            case Integer i: sb.Append(i.Value.ToString(CultureInfo.InvariantCulture)); break;
            case Number n: sb.Append(FormatNumber(n.Value)); break;
            case String s: WriteString(sb, s.Value); break;
            case Array a:
                if (a.Items.Count == 0) { sb.Append("[]"); break; }
                sb.Append('[');
                for (int k = 0; k < a.Items.Count; k++)
                {
                    sb.Append(k == 0 ? "\n" : ",\n").Append(' ', indent + 2);
                    Write(sb, a.Items[k], indent + 2);
                }
                sb.Append('\n').Append(' ', indent).Append(']');
                break;
            case Object o:
                if (o.Members.Count == 0) { sb.Append("{}"); break; }
                sb.Append('{');
                for (int k = 0; k < o.Members.Count; k++)
                {
                    sb.Append(k == 0 ? "\n" : ",\n").Append(' ', indent + 2);
                    WriteString(sb, o.Members[k].Key);
                    sb.Append(": ");
                    Write(sb, o.Members[k].Value, indent + 2);
                }
                sb.Append('\n').Append(' ', indent).Append('}');
                break;
        }
    }

    private static void WriteString(StringBuilder sb, string s)
    {
        sb.Append('"');
        foreach (var c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                case '\b': sb.Append("\\b"); break;
                case '\f': sb.Append("\\f"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
    }

    /// <summary>Python's float repr: the shortest digits that read back as the same double, in fixed notation
    /// for exponents from -4 to 15 and scientific notation otherwise.</summary>
    private static string FormatNumber(double d)
    {
        if (double.IsNaN(d)) return "NaN";
        if (double.IsInfinity(d)) return d > 0 ? "Infinity" : "-Infinity";
        if (d == 0) return 1 / d < 0 ? "-0.0" : "0.0";
        // "E16" is 17 significant digits; trim to the shortest that round-trips.
        string digits = "";
        int exponent = 0;
        for (int precision = 1; precision <= 17; precision++)
        {
            var e = Math.Abs(d).ToString("E" + (precision - 1), CultureInfo.InvariantCulture);
            if (double.Parse(e, CultureInfo.InvariantCulture) != Math.Abs(d)) continue;
            var mantissa = e.Substring(0, e.IndexOf('E'));
            digits = mantissa.Replace(".", "").TrimEnd('0');
            if (digits.Length == 0) digits = "0";
            exponent = int.Parse(e.Substring(e.IndexOf('E') + 1), CultureInfo.InvariantCulture);
            break;
        }
        return (d < 0 ? "-" : "") + LayOutNumber(digits, exponent);
    }

    internal static string LayOutNumber(string digits, int exponent)
    {
        if (exponent < -4 || exponent >= 16)
        {
            var m = digits.Length == 1 ? digits : digits[0] + "." + digits.Substring(1);
            return m + "e" + (exponent < 0 ? "-" : "+") + Math.Abs(exponent).ToString("00", CultureInfo.InvariantCulture);
        }
        if (exponent < 0) return "0." + new string('0', -exponent - 1) + digits;
        if (digits.Length <= exponent + 1) return digits + new string('0', exponent + 1 - digits.Length) + ".0";
        return digits.Substring(0, exponent + 1) + "." + digits.Substring(exponent + 1);
    }
}
