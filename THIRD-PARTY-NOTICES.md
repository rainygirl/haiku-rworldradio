# Third-party components

rworldradio vendors two audio decoders directly into its source tree. Both are
compiled into the arm64 build only, where the target image ships an empty
`/boot/system/add-ons/media/plugins` and the Media Kit therefore cannot decode
anything at all. The x86 build uses the system's own media add-ons and does not
include either decoder.

## minimp3 — `src/thirdparty/minimp3.h`

MP3 decoder, from https://github.com/lieff/minimp3.

Public domain (CC0). Per its own header: "To the extent possible under law, the
author(s) have dedicated all copyright and related and neighboring rights to
this software to the public domain worldwide."

No restrictions worth flagging. MP3's core patents expired worldwide in 2017.

## FDK-AAC — `src/thirdparty/fdk-aac/`

AAC / HE-AAC decoder, from https://github.com/mstorsjo/fdk-aac (the standalone
extraction of Android's Fraunhofer codec). Vendored unmodified, decode path
only — the encoder libraries are not included.

Licensed under the "Software License for The Fraunhofer FDK AAC Codec Library
for Android", reproduced verbatim in `src/thirdparty/fdk-aac/NOTICE`. It is a
modified-BSD-style licence, **not** copyleft: it does not place rworldradio
itself under any particular licence. It does require that the FDK-AAC source
remain available free of charge to anyone receiving a binary containing it,
which this repository satisfies by carrying the source.

**Two things to be aware of, neither of which is hidden by this project:**

1. **No patent licence is granted.** Section 3 of that licence states plainly
   that no express or implied patent licences are granted, and that the
   software may be used "only for purposes that are authorized by appropriate
   patent licenses". Distributors and users bear that risk themselves. This is
   why Debian and Fedora classify FDK-AAC as non-free — the reason is the
   patent disclaimer, not copyleft.

2. **AAC's patent position is not MP3's.** Plain AAC-LC is generally understood
   to be patent-expired; Fedora relies on that conclusion publicly and ships an
   AAC-LC-only build as free software. The HE-AAC extensions (SBR and
   Parametric Stereo — the "AAC+" many stations use) are *not* in the same
   position and remain covered by active patent-licensing programmes into the
   2030s. rworldradio decodes both, because that is what the stations serve.

For a non-commercial open-source project this is a note, not a blocker — the
same decoder ships in Android, Chromium, VLC and ffmpeg builds. It is recorded
here so nobody has to rediscover it, and so no one mistakes AAC support for
being as unencumbered as the MP3 support next to it.
