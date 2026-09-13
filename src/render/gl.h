#pragma once

// Single place that pulls in the GL loader. Include this, never <glad/glad.h>,
// so the loader can be swapped without touching call sites.
#include <glad/glad.h>
