// Checks the IR disk cache tiers without a device: a blocking cache with no
// byte limit keeps every write, and the main cache neither drops more than its
// queue holds nor counts the aot subdirectory.
#include "MVKMetalIRCache.h"
#include <cstdio>
#include <string>
#include <unistd.h>

namespace {
std::string keyOf(unsigned index) {
    char text[65];
    snprintf(text, sizeof(text), "%064x", index);
    return text;
}

bool store(mvkir::DiskCache& cache, unsigned index, size_t bytes) {
    std::vector<uint8_t> metallib(bytes, uint8_t(index));
    MVKMetalIRCompileResult result{};
    result.abiVersion = MVK_METAL_IR_ABI_VERSION;
    result.metallib = metallib.data();
    result.metallibSize = metallib.size();
    snprintf(result.entry, sizeof(result.entry), "main%u", index);
    mvkir::DiskCache::Reflection reflection;
    reflection.usedSets = index;
    reflection.usedBindings = {index};
    cache.store(keyOf(index), 0, result, reflection);
    return true;
}

bool loads(mvkir::DiskCache& cache, unsigned index, size_t bytes) {
    MVKMetalIRCompileResult result{};
    mvkir::DiskCache::Reflection reflection;
    if (!cache.load(keyOf(index), 0, result, reflection)) return false;
    bool good = result.metallibSize == bytes && ((uint8_t*)result.metallib)[bytes - 1] == uint8_t(index) &&
                reflection.usedSets == index && reflection.usedBindings.size() == 1;
    free(result.metallib);
    return good;
}

int fail(const char* message) {
    fprintf(stderr, "disk_cache_probe: %s\n", message);
    return 1;
}
}

int main() {
    char pattern[] = "/tmp/mvk-disk-cache-XXXXXX";
    if (!mkdtemp(pattern)) return fail("no temporary directory");
    const std::filesystem::path root = pattern, aot = root / "aot";
    constexpr unsigned count = 160;
    constexpr size_t bytes = 512 * 1024; // 80 MiB in total, five times the write queue.

    {
        mvkir::DiskCache cache(aot, 0, true);
        if (!cache.enabled()) return fail("aot cache not enabled");
        for (unsigned i = 0; i < count; ++i) store(cache, i, bytes);
        cache.waitIdle();
        auto stats = cache.statistics();
        if (stats.droppedWrites || stats.failedWrites || stats.completedWrites != count || stats.entries != count)
            return fail("blocking cache lost writes");
        // An entry that only fits an empty write queue still goes through.
        store(cache, count, 16 * 1024 * 1024);
        cache.waitIdle();
        if (cache.statistics().completedWrites != count + 1) return fail("blocking cache dropped a large entry");
    }
    {
        mvkir::DiskCache cache(aot, 0, true);
        if (cache.statistics().entries != count + 1) return fail("aot entries trimmed on open");
        if (!loads(cache, 0, bytes) || !loads(cache, count - 1, bytes)) return fail("aot entry did not load");
    }
    {
        // The main cache keeps its old behavior: it drops what its queue cannot
        // hold, trims to its limit, and does not see the aot subdirectory.
        mvkir::DiskCache cache(root, 4 * 1024 * 1024);
        if (cache.statistics().entries) return fail("main cache indexed the aot subdirectory");
        for (unsigned i = 0; i < count; ++i) store(cache, 1000 + i, bytes);
        cache.waitIdle();
        auto stats = cache.statistics();
        if (!stats.droppedWrites) return fail("main cache did not drop writes past its queue");
        if (stats.indexedBytes > 4 * 1024 * 1024) return fail("main cache not trimmed");
    }
    {
        mvkir::DiskCache cache(aot, 0, true);
        if (cache.statistics().entries != count + 1) return fail("main cache trim touched aot entries");
    }

    std::error_code error;
    std::filesystem::remove_all(root, error);
    printf("disk_cache_probe: ok\n");
    return 0;
}
