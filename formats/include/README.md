# Shared headers

What both halves of SkyDot must agree on about the pack format, as C++20
headers with no dependencies: `skydot_formats/*.hpp`. The converter and the
engine each add this directory to their include path.

Only vocabulary of the format belongs here: names, constants and the rules for
forming them that a reader and a writer must share. Never a reader or a
writer, and nothing that needs FlatBuffers (the schemas in `../schema/` are
the one definition of the tables and their flag enums).

- `flags.hpp`: `has_flag`, to test a bit of the schemas' `bit_flags` enums.
- `vpath.hpp`: `normalize_vpath`, the spelling of a virtual path, with
  `model_vpath` and `texture_vpath`, the paths a MODL or TXST value names.
- `units.hpp`: metres per game unit, game units per exterior cell, game units
  per Havok unit.
- `asset_kind.hpp`: the kinds of asset, their word in `vpath.idx`, their
  extension and the path of their loose file.

A header here must compile warning-free in both builds: C++20, and the
converter's and the engine's warning flags.
