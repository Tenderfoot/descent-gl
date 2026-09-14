# miniaudio

Vendored, not fetched. `miniaudio.h` is `v0.11.25` (2026-03-04), taken verbatim
from https://github.com/mackron/miniaudio.

It is a single header released into the public domain (or MIT-0, at the user's
choice; the full text is at the end of the header). It is committed rather than
fetched because it has to be present on both build paths — MSVC + vcpkg, which
has no miniaudio port, and the Linux container, which resolves everything else
from distribution packages. A fetch step would have needed an equivalent on
Windows, and there is no natural place to put one.

Only `src/audio/device.cpp` includes it, and only that file defines
`MINIAUDIO_IMPLEMENTATION`. Everything else in `src/audio` is plain C++ with no
knowledge that miniaudio exists — the same containment `render/gl.h` gives glad.

Only the device half is used: opening a playback device and being handed a
buffer to fill. The mixing, the 3D attenuation and the panning are ours, in
`src/audio/mixer.cpp`, which is why they can be tested with no device and no
sound card (`--sound-test`).

To update: replace the header, bump the version above, and re-run
`descent --sound-test`.
