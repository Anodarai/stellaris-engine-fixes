// Locates the ship design library's designer functions in a stellaris.exe mapped as an image.
#include "../src/interface/ship_design/shipdesignlibrary_patterns.h"
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: shipdesignlibrary_native_test executable\n");
        return 1;
    }
    HMODULE module = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module)
        return 1;
    auto* base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module) & ~uintptr_t(3));
    auto image = locate::Describe(base);
    shipdesignlibrary::Locations l;
    std::string error;
    if (!shipdesignlibrary::Locate(image, l, error)) {
        std::fprintf(stderr, "ship design library: %s\n", error.c_str());
        return 1;
    }
    auto rva = [&](const void* p) { return static_cast<size_t>(static_cast<const uint8_t*>(p) - base); };
    std::printf("ctor=%zx dtor=%zx growth=%zx load=%zx role=%zx allowed=%zx canBuild=%zx stage=%zx\n",
                rva(l.designCtor), rva(l.designDtor), rva(l.createGrowthStages), rva(l.loadDesign), rva(l.rolePicker),
                rva(l.allowed), rva(l.canBuild), rva(l.growthStage));
    std::printf("sectionSet=%zx resources=%zx recalc=%zx setSection=%zx popup=%zx prereqs=%zx\n",
                rva(l.sectionSetComponent), rva(l.updateResources), rva(l.recalc), rva(l.setSection), rva(l.popupOk),
                rva(l.sectionPrereqs));
    std::printf("idler=%zx shipSizes=%zx templates=%zx\n", rva(l.idler), rva(l.shipSizes), rva(l.templates));
    // The three globals are 8-byte slots in a data section (.data is uninitialized in the file and
    // still mapped, so check the whole image instead of initialized sections only).
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    for (auto* g : {l.idler, l.shipSizes, l.templates}) {
        const auto at = rva(g);
        bool code = false;
        for (const auto& s : image.code)
            code |= reinterpret_cast<uint8_t*>(g) >= s.begin && reinterpret_cast<uint8_t*>(g) < s.end;
        if (at == 0 || at + 8 > imageSize || code || at % 8 != 0) {
            std::fprintf(stderr, "global %zx is not a data slot\n", at);
            return 1;
        }
    }
    if (l.idler == l.shipSizes || l.shipSizes == l.templates) {
        std::fprintf(stderr, "globals coincide\n");
        return 1;
    }
    std::puts("ship design library locations found");
    return 0;
}
