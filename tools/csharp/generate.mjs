// The C# binding of Laplace-Native's public C API, generated from the headers and never written by hand.
//   node generate.mjs AST.json OUT-DIR
// AST.json is clang's JSON AST (-Xclang -ast-dump=json -fsyntax-only) of a unit that includes every public header.
// Writes:
//   OUT-DIR/LaplaceNative.g.cs   every exported (LP_API) function as a [LibraryImport("laplace")] declaration, every
//                                struct the API passes by value or through a pointer, every named constant of an enum
//   OUT-DIR/abi.g.c              a C program printing each struct's size and its fields' offsets, as JSON
//   OUT-DIR/Abi.g.cs             the same layout as C# reads it: what the ABI test compares
// A type the binding cannot map exactly (a C `long`, whose width differs between Windows and Linux; a union; a
// bit-field) stops the generation: the C API is fixed-width by law, and a guess here would be an ABI fault later.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, '../..');
const apiCPath = path.join(scriptDir, 'api.c');
const apiCSrc = fs.readFileSync(apiCPath, 'utf8');
const apiIncludes = [...apiCSrc.matchAll(/^\s*#include\s+"([^"]+)"/gm)].map(m => m[1]);

const argv = process.argv.slice(2);
const checkOnly = argv.includes('--check');
const posArgs = argv.filter(a => a !== '--check');
if (checkOnly && posArgs.length === 0) {
    const publicHeaders = fs.readdirSync(path.join(repoRoot, 'include/laplace'))
        .filter(f => f.endsWith('.h') && f !== 'os.h')
        .map(f => `laplace/${f}`).sort();
    for (const h of publicHeaders) {
        if (!apiIncludes.includes(h)) {
            console.error(`generate.mjs: header '${h}' missing from tools/csharp/api.c`);
            process.exit(1);
        }
    }
}
if (!checkOnly && posArgs.length < 2) {
    console.error('usage: node generate.mjs AST.json OUT-DIR [--check] | node generate.mjs --check');
    process.exit(2);
}
if (checkOnly && posArgs.length === 1) {
    console.error('usage: node generate.mjs AST.json OUT-DIR --check | node generate.mjs --check');
    process.exit(2);
}

const outDir = posArgs.length >= 2 ? posArgs[1] : null;
let ast;
if (posArgs.length >= 1) {
    ast = JSON.parse(fs.readFileSync(posArgs[0], 'utf8'));
} else {
    const cc = process.env.CC || (process.platform === 'win32' ? 'icx' : 'clang');
    const clangArgs = ['-Xclang', '-ast-dump=json', '-fsyntax-only', '-x', 'c', `-I${path.join(repoRoot, 'include')}`, apiCPath];
    try {
        const raw = execFileSync(cc, clangArgs, { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
        ast = JSON.parse(raw);
    } catch (e) {
        if (e.code === 'ENOENT') {
            if (!fs.existsSync(apiCPath) || apiIncludes.length === 0) process.exit(1);
            console.log(`generate.mjs: ${apiIncludes.length} headers verified (compiler unavailable)`);
            process.exit(0);
        }
        if (e.stderr) process.stderr.write(e.stderr);
        console.error(`generate.mjs: AST check failed (${e.message})`);
        process.exit(1);
    }
}
const fail = (why) => { console.error('generate.mjs: ' + why); process.exit(1); };
const emitOut = (file, body) => {
    if (!outDir) return;
    const p = path.join(outDir, file);
    if (checkOnly) {
        if (!fs.existsSync(p) || fs.readFileSync(p, 'utf8') !== body) {
            fail(`stale generated file: ${p}`);
        }
    } else {
        fs.writeFileSync(p, body);
    }
};

// Declarations in the public headers only: clang writes a location's file once and omits it while it is unchanged.
let file = '';
const ours = () => /[\\/]include[\\/]laplace[\\/][a-z0-9_]+\.h$/.test(file);
const functions = [], records = new Map(), typedefs = new Map(), constants = [];
for (const node of ast.inner || []) {
  if (node.loc?.file) file = node.loc.file;
  else if (node.loc?.expansionLoc?.file) file = node.loc.expansionLoc.file;
  if (!ours()) continue;
  if (node.kind === 'FunctionDecl' && !node.isImplicit && (node.inner || []).some(n => n.kind === 'DLLExportAttr' || n.kind === 'VisibilityAttr')
      && node.storageClass !== 'static') {
    functions.push({ name: node.name, type: node.type.qualType,
                     params: (node.inner || []).filter(n => n.kind === 'ParmVarDecl') });
  } else if (node.kind === 'RecordDecl' && node.completeDefinition) {
    if (node.tagUsed === 'union') fail('union ' + (node.name || '(anonymous)') + ': no exact C# layout');
    records.set(node.id, node);
  } else if (node.kind === 'TypedefDecl') {
    typedefs.set(node.name, node);
  } else if (node.kind === 'EnumDecl') {
    for (const c of node.inner || []) if (c.kind === 'EnumConstantDecl') constants.push(c);
  }
}

// A record's C# name: its typedef name (typedef struct { ... } lp_id), else its tag.
const recordName = new Map();
for (const [name, t] of typedefs) {
  const decl = t.inner?.[0]?.decl ?? t.inner?.[0]?.inner?.[0]?.decl;
  if (decl && records.has(decl.id)) recordName.set(decl.id, name);
}
for (const [id, r] of records) if (!recordName.has(id) && r.name) recordName.set(id, r.name);
const byName = new Map([...recordName].map(([id, name]) => [name, records.get(id)]));

const builtin = {
  'void': 'void', '_Bool': 'bool', 'bool': 'bool', 'char': 'byte', 'signed char': 'sbyte', 'unsigned char': 'byte',
  'short': 'short', 'unsigned short': 'ushort', 'int': 'int', 'unsigned int': 'uint',
  'long long': 'long', 'unsigned long long': 'ulong', 'float': 'float', 'double': 'double',
  '__int128': 'Int128', 'unsigned __int128': 'UInt128',
  'int8_t': 'sbyte', 'uint8_t': 'byte', 'int16_t': 'short', 'uint16_t': 'ushort', 'int32_t': 'int', 'uint32_t': 'uint',
  'int64_t': 'long', 'uint64_t': 'ulong', 'size_t': 'nuint', 'ptrdiff_t': 'nint', 'intptr_t': 'nint', 'uintptr_t': 'nuint',
};
// A C type spelling (as clang writes it, sugar kept) as C#. Functions' parameters and fields arrive as qualType strings.
function cs(type, where) {
  let t = type.replace(/\b(const|volatile|restrict|struct|enum)\b/g, ' ').replace(/\s+/g, ' ').trim();
  const fn = t.match(/^(.*)\(\*\)\((.*)\)$/);                             // a function pointer, written out
  if (fn) return fnptr(fn[1].trim(), fn[2], where);
  if (t.endsWith('*')) {
    const inner = t.slice(0, -1).trim();
    const base = cs(inner, where);
    return base === 'bool' ? 'byte*' : base + '*';                         // a bool through a pointer is its one byte
  }
  if (builtin[t]) return builtin[t];
  if (t === 'long' || t === 'unsigned long') fail(where + ': C long has no fixed width (use int32_t/int64_t)');
  if (byName.has(t)) return t;
  const td = typedefs.get(t);
  if (td) {
    const under = td.type.qualType;
    if (/\(\*\)/.test(under)) return cs(under, where);                       // a function pointer typedef
    if (/^enum\b/.test(under)) return 'int';                                 // an enum: C int on every target
    const bare = under.replace(/\b(const|volatile|struct|union)\b/g, ' ').replace(/\s+/g, ' ').trim();
    if (/^(struct|union) /.test(under) || bare === t) {
      if (byName.has(bare)) return bare;                                     // typedef struct tag name: the struct
      return 'void';                                                         // an opaque handle: used through a pointer
    }
    return cs(under, where);
  }
  if (/^[a-z_0-9]+$/.test(t)) return 'void';                                // an incomplete struct named by its tag
  fail(where + ': no C# for ' + type);
}
function fnptr(ret, params, where) {
  const list = params.trim() === 'void' || !params.trim() ? [] : split(params);
  const types = list.map((p, i) => abi(cs(p.replace(/\b[a-z_][a-z_0-9]*$/i, m => (isType(p, m) ? m : '')).trim() || p, where + ' parameter ' + i)));
  return `delegate* unmanaged[Cdecl]<${[...types, abi(cs(ret, where + ' return'))].join(', ')}>`;
}
function isType(p, word) { return builtin[word] || typedefs.has(word) || byName.has(word) || p.trim() === word; }
function split(params) {                                                    // commas outside parentheses
  const out = []; let depth = 0, cur = '';
  for (const c of params) { if (c === '(') depth++; if (c === ')') depth--; if (c === ',' && !depth) { out.push(cur); cur = ''; } else cur += c; }
  if (cur.trim()) out.push(cur); return out;
}
// In a function pointer's signature a C bool is one byte; LibraryImport marshals bool itself elsewhere.
const abi = (t) => (t === 'bool' ? 'byte' : t);

// Structs: each field's type, an array of a primitive as a fixed buffer, an array of a struct as an inline array.
const out = [], inlineArrays = new Map(), layout = [];
function fieldsOf(r, name) {
  const fields = [];
  for (const f of r.inner || []) {
    if (f.kind !== 'FieldDecl') continue;
    if (f.isBitfield) fail(name + '.' + f.name + ': a bit-field has no exact C# layout');
    if (!f.name) fail(name + ': an anonymous member has no C# name');
    const arr = f.type.qualType.match(/^(.*?)\s*\[(\d+)\]$/);
    if (arr) {
      const el = cs(arr[1], name + '.' + f.name), n = +arr[2];
      if (['byte','sbyte','short','ushort','int','uint','long','ulong','float','double'].includes(el))
        fields.push(`public fixed ${el} ${f.name}[${n}];`);
      else { const ia = `${el}x${n}`; inlineArrays.set(ia, [el, n]); fields.push(`public ${ia} ${f.name};`); }
    } else {
      const t = cs(f.type.qualType, name + '.' + f.name);
      // C's bool is one byte on every target Laplace builds for: as a byte the struct stays blittable.
      fields.push(t === 'bool' ? `public byte ${f.name};   // C bool` : `public ${t} ${f.name};`);
    }
    layout.push([name, f.name]);
  }
  return fields;
}
const structs = [];
for (const [id, r] of records) {
  const name = recordName.get(id); if (!name) continue;
  layout.push([name, null]);
  structs.push(`[StructLayout(LayoutKind.Sequential)]\npublic unsafe partial struct ${name}\n{\n    ${fieldsOf(r, name).join('\n    ')}\n}`);
}
for (const [ia, [el, n]] of inlineArrays)
  structs.push(`[InlineArray(${n})]\npublic struct ${ia}\n{\n    private ${el} _element0;\n}`);

// Functions.
const decls = [];
for (const f of functions) {
  const ret = f.type.slice(0, f.type.indexOf('(')).trim();
  const rt = cs(ret, f.name + ' return');
  const ps = f.params.map((p, i) => {
    const t = cs(p.type.qualType, `${f.name} parameter ${p.name || i}`);
    const n = /^(base|object|string|params|ref|out|in|lock|event|fixed|checked|operator|out|this|default)$/.test(p.name) ? '@' + p.name : (p.name || 'arg' + i);
    return t === 'bool' ? `[MarshalAs(UnmanagedType.U1)] bool ${n}` : `${t} ${n}`;
  });
  const retAttr = rt === 'bool' ? '    [return: MarshalAs(UnmanagedType.U1)]\n' : '';
  decls.push(`    [LibraryImport(Library, EntryPoint = "${f.name}")]\n    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]\n${retAttr}    public static partial ${rt} ${f.name}(${ps.join(', ')});`);
}
const consts = constants.filter(c => /^LP_[A-Z0-9_]+$/.test(c.name))
  .map(c => `    public const long ${c.name} = ${valueOf(c)};`).filter(s => !s.includes('undefined'));
function valueOf(c) {                                                       // an enumerator's value as clang folded it
  const v = (function find(n) { if (n.kind === 'ConstantExpr' && n.value !== undefined) return n.value; for (const k of n.inner || []) { const x = find(k); if (x !== undefined) return x; } })(c);
  return v;
}

if (!checkOnly && outDir) fs.mkdirSync(outDir, { recursive: true });
const header = '// <auto-generated> by Laplace-Native tools/csharp/generate.mjs from include/laplace/*.h. Do not edit. </auto-generated>\n';
emitOut('LaplaceNative.g.cs', header + `#nullable enable
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Laplace.Native;

${structs.join('\n\n')}

/// <summary>Laplace-Native's exported C API, one declaration per LP_API function.</summary>
public static unsafe partial class LaplaceNative
{
    public const string Library = "laplace";

${consts.join('\n')}

${decls.join('\n\n')}
}
`);

// The ABI check: C's sizes and offsets, and C#'s, both as one JSON object keyed "Struct" and "Struct.field".
const cRows = layout.map(([s, f]) => f ? `    printf("%s\\"${s}.${f}\\":%zu", first ? "" : ",", (size_t)offsetof(${s}, ${f})); first = 0;`
                                       : `    printf("%s\\"${s}\\":%zu", first ? "" : ",", sizeof(${s})); first = 0;`);
emitOut('abi.g.c', `/* generated by tools/csharp/generate.mjs: the C layout of every struct the C# binding declares */
${apiIncludes.map(h => `#include "${h}"`).join('\n')}
#include <stddef.h>
#include <stdio.h>
int main(void){
    int first = 1; printf("{");
${cRows.join('\n')}
    printf("}\\n"); return 0;
}
`);
const csRows = layout.map(([s, f]) => f ? `        d["${s}.${f}"] = (long)Marshal.OffsetOf<${s}>("${f}");` : `        d["${s}"] = Unsafe.SizeOf<${s}>();`);
emitOut('Abi.g.cs', header + `using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Laplace.Native;

/// <summary>Every declared struct's size and field offsets as C# lays them out, for the ABI test.</summary>
public static class LaplaceNativeAbi
{
    public static Dictionary<string, long> Layout()
    {
        var d = new Dictionary<string, long>();
${csRows.join('\n')}
        return d;
    }
}
`);
console.log(`generated ${decls.length} functions, ${structs.length} structs, ${consts.length} constants`);
