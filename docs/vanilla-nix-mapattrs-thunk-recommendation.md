# Recommendation for vanilla Nix: compact shared-descriptor thunks for `mapAttrs`/`zipAttrsWith`

Status: RECOMMENDATION for upstream cppnix (`~/nix`), not an asnix change.
Nothing in this document is implemented in this repository; it is a
writeup of a finding from this project's comparative work against `iets`
(a sibling Rust evaluator) and asnix's own `LazyApp` optimization (M113),
identifying one concrete opportunity that is NOT specific to asnix's
architecture and could benefit real, vanilla Nix directly.

## Why most of this project's other findings do NOT transfer to vanilla Nix

asnix is a from-scratch, handwritten x86-64 assembly evaluator with an
ahead-of-time compile-to-bytecode step (AST -> IR -> a custom opcode
dispatch VM). Vanilla Nix (cppnix) is a tree-walking interpreter over
its own AST, with no compile step, no IR, and no opcode dispatcher. Most
of this project's wins this session -- removing the lexer's token array,
shrinking `IrOp`, lookup-table opcode dispatch -- are fixes to costs that
only exist because of asnix's own VM design. cppnix has no equivalent
structure to fix. (One specific example: asnix's M180 finding, that ~62%
of its compiled IR is wasted on thunk bodies that are never forced, is
a cost specific to asnix's eager AOT-compile step -- a tree-walker like
cppnix never pays it in the first place, since it has no compile step to
skip.)

## The one finding that does transfer

cppnix's `prim_mapAttrs` (`src/libexpr/primops.cc:3643-3657`) allocates,
per attribute entry:

```cpp
for (auto & i : *args[1]->attrs()) {
    Value * vName = Value::toPtr(state.symbols[i.name]);
    Value * vFun2 = state.allocValue();
    vFun2->mkApp(args[0], vName);
    attrs.alloc(i.name).mkApp(vFun2, i.value);
}
```

**Two full heap `Value` allocations per entry** -- one for the
partially-applied `f name` (`vFun2`), one for the final application
thunk. Every entry shares the identical callback `f` and the identical
`mkApp` shape; only `name`/`value` differ per entry.

`prim_zipAttrsWith` (`primops.cc:3718-3727`) has an even heavier version
of the same pattern -- **three** heap `Value` allocations per key
(`call1`, `call2`, and the list-wrapping `arg`), for the exact same
reason (one shared callback `f`, applied once per key).

This is exactly the cost shape that:
- `iets` measured and fixed with `THUNK_MAP`: on a real workload, 22
  million `mapAttrs` thunks, 93% never forced. Its fix replaces the
  per-entry heap thunk with an 8-byte shared-descriptor cell addressed
  by index, since every entry's thunk is structurally identical except
  for which entry it is.
- asnix itself already fixed internally via **M113's `LazyApp` kind**
  (`docs/architecture.md`), a lightweight `Value::mkApp`-style
  representation for `map`/`mapAttrs`/`zipAttrsWith` specifically
  because of this same shared-callback-body property.

cppnix has adopted neither fix. Its `mapAttrs`/`zipAttrsWith` primops
still pay full per-entry `Value` allocation cost regardless of whether
the resulting attribute is ever forced -- and in most real nixpkgs-style
usage (e.g. `mapAttrs` over a large generated attrset where only a few
keys are ever selected), most of that allocation is wasted.

## Recommended change

Replace `prim_mapAttrs`'s per-entry `allocValue()` + `mkApp` pair with a
compact, index-addressed descriptor: a single shared object per
`mapAttrs` call holding `f` and a pointer to the source attrset, plus one
small per-entry cell (just enough to identify "entry N of this call")
instead of two full `Value` objects. The descriptor resolves to a real
`f name value` application only on first force of that specific
attribute -- the same sharing property `THUNK_MAP` and `LazyApp` already
exploit, applied at the C++ `Value`/thunk level instead of asnix's own
VM-internal representation.

`zipAttrsWith`'s final loop (`call1`/`call2`/`arg`) is a smaller version
of the same opportunity and should be revisited once `mapAttrs` is done,
since it shares the identical "one callback `f`, N structurally-identical
applications" shape, just with one extra allocation (the list-wrapping
`arg`) per entry.

## What this needs before it could land upstream

- A real measurement on a representative nixpkgs-scale workload (e.g.
  something resembling `all-packages.nix`'s use of `mapAttrs` /
  overlay-style patterns) showing the actual allocation/instruction-count
  cost today, the same "measure before assuming" discipline this project
  applies to its own changes -- `NIX_SHOW_STATS` already exposes thunk
  allocation counts for exactly this purpose.
- Care around cppnix's own GC/value-lifetime model: the shared descriptor
  must stay alive exactly as long as the attrset it backs, and any
  `Value` representing a deferred entry must remain safely distinguishable
  from a normal forced-or-unforced thunk by cppnix's existing garbage
  collector and `forceValue` logic -- this is a real interaction with
  code this project does not own and has not audited, unlike `LazyApp`
  (an asnix-internal change the asnix team fully controls) and `THUNK_MAP`
  (already shipped and tested inside `iets`).
- This is a recommendation for the cppnix project to evaluate and
  implement, not something this project should attempt to patch into
  `~/nix` directly without that project's own review process.
