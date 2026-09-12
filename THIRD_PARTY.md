# Third-party components

Unique Collection Tab is MIT (see `LICENSE`). This file lists everything else it uses and what
that means for anyone who ships, forks or repackages it.

| component | licence | how it is used |
| --- | --- | --- |
| MinHook | BSD 2-Clause | vendored source under `third_party\minhook`, compiled into `uniquetab.asi` |
| Ultimate ASI Loader | MIT | required at run time, linked to, **not** redistributed here |
| Grim Dawn's own data | not ours | the catalogue is built at run time from the player's own installation; the page and plate files are derived from it - see below |

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

**The catalogue comes from the player's own copy.** Every item name, icon size, class and record
path the tab shows is read at run time out of the archives the player already has. On the first
launch, and again whenever the game's archives change, the mod writes `catalogue.bin` and three
record lists into its own folder. The release package ships none of them.

**The repository does carry one copy of that generated data**, under `data\oracle\`, as the test
fixtures the generator is compared against byte for byte: `catalogue.bin` (the English display
names of about 2,700 items, and about 6,300 record and bitmap paths), `uniq-records.txt` and
`uniq-groups.txt` (record paths) and `catalogue-stats.md`. That is item text and record paths
extracted from the game, kept so the test suite can prove the generator's output. It is not in
the package a player installs, and a maintainer who would rather not publish it can delete the
folder and regenerate the fixtures from a copy of the game before running the catalogue harness.

**Two shipped files are derived from the game's own data, and it is worth being exact about
them**, because "derived" is not "unrelated":

- `uniq-pages.arz` is written by `tools\build_uniq_db.py`. Its records are new - one page
  window and forty boxes per page, at coordinates this project chose - but each box names an
  item record of the game's, so the file contains a long list of the game's own record paths.
- the plate textures in `plates\` are painted by `tools\make_plates.py` **on top of** two vanilla
  caravan textures: the ground is the game's cover image with its lattice erased, the new box
  frames are alpha-stamped from one real vanilla frame, and the border and tab strip are the
  game's, composited back on top. The vanilla textures themselves are not in the repository;
  `data\plates\src\README.txt` says which archive entries to extract to run the script.

So the package includes work derived from Crate's data and artwork, in the ordinary way a mod
does. It is useless without the game, replaces nothing in the installation, and is offered as a
free unofficial mod. Anyone republishing it should know exactly this much and decide for
themselves; a maintainer who wants the derivation gone can generate the plates at run time from
the player's own archives instead of shipping them.

Grim Dawn is a trademark of its owner. This is an unofficial mod. It is not endorsed by, nor
affiliated with, Crate Entertainment.
