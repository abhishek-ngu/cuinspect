#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/WithColor.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/Errno.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/DebugInfo/DWARF/DWARFUnit.h>
#include <llvm/DebugInfo/DWARF/DWARFDie.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>
#include <regex>
#include <map>
#include <cstdio>
#include <memory>
#include <array>
#include <sstream>
#include <fstream>

using namespace llvm;
using namespace llvm::object;

// ============================================================================
// Helper: human-readable size
// ============================================================================
static std::string formatSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024 && unit < 3) {
        size /= 1024;
        unit++;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", size, units[unit]);
    return std::string(buf);
}

// ============================================================================
// Helper: print section with optional content preview (TEXT mode)
// ============================================================================
static void printSection(const SectionRef& Sec, bool previewPtx = false) {
    StringRef Name; Sec.getName(Name);
    uint64_t Size; Sec.getSize(Size);
    if (Size == 0) return;

    std::cout << "  " << std::left << std::setw(22) << Name.str()
              << std::right << std::setw(10) << formatSize(Size);

    if (previewPtx && Name == ".nv.ptx") {
        StringRef Data; Sec.getContents(Data);
        std::string preview = Data.substr(0, 180).str();
        std::replace(preview.begin(), preview.end(), '\n', ' ');
        std::cout << "   [PTX]: " << preview << "...";
    }
    std::cout << "\n";
}

// ============================================================================
// JSON helper: emit a key-value field
// ============================================================================
static void emitJsonField(const std::string& key, const std::string& value, bool last = false) {
    std::cout << "    \"" << key << "\": \"" << value << "\"" << (last ? "" : ",") << "\n";
}

static void emitJsonField(const std::string& key, int64_t value, bool last = false) {
    std::cout << "    \"" << key << "\": " << value << (last ? "" : ",") << "\n";
}

static void emitJsonField(const std::string& key, double value, bool last = false) {
    std::cout << "    \"" << key << "\": " << std::fixed << std::setprecision(1) << value << (last ? "" : ",") << "\n";
}

static void emitJsonField(const std::string& key, bool value, bool last = false) {
    std::cout << "    \"" << key << "\": " << (value ? "true" : "false") << (last ? "" : ",") << "\n";
}

// ============================================================================
// Extract register count from PTX
// ============================================================================
static int extractRegisterCount(const std::string& ptx) {
    std::regex reg32(R"(\.reg\s+\.b32\s+%r<(\d+)>)");
    std::regex reg64(R"(\.reg\s+\.b64\s+%rd<(\d+)>)");
    std::smatch match;
    int maxReg = 0;
    std::string::const_iterator start = ptx.begin();
    while (std::regex_search(start, ptx.end(), match, reg32)) {
        maxReg = std::max(maxReg, std::stoi(match[1].str()));
        start = match.suffix().first;
    }
    start = ptx.begin();
    while (std::regex_search(start, ptx.end(), match, reg64)) {
        maxReg = std::max(maxReg, std::stoi(match[1].str()));
        start = match.suffix().first;
    }
    return maxReg;
}

// ============================================================================
// Extract shared memory from PTX
// ============================================================================
static int extractSharedMem(const std::string& ptx) {
    std::regex shared(R"(\.shared\s+\.b8\s+\w+\[(\d+)\])");
    std::smatch match;
    if (std::regex_search(ptx, match, shared)) {
        return std::stoi(match[1].str());
    }
    return 0;
}

// ============================================================================
// Calculate theoretical occupancy for given CC
// ============================================================================
static double calculateOccupancy(int cc, int regsPerThread, int sharedMemPerBlock, int blockSize = 256) {
    struct SMLimits { int maxWarps; int maxRegs; int maxSharedKB; int maxBlocks; };
    static const std::map<int, SMLimits> limits = {
        {50,  {64, 65536,  48,  32}},  // Maxwell
        {60,  {64, 65536,  96,  32}},  // Pascal
        {70,  {64, 65536,  96,  32}},  // Volta
        {75,  {64, 65536,  96,  32}},  // Turing
        {80,  {64, 65536, 164,  32}},  // Ampere
        {90,  {64, 65536, 228,  32}},  // Hopper
        {100, {64, 65536, 228,  32}},  // Blackwell
    };

    auto it = limits.find(cc);
    if (it == limits.end()) return 0.0;

    const auto& lim = it->second;
    int warpsPerBlock = (blockSize + 31) / 32;

    int maxBlocksByWarps = lim.maxWarps / warpsPerBlock;

    int regsPerBlock = regsPerThread * blockSize;
    int maxBlocksByRegs = (regsPerBlock > 0) ? lim.maxRegs / regsPerBlock : lim.maxBlocks;

    int maxBlocksByShared = (sharedMemPerBlock > 0) ? (lim.maxSharedKB * 1024) / sharedMemPerBlock : lim.maxBlocks;

    int maxBlocks = std::min({maxBlocksByWarps, maxBlocksByRegs, maxBlocksByShared, lim.maxBlocks});

    int achievedWarps = maxBlocks * warpsPerBlock;
    return static_cast<double>(achievedWarps) / lim.maxWarps * 100.0;
}

// ============================================================================
// NEW: Determine what limits occupancy
// ============================================================================
static std::string occupancyLimitingFactor(int cc, int regsPerThread, int sharedMemPerBlock, int blockSize = 256) {
    struct SMLimits { int maxWarps; int maxRegs; int maxSharedKB; int maxBlocks; };
    static const std::map<int, SMLimits> limits = {
        {50,  {64, 65536,  48,  32}},
        {60,  {64, 65536,  96,  32}},
        {70,  {64, 65536,  96,  32}},
        {75,  {64, 65536,  96,  32}},
        {80,  {64, 65536, 164,  32}},
        {90,  {64, 65536, 228,  32}},
        {100, {64, 65536, 228,  32}},
    };

    auto it = limits.find(cc);
    if (it == limits.end()) return "unknown";
    const auto& lim = it->second;

    int warpsPerBlock = (blockSize + 31) / 32;
    int maxBlocksByWarps = lim.maxWarps / warpsPerBlock;

    int regsPerBlock = regsPerThread * blockSize;
    int maxBlocksByRegs = (regsPerBlock > 0) ? lim.maxRegs / regsPerBlock : lim.maxBlocks;

    int maxBlocksByShared = (sharedMemPerBlock > 0) ? (lim.maxSharedKB * 1024) / sharedMemPerBlock : lim.maxBlocks;

    int minBlocks = std::min({maxBlocksByWarps, maxBlocksByRegs, maxBlocksByShared, lim.maxBlocks});

    if (minBlocks == maxBlocksByRegs) return "registers";
    if (minBlocks == maxBlocksByShared) return "shared_memory";
    if (minBlocks == maxBlocksByWarps) return "warps";
    return "block_limit";
}

// ============================================================================
// NEW: Occupancy optimization recommendation
// ============================================================================
static std::string occupancyRecommendation(int cc, int regsPerThread, int sharedMemPerBlock, double occupancy) {
    if (occupancy >= 95.0) {
        return "Occupancy is near-optimal (>=95%). No changes needed.";
    }

    std::string limiter = occupancyLimitingFactor(cc, regsPerThread, sharedMemPerBlock);

    if (limiter == "registers") {
        return "Reduce registers/thread (currently " + std::to_string(regsPerThread) +
               "). Try __launch_bounds__ or lower register pressure to reach higher occupancy.";
    }
    if (limiter == "shared_memory") {
        return "Reduce shared memory per block (currently " + std::to_string(sharedMemPerBlock) +
               " bytes) to allow more resident blocks per SM.";
    }
    if (limiter == "warps") {
        return "Increase block size (warps per block) to better utilize warp capacity.";
    }
    return "Consider reducing block size or increasing grid size to hide latency.";
}

// ============================================================================
// Parse PTX for kernel names
// ============================================================================
static std::vector<std::string> extractKernelNames(const std::string& ptx) {
    std::vector<std::string> kernels;
    std::regex entry(R"(\.entry\s+(\w+)\s*\()");
    std::sregex_iterator it(ptx.begin(), ptx.end(), entry), end;
    for (; it != end; ++it) kernels.push_back((*it)[1].str());
    return kernels;
}

// ============================================================================
// Print DWARF variables for a kernel (TEXT mode)
// ============================================================================
static void printDWARFVariables(DWARFContext* DwarfCtx, const std::string& kernelName) {
    if (!DwarfCtx) return;

    for (const auto& CU : DwarfCtx->compile_units()) {
        for (const auto& DIE : CU->getUnitDIE()) {
            if (DIE.getTag() == dwarf::DW_TAG_subprogram) {
                DWARFFormValue NameVal;
                if (DIE.findAttribute(dwarf::DW_AT_name, NameVal)) {
                    std::string name = NameVal.getAsCString().value_or("");
                    if (name == kernelName || name.find(kernelName) != std::string::npos) {
                        std::cout << "\n=== DWARF Variables (" << kernelName << ") ===\n";
                        for (const auto& Child : DIE.children()) {
                            if (Child.getTag() == dwarf::DW_TAG_variable ||
                                Child.getTag() == dwarf::DW_TAG_formal_parameter) {
                                DWARFFormValue VarName;
                                if (Child.findAttribute(dwarf::DW_AT_name, VarName)) {
                                    std::string varName = VarName.getAsCString().value_or("<anon>");

                                    std::string location = "unknown";
                                    DWARFFormValue LocVal;
                                    if (Child.findAttribute(dwarf::DW_AT_location, LocVal)) {
                                        const DWARFExpression* Expr = LocVal.getAsBlockExpr();
                                        if (Expr) {
                                            location = "DW_OP_...";
                                        }
                                    }

                                    bool isUniform = false;
                                    DWARFFormValue UniformVal;
                                    if (Child.findAttribute(dwarf::DW_AT_NVPTX_uniform, UniformVal)) {
                                        isUniform = UniformVal.getAsUnsignedConstant().value_or(0);
                                    }

                                    std::cout << "  " << std::left << std::setw(16) << varName
                                              << " location: " << location
                                              << (isUniform ? " (uniform)" : " (divergent)") << "\n";
                                }
                            }
                        }
                        return;
                    }
                }
            }
        }
    }
}

// ============================================================================
// NEW: SASS instruction mix analyzer via nvdisasm
// ============================================================================
struct SassMix {
    int fma = 0;
    int ldst = 0;
    int ctrl = 0;
    int tex = 0;
    int sync = 0;
    int other = 0;
    int total = 0;
};

static std::string runNvdisasm(const std::string& cubinPath) {
    std::string cmd = "nvdisasm \"" + cubinPath + "\" 2>/dev/null";
    std::array<char, 256> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

static SassMix analyzeSassMix(const std::string& disasm) {
    SassMix m;
    std::istringstream iss(disasm);
    std::string line;
    while (std::getline(iss, line)) {
        // Skip empty lines and header lines that don't start with whitespace
        if (line.empty()) continue;
        if (line[0] != ' ' && line[0] != '\t') continue;

        std::string upper = line;
        for (auto& c : upper) c = std::toupper(static_cast<unsigned char>(c));

        if (upper.find("FMA") != std::string::npos || upper.find("HMMA") != std::string::npos ||
            upper.find("MUFU") != std::string::npos || upper.find("DMUL") != std::string::npos ||
            upper.find("DADD") != std::string::npos || upper.find("DMIN") != std::string::npos ||
            upper.find("DMAX") != std::string::npos || upper.find("ADD") != std::string::npos ||
            upper.find("MUL") != std::string::npos || upper.find("MAD") != std::string::npos) {
            m.fma++;
        } else if (upper.find("LD") != std::string::npos || upper.find("ST") != std::string::npos ||
                   upper.find("LDG") != std::string::npos || upper.find("STG") != std::string::npos ||
                   upper.find("LDS") != std::string::npos || upper.find("STS") != std::string::npos ||
                   upper.find("LDC") != std::string::npos || upper.find("LDU") != std::string::npos ||
                   upper.find("ATOM") != std::string::npos || upper.find("RED") != std::string::npos) {
            m.ldst++;
        } else if (upper.find("BRA") != std::string::npos || upper.find("JMP") != std::string::npos ||
                   upper.find("RET") != std::string::npos || upper.find("CAL") != std::string::npos ||
                   upper.find("EXIT") != std::string::npos || upper.find("PRED") != std::string::npos ||
                   upper.find("SEL") != std::string::npos || upper.find("SELP") != std::string::npos) {
            m.ctrl++;
        } else if (upper.find("TEX") != std::string::npos || upper.find("LDG") != std::string::npos ||
                   upper.find("SULD") != std::string::npos || upper.find("SUST") != std::string::npos ||
                   upper.find("SURED") != std::string::npos || upper.find("SAMP") != std::string::npos) {
            m.tex++;
        } else if (upper.find("SYNC") != std::string::npos || upper.find("BAR") != std::string::npos ||
                   upper.find("MEMBAR") != std::string::npos || upper.find("FENCE") != std::string::npos) {
            m.sync++;
        } else {
            m.other++;
        }
        m.total++;
    }
    return m;
}

static void printSassMix(const SassMix& mix, bool jsonMode) {
    if (jsonMode) {
        std::cout << "  \"sass_instruction_mix\": {\n";
        emitJsonField("fma", mix.fma, false);
        emitJsonField("load_store", mix.ldst, false);
        emitJsonField("control", mix.ctrl, false);
        emitJsonField("texture", mix.tex, false);
        emitJsonField("sync", mix.sync, false);
        emitJsonField("other", mix.other, false);
        emitJsonField("total", mix.total, true);
        std::cout << "  },\n";
    } else {
        std::cout << "\n=== SASS Instruction Mix ===\n";
        std::cout << "  FMA/Compute:   " << mix.fma << " (" 
                  << (mix.total > 0 ? std::to_string((mix.fma * 100) / mix.total) : "0") << "%)\n";
        std::cout << "  Load/Store:    " << mix.ldst << " (" 
                  << (mix.total > 0 ? std::to_string((mix.ldst * 100) / mix.total) : "0") << "%)\n";
        std::cout << "  Control:       " << mix.ctrl << " (" 
                  << (mix.total > 0 ? std::to_string((mix.ctrl * 100) / mix.total) : "0") << "%)\n";
        std::cout << "  Texture:       " << mix.tex << " (" 
                  << (mix.total > 0 ? std::to_string((mix.tex * 100) / mix.total) : "0") << "%)\n";
        std::cout << "  Sync/Barrier:  " << mix.sync << " (" 
                  << (mix.total > 0 ? std::to_string((mix.sync * 100) / mix.total) : "0") << "%)\n";
        std::cout << "  Other:         " << mix.other << " (" 
                  << (mix.total > 0 ? std::to_string((mix.other * 100) / mix.total) : "0") << "%)\n";
        std::cout << "  Total:         " << mix.total << "\n";
    }
}

// ============================================================================
// Print DWARF variables for a kernel (JSON mode)
// ============================================================================
static void printDWARFVariablesJson(DWARFContext* DwarfCtx, const std::string& kernelName, bool first) {
    if (!DwarfCtx) return;

    for (const auto& CU : DwarfCtx->compile_units()) {
        for (const auto& DIE : CU->getUnitDIE()) {
            if (DIE.getTag() == dwarf::DW_TAG_subprogram) {
                DWARFFormValue NameVal;
                if (DIE.findAttribute(dwarf::DW_AT_name, NameVal)) {
                    std::string name = NameVal.getAsCString().value_or("");
                    if (name == kernelName || name.find(kernelName) != std::string::npos) {
                        if (!first) std::cout << ",\n";
                        std::cout << "      \"variables\": [\n";
                        bool firstVar = true;
                        for (const auto& Child : DIE.children()) {
                            if (Child.getTag() == dwarf::DW_TAG_variable ||
                                Child.getTag() == dwarf::DW_TAG_formal_parameter) {
                                DWARFFormValue VarName;
                                if (Child.findAttribute(dwarf::DW_AT_name, VarName)) {
                                    std::string varName = VarName.getAsCString().value_or("<anon>");

                                    std::string location = "unknown";
                                    DWARFFormValue LocVal;
                                    if (Child.findAttribute(dwarf::DW_AT_location, LocVal)) {
                                        const DWARFExpression* Expr = LocVal.getAsBlockExpr();
                                        if (Expr) {
                                            location = "DW_OP_...";
                                        }
                                    }

                                    bool isUniform = false;
                                    DWARFFormValue UniformVal;
                                    if (Child.findAttribute(dwarf::DW_AT_NVPTX_uniform, UniformVal)) {
                                        isUniform = UniformVal.getAsUnsignedConstant().value_or(0);
                                    }

                                    if (!firstVar) std::cout << ",\n";
                                    std::cout << "        {";
                                    emitJsonField("name", varName, false);
                                    emitJsonField("location", location, false);
                                    emitJsonField("uniform", isUniform, true);
                                    std::cout << "}";
                                    firstVar = false;
                                }
                            }
                        }
                        std::cout << "\n      ]";
                        return;
                    }
                }
            }
        }
    }
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    bool jsonMode = false;
    if (argc < 2 || argc > 3) {
        llvm::errs() << "Usage: " << argv[0] << " <file.cubin|file.elf> [--json]\n";
        return 1;
    }

    if (argc == 3 && std::string(argv[2]) == "--json") {
        jsonMode = true;
    }

    // === Load file ===
    ErrorOr<std::unique_ptr<MemoryBuffer>> BufferOrErr =
        MemoryBuffer::getFile(argv[1]);
    if (std::error_code EC = BufferOrErr.getError()) {
        llvm::errs() << "Error reading file: " << EC.message() << "\n";
        return 1;
    }

    // === Parse as ELF ===
    Expected<std::unique_ptr<ObjectFile>> ObjOrErr =
        ObjectFile::createELFObjectFile(BufferOrErr.get()->getMemBufferRef());
    if (!ObjOrErr) {
        llvm::errs() << "Not a valid ELF: " << toString(ObjOrErr.takeError()) << "\n";
        return 1;
    }

    ELFObjectFileBase* ELF = dyn_cast<ELFObjectFileBase>(ObjOrErr->get());
    if (!ELF) {
        llvm::errs() << "Not an ELF file\n";
        return 1;
    }

    // === Create DWARF context ===
    std::unique_ptr<DWARFContext> DwarfCtx = DWARFContext::create(*ELF);

    // === JSON header ===
    if (jsonMode) {
        std::cout << "{\n";
        emitJsonField("file", argv[1], false);
        emitJsonField("elf_class", ELF->getBytesInAddress() == 8 ? "ELF64" : "ELF32", false);
        emitJsonField("machine", ELF->getArch(), false);
        emitJsonField("entry", static_cast<int64_t>(ELF->getEntry()), false);
        std::cout << "  \"sections\": [\n";
    } else {
        std::cout << "=== ELF Header ===\n";
        std::cout << "  Class: " << (ELF->getBytesInAddress() == 8 ? "ELF64" : "ELF32") << "\n";
        std::cout << "  Machine: " << ELF->getArch() << "\n";
        std::cout << "  Entry: 0x" << std::hex << ELF->getEntry() << std::dec << "\n\n";

        std::cout << "=== Sections ===\n";
    }

    // === Enumerate sections ===
    bool hasPtx = false;
    std::string ptxContent;
    int ptxSectionIdx = 0;
    int totalSections = 0;

    // Count non-empty sections first (for JSON comma handling)
    for (const SectionRef& Sec : ELF->sections()) {
        uint64_t Size; Sec.getSize(Size);
        if (Size > 0) totalSections++;
    }

    int sectionCounter = 0;
    for (const SectionRef& Sec : ELF->sections()) {
        StringRef Name; Sec.getName(Name);
        uint64_t Size; Sec.getSize(Size);
        if (Size == 0) continue;

        sectionCounter++;
        bool isLast = (sectionCounter == totalSections);

        if (Name == ".nv.ptx") {
            hasPtx = true;
            Sec.getContents(ptxContent);
        }

        if (jsonMode) {
            std::cout << "    {\n";
            emitJsonField("name", Name.str(), false);
            emitJsonField("size", static_cast<int64_t>(Size), false);
            emitJsonField("is_gpu_section", Name.startswith(".nv."), true);
            std::cout << "    }" << (isLast ? "" : ",") << "\n";
        } else {
            printSection(Sec, true);
        }
    }

    if (jsonMode) {
        std::cout << "  ],\n";
    } else {
        std::cout << "\n=== DWARF Debug Info ===\n";
        bool hasDwarf = false;
        for (const SectionRef& Sec : ELF->sections()) {
            StringRef Name; Sec.getName(Name);
            if (Name.startswith(".debug_")) {
                hasDwarf = true;
                uint64_t Size; Sec.getSize(Size);
                std::cout << "  " << Name.str() << ": " << formatSize(Size) << "\n";
            }
        }
        if (!hasDwarf) std::cout << "  (none found - compile with -g)\n";
    }

    // === Kernel Analysis ===
    if (hasPtx) {
        auto kernels = extractKernelNames(ptxContent);
        int regs = extractRegisterCount(ptxContent);
        int sharedMem = extractSharedMem(ptxContent);

        // Detect architecture from .nv.info or default to 90
        int cc = 90;
        for (const SectionRef& Sec : ELF->sections()) {
            StringRef Name; Sec.getName(Name);
            if (Name == ".nv.info") {
                StringRef Data; Sec.getContents(Data);
                std::regex smRx(R"(sm_(\d+))");
                std::smatch m;
                if (std::regex_search(Data.str(), m, smRx)) {
                    cc = std::stoi(m[1].str());
                }
            }
        }

        if (jsonMode) {
            std::cout << "  \"kernels\": [\n";
        } else {
            std::cout << "\n=== Kernel Analysis ===\n";
        }

        for (size_t k = 0; k < kernels.size(); ++k) {
            const auto& kernel = kernels[k];
            bool isLastKernel = (k == kernels.size() - 1);
            double occupancy = calculateOccupancy(cc, regs, sharedMem);
            std::string limiter = occupancyLimitingFactor(cc, regs, sharedMem);
            std::string recommendation = occupancyRecommendation(cc, regs, sharedMem, occupancy);

            if (jsonMode) {
                std::cout << "    {\n";
                emitJsonField("name", kernel, false);
                emitJsonField("registers_per_thread", regs, false);
                emitJsonField("shared_mem_per_block", sharedMem, false);
                emitJsonField("compute_capability", cc, false);
                emitJsonField("theoretical_occupancy_pct", occupancy, false);
                emitJsonField("occupancy_limited_by", limiter, false);
                emitJsonField("recommendation", recommendation, true);
                printDWARFVariablesJson(DwarfCtx.get(), kernel, k == 0);
                std::cout << "\n    }" << (isLastKernel ? "" : ",") << "\n";
            } else {
                std::cout << "\n=== Kernel: " << kernel << " ===\n";
                std::cout << "  Registers: " << regs << "/thread\n";
                std::cout << "  Shared Mem: " << (sharedMem > 0 ? formatSize(sharedMem) + "/block" : "0") << "\n";
                std::cout << "  Theoretical Occupancy: " << std::fixed << std::setprecision(0)
                          << occupancy << "% (sm_" << cc << ")\n";
                std::cout << "  Limited by: " << limiter << "\n";
                std::cout << "  Recommendation: " << recommendation << "\n";

                printDWARFVariables(DwarfCtx.get(), kernel);
            }
        }

        if (jsonMode) {
            std::cout << "  ],\n";
        }
    }

    // === SASS Instruction Mix ===
    bool hasSass = false;
    for (const SectionRef& Sec : ELF->sections()) {
        StringRef Name; Sec.getName(Name);
        if (Name == ".nv.sass") {
            hasSass = true;
            break;
        }
    }

    if (hasSass) {
        if (jsonMode) {
            // We'll embed SASS mix before closing
        } else {
            std::string disasm = runNvdisasm(argv[1]);
            SassMix mix = analyzeSassMix(disasm);
            printSassMix(mix, false);
        }
    }

    // === JSON footer ===
    if (jsonMode) {
        if (hasSass) {
            std::string disasm = runNvdisasm(argv[1]);
            SassMix mix = analyzeSassMix(disasm);
            printSassMix(mix, true);
        }
        std::cout << "}\n";
    }

    return 0;
}