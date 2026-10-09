# Third-party notices

mcode is Apache-2.0. It embeds the following third-party components.

## Luau

The extension VM. Distributed under the MIT License.

```
MIT License

Copyright (c) 2019-2025 Roblox Corporation
Copyright (c) 1994–2019 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Luau is derived from Lua 5.1, which carries the following notice:

```
Copyright © 1994–2019 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

- Source: <https://github.com/luau-lang/luau>
- Project: <https://luau.org/>
- Pinned commit: `c0e346edd89066b44dca174c9f54ce84c746a540`
- Package: `luau/0.0.0-mcode.c0e346ed`, built by `conan/recipes/luau/`

**Attribution.** Luau's licence is MIT with a request that user-facing product
documentation credit the language and link to the project. This file and the
Licence section of `README.md` satisfy that request.

The complete licence texts also ship inside the Conan package, under
`licenses/LICENSE.txt` and `licenses/lua_LICENSE.txt`.

## Everything else

mcode links the following statically. Every licence below is the one the Conan
recipe itself declares, read from the recipe in the local cache rather than
assumed from the project's reputation. The full texts ship inside each Conan
package under `licenses/`.

| Component | Version | Licence | Role |
|---|---|---|---|
| Boost | 1.91.0 | `BSL-1.0` | Beast (HTTP/SSE), Process v2 (subprocess) |
| OpenSSL | 3.5.7 | `Apache-2.0` | TLS for the model transport |
| fmt | 12.1.0 | `MIT` | Formatting |
| spdlog | 1.17.0 | `MIT` | Logging |
| yyjson | 0.12.0 | `MIT` | JSON |
| mimalloc | 3.5.1 | `MIT` | Allocator |
| simdutf | 9.0.0 | `Apache-2.0` **or** `MIT` | UTF-8/UTF-16 |
| ankerl::unordered_dense | 5.0.1 | `MIT` | Tool registry |
| Catch2 | 3.16.0 | `BSL-1.0` | Tests only, not linked into the binary |

Three of these are not MIT, and each is worth naming rather than folding into a
generic sentence:

- **Boost and Catch2 are `BSL-1.0`**, which requires the licence text to
  accompany a distribution of the compiled form. Boost is linked into every
  build; Catch2 is a `test_requires` and is not in the shipped binary.
- **OpenSSL is `Apache-2.0`**, the same licence as mcode itself, so `LICENSE`
  covers it.
- **simdutf is dual-licensed** `Apache-2.0` or `MIT`; either may be chosen.

The choice of dependency, and why each earned its size, is recorded in
[`docs/14-cpp23-stack.md`](docs/14-cpp23-stack.md). A dependency that cannot
justify itself against measured size and compile cost does not get added.

### What a release archive contains

The archives published by the release workflow contain the binary, `README.md`,
`LICENSE`, and this file. The per-dependency licence texts are **not** reproduced
inside the archive; they are in the Conan packages they came from.

If you redistribute mcode, take the licence texts for Boost, OpenSSL, simdutf,
fmt, spdlog, yyjson, mimalloc and ankerl::unordered_dense from those packages.
Apache-2.0 §4 requires the same of anyone redistributing mcode itself, which is
why `LICENSE` is staged into the archive rather than left at the repository root.
