// The one translation unit that compiles the cgltf implementation. The header is third-party
// code outside the repository (it comes from the build environment) and is included as a system
// header, so project warnings and clang-tidy do not apply to it.
#define CGLTF_IMPLEMENTATION
#include "cgltf_backend.hpp"
