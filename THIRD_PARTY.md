# Third-party components

Unique Collection Tab is MIT (see `LICENSE`). This file lists everything else it uses and what
that means for anyone who ships, forks or repackages it.

| component | licence | how it is used |
| --- | --- | --- |
| MinHook | BSD 2-Clause | vendored source under `third_party\minhook`, compiled into `uniquetab.asi` |
| Ultimate ASI Loader | MIT | required at run time, linked to, **not** redistributed here |
| Grim Dawn's own data | not ours | read at run time from the player's own installation; nothing derived from it ships - see below |

---

## MinHook

The detour library. Its sources are vendored under `third_party\minhook` and are compiled
directly into `uniquetab.asi`, so the binary is a derived work and carries the notice below.
Upstream: <https://github.com/TsudaKageyu/minhook>.

The text is `third_party\minhook\LICENSE.txt`, reproduced in full:

```
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

================================================================================
Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
================================================================================
Hacker Disassembler Engine 32 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Hacker Disassembler Engine 64 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

The authors listed by upstream in `third_party\minhook\AUTHORS.txt` are Tsuda Kageyu (creator and
maintainer), Michael Maltsev (the queue functions and many fixes) and Andrey Unis (the plain-C
rewrite of the hook engine).

Anyone who redistributes a build of this mod redistributes MinHook in binary form and must carry
the text above with it. Keeping this file beside the binary is enough.

## Ultimate ASI Loader

`uniquetab.asi` is an ASI plugin: it does nothing until a loader loads it. That loader is
Ultimate ASI Loader (MIT), by ThirteenAG: <https://github.com/ThirteenAG/Ultimate-ASI-Loader>.

It is **not** part of this package. Its licence would allow shipping it, but the player downloads
it themselves, once, for every ASI mod they use - and a loader that comes from upstream is a
loader they can update without waiting for this mod. `README.md` says which file to take and
where to put it.

Nothing in this repository is derived from Ultimate ASI Loader's source. The mod only relies on
its documented behaviour: a `.asi` file next to the host executable (or in `scripts\` /
`plugins\`) is `LoadLibrary`'d at start-up.

## Grim Dawn

Grim Dawn is Crate Entertainment's. No part of the game's program is here: no executable, no
library, no game archive, no icon.

**Everything the tab shows is made on the player's machine, out of the player's own copy.** On
the first launch, and again whenever the game's archives change, the mod reads the archives the
player already has and writes into its own folder:

- `catalogue.bin` and three record lists - the item names, icon sizes, classes and record paths
  the tab shows;
- `uniq-pages.arz`, the page database. Its records are new - one page window and its boxes per
  page, at coordinates the mod derives from the caravan window it finds - but each box names an
  item record of the game's, so the file holds a long list of the game's own record paths.

At the same time it paints the page backgrounds into `<game>\settings\ui\caravan\`. On the
vanilla layout they are painted on top of the game's own stash cover texture, read out of the
player's `UI.arc`: its lattice erased, the new box frames alpha-stamped from one real vanilla
frame, the border and tab strip composited back on top. Under a stash-UI mod they are one flat
colour with the collection's own frame lines, and nothing of that mod's art is copied.

**The release package ships none of it.** It is `uniquetab.asi` and four documents; the files
above exist only on the machine of a player who owns the game, written from that player's copy.

**The development repository carries one copy of that derived data**, under `data\oracle\`, as
the fixtures the generator is compared against: `catalogue.bin` (the English display names of
about 4,000 items and their record and bitmap paths), `uniq-records.txt`, `uniq-pages.txt` and
`uniq-groups.txt` (record paths), `catalogue-stats.md`, `uniq-pages.arz`, the seven vanilla
plates, a few sample records as text, and, under `plates\src\`, the three vanilla caravan
textures the plates are painted over. That is item text, record paths and artwork extracted from
the game, kept so the test suite can prove the generator's output. It is not in the package and
not in the public tree; `data\oracle\ORACLE.txt` says how to rebuild it from a copy of Grim Dawn
1.3.0.8 before running the catalogue harness.

Grim Dawn is a trademark of its owner. This is an unofficial mod. It is not endorsed by, nor
affiliated with, Crate Entertainment.
