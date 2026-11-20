// The C# binding checked against Laplace-Native itself:
//   AbiCheck C-LAYOUT.json   every struct's size and field offsets equal C's (abi.g.c prints them), then calls through
//                            the library: identity, composition, JSON, hexadecimal.
using System.Text;
using System.Text.Json;
using Laplace.Native;

unsafe
{
    int failures = 0;
    void Check(bool ok, string what) { if (!ok) { failures++; Console.Error.WriteLine("FAIL " + what); } }

    var c = JsonSerializer.Deserialize<Dictionary<string, long>>(File.ReadAllText(args[0]))!;
    var cs = LaplaceNativeAbi.Layout();
    foreach (var (key, value) in c)
        Check(cs.TryGetValue(key, out var v) && v == value, $"{key}: C {value}, C# {(cs.TryGetValue(key, out var w) ? w : -1)}");
    Check(cs.Count == c.Count, $"{cs.Count} C# entries, {c.Count} C");

    // A text's flat ID is the composition of its codepoints' IDs: two ways through the library, one answer.
    lp_id a, b, ab, flat;
    LaplaceNative.lp_id_codepoint('a', &a);
    LaplaceNative.lp_id_codepoint('b', &b);
    lp_id* pair = stackalloc lp_id[2]; pair[0] = a; pair[1] = b;
    LaplaceNative.lp_id_compose(pair, 2, &ab);
    var text = "ab"u8;
    fixed (byte* s = text) Check(LaplaceNative.lp_id_codepoints_utf8(s, (nuint)text.Length, &flat), "lp_id_codepoints_utf8");
    Check(new ReadOnlySpan<byte>(ab.b, 16).SequenceEqual(new ReadOnlySpan<byte>(flat.b, 16)), "compose(a, b) == codepoints(\"ab\")");

    // Hexadecimal of the ID, through lp_hex.
    byte* hex = stackalloc byte[33];
    LaplaceNative.lp_hex(&ab, 16, hex);
    var h = Encoding.ASCII.GetString(hex, 32);
    Check(h.Length == 32 && h.All(Uri.IsHexDigit), "lp_hex: " + h);

    // JSON: a member read through the one reader.
    var doc = "{\"name\":\"Holmes\",\"n\":221}"u8;
    fixed (byte* d = doc)
    {
        void* j = LaplaceNative.lp_json_parse(d, (nuint)doc.Length);
        Check(j != null, "lp_json_parse");
        long n;
        fixed (byte* key = "n\0"u8) Check(LaplaceNative.lp_json_integer(j, LaplaceNative.lp_json_get(j, 0, key), &n) && n == 221, "lp_json_integer");
        LaplaceNative.lp_json_free(j);
    }

    // Envelope, decimal, tensor, and scoped consensus (#1708, #1709).
    var envFast = LaplaceNative.lp_envelope_preset((int)LaplaceNative.LP_ENVELOPE_FAST);
    Check(envFast.max_db_probes == 8 && envFast.max_hops == 2, "lp_envelope_preset");
    var wt = LaplaceNative.lp_work_weights_default();
    lp_work_counters wc = default;
    wc.db_probes = 2; wc.rows_scanned = 100;
    Check(LaplaceNative.lp_work_check(&envFast, &wc) && LaplaceNative.lp_work_actual_us(&wc, &wt) > 0, "lp_work_check");

    byte* dbuf = stackalloc byte[32];
    nuint dn = LaplaceNative.lp_decimal_u64(255, dbuf, 32);
    Check(dn == 3 && dbuf[0] == (byte)'2' && dbuf[1] == (byte)'5' && dbuf[2] == (byte)'5', "lp_decimal_u64");

    fixed (byte* dtName = "Q4_K\0"u8)
        Check(LaplaceNative.lp_dtype_from_name(dtName) == LaplaceNative.LP_DTYPE_Q4_K, "lp_dtype_from_name Q4_K");

    double* weights = stackalloc double[2]; weights[0] = 1.0; weights[1] = 3.0;
    int* bp = stackalloc int[2];
    Check(LaplaceNative.lp_fisher_shares_bp(weights, null, 2, bp) && bp[0] == 2500 && bp[1] == 7500, "lp_fisher_shares_bp");

    Console.WriteLine(failures == 0 ? $"csharp abi: {c.Count} layouts and the calls agree with C" : $"csharp abi: FAILED {failures}");
    return failures == 0 ? 0 : 1;
}
