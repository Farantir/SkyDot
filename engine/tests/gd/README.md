# GDScript unit tests

Headless tests of the Game layer (`engine/game/game/`), one file per subject.

## Writing one

```gdscript
extends "gd_test.gd"
const Fakes := preload("fakes.gd")

func test_something() -> void:
    expect_eq(Foo.bar(1), 2, "bar of 1")
```

- Every `test_*` method is a test (declaration order), on a fresh instance that
  is a Node in the tree; `await` works. Optional `before_each()` / `after_each()`.
- `expect(cond, what)`, `expect_eq(actual, expected, what)`,
  `watch(obj, signal)` (returns an Array filling with one Array of args per emission).
- Any error logged during a test (script error or engine error) fails it, unless
  the test declares it with `expect_errors("substring")`.
- Register the file in `engine/tests/CMakeLists.txt` with `skydot_gd_test`.

## Running

```sh
cd engine
godot4.7 --headless --path game --script $PWD/tests/gd/run.gd -- $PWD/tests/gd/test_activation_prompt.gd
ctest --preset linux-debug -L gd --output-on-failure
```

Exit code: number of failed tests; 1 if the file does not load or has no tests;
77 (CTest skip) if every test skipped.

## Fakes

`fakes.gd` hands out `SkydotPapyrus.new()` and `SkydotWorld.new()` without
`setup()`. A GDScript subclass of a native class cannot override native methods
(parse error), and typed calls would bypass it, so the unconfigured native object
is the fake: all signals exist (`emit_signal`), queries return neutral defaults,
the world is empty.

## Skips

`skip("reason")` marks a test skipped. `testpack_world()` returns the test pack's
world, or skips when `SKYDOT_TESTPACK` is unset (a failure if
`SKYDOT_TESTPACK_REQUIRE` is set, as in CI).
