#pragma once

namespace dvk {

// Drives the CPU illumination query headlessly: the falloff against an
// independent transcription of the shader's own text, the spot cone, whether
// occlusion does anything at all, and whether the level actually has both lit
// and dark places to stand. Returns 0 on success.
int runLightTest(const char* levelPath);

} // namespace dvk
