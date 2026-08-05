#pragma once

// Host-only syntax tests need the public stream handle but deliberately do
// not pretend to provide a HIP runtime or execute GPU code.
using hipStream_t = void*;
