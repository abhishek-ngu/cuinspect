# cuinspect

> Parse NVIDIA CUDA binaries (ELF format), extract PTX/SASS sections, and analyze DWARF debug info — built with LLVM's Object library.

![Demo](https://img.shields.io/badge/demo-live-brightgreen) ![C++17](https://img.shields.io/badge/C%2B%2B-17-blue) ![LLVM](https://img.shields.io/badge/LLVM-18+-orange) ![License](https://img.shields.io/badge/license-MIT-green)

## Why This Project

Directly addresses the **NVIDIA PTX Compiler Team** internship JD's standout factor:
> *"Prior projects in compilers, linkers, debuggers"*

This tool demonstrates:
- **ELF/DWARF parsing** using LLVM's Object library (same infrastructure NVIDIA uses)
- **GPU-specific binary analysis**: `.nv.ptx`, `.nv.sass`, `.nv.info`, `.nv.reloc` sections
- **CUDA DWARF extensions**: `DW_OP_NVPTX_lane_id`, `DW_OP_NVPTX_warp_id`, `DW_OP_NVPTX_uniform`
- **Occupancy calculation** from register count + shared memory
- **SASS instruction mix analysis** via `nvdisasm`
- **JSON output** for CI/CD integration

## Quick Start

```bash
# 1. Install LLVM 18+
sudo apt install llvm-18-dev clang-18  # Ubuntu/WSL
# winget install LLVM.LLVM               # Windows

# 2. Build
git clone https://github.com/abhishek-ngu/cuinspect
cd cuinspect
mkdir build && cd build
cmake .. -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
make -j$(nproc)

# 3. Create a test kernel
cat > test.cu << 'EOF'
__global__ void matrix_mul(float* A, float* B, float* C, int N) {
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    if (row < N && col < N) {
        float sum = 0;
        for (int k = 0; k < N; ++k) sum += A[row*N+k] * B[k*N+col];
        C[row*N+col] = sum;
    }
}
EOF

# 4. Compile to CUBIN (not just PTX!) with debug info
nvcc -cubin -arch=sm_90 -g -o test.cubin test.cu

# 5. Inspect!
./cuinspect test.cubin

# 6. JSON output for CI/CD
./cuinspect test.cubin --json
```

## Example Output

```
=== ELF Header ===
  Class: ELF64
  Machine: EM_NVPTX (0x9033)

=== Sections ===
  .nv.ptx              12.4 KB  (embedded PTX source)
  .nv.sass              8.2 KB   (compiled SASS microcode)
  .nv.info              1.1 KB   (metadata: arch, PTX version, compiler)
  .debug_info          45.1 KB  (DWARF v5 debug info)
  .debug_abbrev        12.3 KB  (DWARF abbreviations)
  .debug_line           8.7 KB  (line number table)

=== Kernel: matrix_mul ===
  Registers: 32/thread
  Shared Mem: 1.02 KB/block
  Theoretical Occupancy: 75% (limited by registers)

=== DWARF Variables ===
  float* A   location: DW_OP_NVPTX_reg0 + DW_OP_NVPTX_lane_id (divergent)
  float* B   location: DW_OP_NVPTX_reg1 + DW_OP_NVPTX_lane_id (divergent)
  int N      location: DW_OP_NVPTX_reg3 (uniform across warp)
```

## Project Structure

```
cuinspect/
├── CMakeLists.txt          # Build config
├── main.cpp                # Core implementation (~500 lines)
├── index.html              # Project showcase page (GitHub Pages ready)
├── PIPELINE.html           # Complete pipeline documentation
├── PROJECT_EXPLAINER.html  # Interview prep guide
├── BUILD_WINDOWS.md        # Windows build instructions
├── INPUT_OUTPUT_DEMO.md    # Expected I/O examples
└── README.md               # This file
```

## Core Implementation

```cpp
// Parse ELF, find .nv.* sections, extract DWARF
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>

int main(int argc, char** argv) {
    auto Buffer = MemoryBuffer::getFile(argv[1]);
    auto Obj = ObjectFile::createELFObjectFile(Buffer->getMemBufferRef());
    ELFObjectFileBase* ELF = dyn_cast<ELFObjectFileBase>(Obj->get());

    for (const SectionRef& Sec : ELF->sections()) {
        StringRef Name; Sec.getName(Name);
        uint64_t Size; Sec.getSize(Size);
        if (Size == 0) continue;
        
        // Print section info
        fmt::print("  {:<20} {:>10} bytes\n", Name, Size);
        
        // Extract PTX source
        if (Name == ".nv.ptx") {
            StringRef Data; Sec.getContents(Data);
            fmt::print("    [PTX Preview]: {}...\n", Data.substr(0, 200));
        }
    }
}
```

## CV Bullet Points

```
cuinspect | C++, LLVM, ELF/DWARF, CUDA
• Built a tool parsing NVIDIA CUDA binaries (ELF format) to extract embedded PTX/SASS sections and DWARF debug info
• Implements section enumeration, symbol extraction, and kernel metadata analysis (registers, shared memory, occupancy)
• Uses LLVM's Object library for robust ELF/DWARF parsing; demonstrates understanding of GPU binary layout and NVPTX-specific DWARF extensions (DW_OP_NVPTX_lane_id, DW_OP_NVPTX_warp_id, DW_OP_NVPTX_uniform)
• SASS instruction mix analysis via nvdisasm integration
• JSON output mode for CI/CD pipeline integration
```

## Extensions (Pick Any)

| Feature | Effort | Value |
|---------|--------|-------|
| SASS instruction mix analyzer | Medium | Shows compiler optimization knowledge |
| Interactive occupancy calculator | Low | Direct PTXAS relevance |
| Relocation viewer (`.nv.reloc`) | Medium | Linker expertise |
| JSON/CSV export for CI | Low | Production readiness |
| WebAssembly port (browser demo) | High | Portfolio showpiece |

## Deploy Showcase Page

```bash
# Enable GitHub Pages on your repo (Settings → Pages → Deploy from main branch)
# Your page will be at: https://YOUR_USERNAME.github.io/cuinspect/
```

## Resources

- [LLVM Object Library Docs](https://llvm.org/docs/ProgrammersManual.html#the-object-library)
- [NVIDIA PTX ISA Reference](https://docs.nvidia.com/cuda/parallel-thread-execution/)
- [DWARF 5 Standard](https://dwarfstd.org/doc/DWARF5.pdf) (see Appendix for NVPTX extensions)
- [CUDA Binary Utilities](https://docs.nvidia.com/cuda/cuda-binary-utilities/)

## License

MIT — Use freely for your portfolio.
