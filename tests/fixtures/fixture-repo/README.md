# fixture-repo

A synthetic project used by the eval suite (`tools/eval`). Deterministic,
offline, and small on purpose: the tasks assert harness behaviour, not model
behaviour, so nothing here should ever change between runs.

The layout is a normal C++ project -- sources, headers, a test directory, and an
`.mcode/` config -- so the tasks exercise the same paths a real workspace does.
