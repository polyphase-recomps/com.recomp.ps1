/*
 * The PS1 GPU (Runtime/port/guest/gpu.c), compiled into the host for the wasm2c guest:
 * rasterising in native code is far faster than through the guest's masked,
 * byte-swapped memory. The guest's libgpu/libgs call the gpu_* functions as imports;
 * ps1w_backend.c turns their guest data into native data and calls these.
 */
#include "../../Runtime/port/include/port_host.h"
#include "../../Runtime/port/include/port_gpu.h"

#define PORT_GPU_IN_HOST 1
#include "../../Runtime/port/guest/gpu.c"
