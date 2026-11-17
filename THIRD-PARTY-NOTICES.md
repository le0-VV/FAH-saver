# Third-party notices

FAH Saver's original code is licensed under **GPL-3.0-or-later**. See
`COPYRIGHT` and the complete licence in `LICENSE`. The components below retain
their own copyright notices and licences. A licence for one component does not
change the licence of another component.

The production macOS screensaver uses Metal. The production Windows screensaver
uses Vulkan. Both use native networking. The WebView renderers and benchmark
tools in the source tree are development tools and are not included in these
screensaver binaries.

## Folding@home source references

FAH Saver uses the Folding@home client and viewer as protocol and visual
references. These upstream projects are licensed under GPL-3.0-or-later:

- [Folding@home client 8.5.6](https://github.com/FoldingAtHome/fah-client-bastet/tree/93b96817a82ade9769fba3571ac697788f13357a),
  commit `93b96817a82ade9769fba3571ac697788f13357a`.
  The `src/fah/client/Remote.cpp` notice states:
  **Copyright (c) 2001-2024, foldingathome.org. All rights reserved.**
- [Folding@home web client 8.5.7](https://github.com/FoldingAtHome/fah-web-client-bastet/tree/c54888af2a261d79270a390f289a4972965cedb4),
  commit `c54888af2a261d79270a390f289a4972965cedb4`.
  The `src/Visualization.vue` notice states:
  **Copyright (c) 2001-2026, foldingathome.org. All rights reserved.**

FAH Saver is an independent project. It does not bundle the Folding@home client,
simulation cores, Cbang, Three.js or Vue. Its native sky background is an
approximation; it does not include the web viewer's atmospheric sky shader.
The Folding@home names remain the property of their respective owners.

## nlohmann JSON 3.12.0 — both platforms

Copyright (c) 2013-2025 Niels Lohmann. Licensed under MIT.
The unmodified single header and licence are in `third_party/nlohmann/` in the
source tree. The installed licence is `nlohmann-json-LICENSE.MIT` (inside
`Contents/Resources` on macOS).

Source: [nlohmann/json v3.12.0](https://github.com/nlohmann/json/tree/v3.12.0).

## Windows compiler and runtime components

The audited Windows build uses Zig 0.17.1 and its bundled LLVM and MinGW-w64
components. These components are not covered by FAH Saver's copyright claim.
The Windows package includes the following unmodified toolchain licence texts
under `licences/`. Copies are maintained in `third_party/licenses/` in the
source tree.

| Component | Terms | Licence file |
| --- | --- | --- |
| LLVM libc++ | Apache-2.0 WITH LLVM-exception; retained legacy notices in the upstream text | `libcxx-LICENSE.TXT` |
| LLVM libc++abi | Apache-2.0 WITH LLVM-exception; retained legacy notices in the upstream text | `libcxxabi-LICENSE.TXT` |
| LLVM libunwind | Apache-2.0 WITH LLVM-exception; retained legacy notices in the upstream text | `libunwind-LICENSE.TXT` |
| Zig compiler runtime | MIT | `zig-compiler-runtime-LICENSE.MIT` |
| MinGW-w64 runtime | ZPL-2.1 except separately marked components | `mingw-runtime-COPYING.txt` |
| MinGW-w64 public-domain components | Public domain; no warranty | `mingw-DISCLAIMER.PD` |
| MinGW-w64 gdtoa by David M. Gay | Lucent permissive terms, with source-specific copyright years | `mingw-gdtoa-LICENSE.txt` |

The MinGW-w64 umbrella notice is Copyright (c) 2009-2013 by the mingw-w64
project. The linked gdtoa sources carry Lucent Technologies notices for 1998,
1998-1999, 2000, 1998-2001 and 1998/2000. Their complete notices, permissions and
disclaimers are retained in `mingw-gdtoa-LICENSE.txt` with the applicable source
filenames. The audited linked runtime contains public-domain, ZPL and gdtoa
parts. It does not contain an identified LGPL-licensed CRT implementation or
PSEH object. This statement applies to the audited build, not every component
available in the toolchain.

Sources: [Zig](https://ziglang.org/), [LLVM licensing](https://llvm.org/LICENSE.txt)
and [MinGW-w64](https://www.mingw-w64.org/).

## Windows SDK headers

The used MinGW-w64 `winhttp.h` header carries **Copyright (C) 2007 Francois
Gouget**, under LGPL-2.1-or-later. The audited header supplies declarations,
constants and structure layouts, with no static or inline implementation.
LGPL version 2.1 section 5 describes the relevant terms for object files that
use material from library header files. The original notice and the complete
LGPL 2.1 text are included as `licences/winhttp-header-NOTICE.txt` and
`licences/LGPL-2.1.txt`. Windows supplies the WinHTTP implementation.

## Vulkan headers — Windows build

The build uses Vulkan-Headers 1.4.363. The used Khronos headers carry
**Copyright 2015-2026 The Khronos Group Inc.** and
`SPDX-License-Identifier: Apache-2.0 OR MIT`. FAH Saver uses the MIT option.
The upstream repository notice and MIT text are retained as
`licences/Vulkan-Headers-LICENSE.md` and `licences/Vulkan-Headers-MIT.txt`.
The loader and GPU driver are supplied by the user's Vulkan installation.

Source: [Vulkan-Headers v1.4.363](https://github.com/KhronosGroup/Vulkan-Headers/tree/v1.4.363).

## NSIS and Modern UI — Windows installer

The installer uses NSIS 3.13 with **solid zlib compression**. NSIS core,
plug-ins and the zlib compression module use the zlib/libpng licence.
NSIS carries **Copyright (C) 1999-2026 Contributors**. The complete upstream
notice is retained as `licences/NSIS-COPYING.txt`.

Modern UI carries **Copyright 2002-2026 Joost Verburg**. Its zlib-style licence
requires its notice to remain in every distribution. The complete notice is
retained as `licences/NSIS-Modern-UI-LICENSE.txt`.

The full upstream NSIS `COPYING` also describes its bzip2 module and its
CPL-1.0 LZMA module with a linking exception. Those sections are retained
verbatim for completeness. This installer selects zlib and does not use the
bzip2 or LZMA compression modules.

Source: [NSIS licensing](https://nsis.sourceforge.io/License).

## Platform libraries and optional development tools

The macOS frameworks and system libraries, Windows system DLLs, and Vulkan
loader/driver are dynamically supplied by the operating system or the GPU
installation. Their implementations are not copied into the release.

The optional Windows WebView2 benchmark uses a separately obtained Microsoft
WebView2 SDK and loader. Its package retains the SDK's own licence. WebView2 is
not part of the production screensaver or installer. The WebView2 Runtime has
separate Microsoft terms. The optional macOS WebView benchmarks use Apple's
system WebKit framework.
