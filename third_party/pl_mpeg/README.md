# pl_mpeg

Vendored, not fetched. `pl_mpeg.h` was taken verbatim from
https://github.com/phoboslab/pl_mpeg on 2026-09-16 (the repository publishes no
version number; it is the `master` header at that date).

It is a single header under the MIT licence — unlike miniaudio beside it, which
is public domain, so the copyright notice at the top of the header has to stay
there. It is committed rather than fetched for the same reason miniaudio is: it
has to be present on both build paths, MSVC + vcpkg, which has no port for it,
and the Linux container, which resolves everything else from distribution
packages. A fetch step would have needed an equivalent on Windows and there is
no natural place to put one.

It decodes MPEG-1 video and MP2 audio out of an MPEG-PS container. Only the
video half is used. The game's own mixer plays a transmission's voice, so
`plm_set_audio_enabled` is off and the clips the authoring tool writes carry no
audio track at all.

MPEG-1 rather than anything more modern because the patents expired years ago,
the decoder is one file with no dependencies, and the pictures are being drawn
into a panel about ninety pixels wide. Quality is not the constraint here;
having a decoder at all is.

Only `src/render/comm_video.cpp` includes it, and only that file defines
`PL_MPEG_IMPLEMENTATION`. `comm_video.h` exposes RGBA pixels and nothing else —
the same containment `device.cpp` gives miniaudio and `render/gl.h` gives glad.

To update: replace the header, bump the date above, and re-run
`descent --comm-test`, which decodes, seeks, wraps and measures the decoder's
allocations.
