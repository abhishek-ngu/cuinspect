# Building on Windows

## Prerequisites (install in order)

1. **Visual Studio 2022 Community/Build Tools** with **"Desktop development with C++"** workload
   - Installer: https://visualstudio.microsoft.com/downloads/
   - Select: "Desktop development with C++" (includes Windows 10 SDK, MSVC, CMake)

2. **LLVM 18+** 
   - Download: https://github.com/llvm/llvm-project/releases/tag/llvmorg-18.1.8
   - File: `LLVM-18.1.8-win64.exe`
   - **Important**: Check "Add LLVM to system PATH" during install

3. **CUDA Toolkit 12.x or 13.x**
   - Download: https://developer.nvidia.com/cuda-downloads
   - Windows → x86_64 → 11/12 → exe (local)

4. **CMake 3.16+** (included with VS 2022 workload, or: `winget install Kitware.CMake`)

---

## Build Steps (Developer Command Prompt)

Open **"Developer Command Prompt for VS 2022"** (or "x64 Native Tools Command Prompt") — this sets up `cl.exe`, `link.exe`, Windows SDK paths automatically.

```cmd
cd C:\Nvidia\cuinspect
mkdir build
cd build
cmake .. -G "Ninja" -DCMAKE_CXX_COMPILER=clang-cl -DLLVM_DIR="C:\Program Files\LLVM\lib\cmake\llvm"
cmake --build .
```

Or with Visual Studio generator:
```cmd
cmake .. -G "Visual Studio 17 2022" -A x64 -DLLVM_DIR="C:\Program Files\LLVM\lib\cmake\llvm"
cmake --build . --config Release
```

---

## Test

```cmd
# Create test kernel
echo __global__ void add_kernel(float* a, float* b, float* c, int n) { int i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) c[i] = a[i] + b[i]; } > test.cu

# Compile CUBIN
nvcc -cubin -arch=sm_90 -g -o test.cubin test.cu

# Run (text mode)
.\cuinspect.exe test.cubin

# Run (JSON mode)
.\cuinspect.exe test.cubin --json
```

---

## Common Issues

| Issue | Fix |
|-------|-----|
| `lld-link: error: could not open 'kernel32.lib'` | Use **Developer Command Prompt** (sets VC++ env) |
| `CMAKE_CXX_COMPILER not set` | Use `-DCMAKE_CXX_COMPILER=clang-cl` or open VS Developer Prompt |
| `Visual Studio 17 2022 not found` | Install VS 2022 with C++ workload, or use `-G "Ninja"` |
| `LLVM_DIR not found` | Point to `C:\Program Files\LLVM\lib\cmake\llvm` (or your install path) |
| `nvcc not found` | Add `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\vXX.X\bin` to PATH |

---

## Recommended: Use WSL2 / Linux

Building on Ubuntu/WSL is significantly easier:
```bash
sudo apt install llvm-18-dev clang-18 cmake build-essential
# Install CUDA Toolkit for WSL: https://developer.nvidia.com/cuda-downloads
cmake -B build -DCMAKE_CXX_COMPILER=clang++-18 -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build -j$(nproc)
```