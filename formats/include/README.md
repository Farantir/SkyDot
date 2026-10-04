# Shared headers

What both halves of SkyDot must agree on about the pack format, as C++20
headers with no dependencies: `skydot_formats/*.hpp`. The converter and the
engine each add this directory to their include path.

Only vocabulary of the format belongs here: names, constants and the rules for
forming them that a reader and a writer must share. Never a reader or a
writer, and nothing that needs FlatBuffers (the schemas in `../schema/` are
the one definition of the tables and their flag enums).

- `flags.hpp`: `has_flag`, to test a bit of the schemas' `bit_flags` enums.
