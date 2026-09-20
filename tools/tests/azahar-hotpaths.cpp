#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>
#include <vector>
#include <boost/circular_buffer.hpp>
#include "video_core/pica/vertex_cache.h"
#include "video_core/shader/shader_stack.h"

// Standalone differential checks against the implementations used before optimization.
// They can run in native ASan/UBSan builds and in WebAssembly under Node.
static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

template <typename T, std::size_t N>
struct OriginalStack : boost::circular_buffer<T> {
    OriginalStack() : boost::circular_buffer<T>(N) {}
};

template <std::size_t N>
static void CheckStack() {
    OriginalStack<std::uint32_t, N> original;
    Pica::Shader::ShaderStack<std::uint32_t, N> candidate;
    std::mt19937 random{1234};
    for (unsigned i = 0; i < 100000; ++i) {
        if (original.empty() || random() % 4 != 0) {
            const auto value = random();
            original.push_back(value);
            candidate.push_back(value);
        } else {
            // The interpreter mutates the top loop counter before popping it.
            original.back() ^= i;
            candidate.back() ^= i;
            Check(original.back() == candidate.back(), "stack mutable top");
            original.pop_back();
            candidate.pop_back();
        }
        Check(original.size() == candidate.size(), "stack size after wrap/overflow");
        Check(original.empty() == candidate.empty(), "stack empty");
        if (!original.empty()) {
            Check(original.back() == candidate.back(), "stack contents after wrap/overflow");
        }
    }
    while (!original.empty()) {
        Check(original.back() == candidate.back(), "stack drain order");
        original.pop_back();
        candidate.pop_back();
    }
    Check(candidate.empty(), "stack fully drained");
}

struct OriginalVertexCacheIndex {
    static constexpr unsigned Capacity = 64;
    static constexpr unsigned Missing = Capacity;
    std::array<bool, Capacity> valid{};
    std::array<std::uint16_t, Capacity> vertices;
    unsigned next = 0;

    unsigned Find(std::uint16_t vertex) {
        for (unsigned i = 0; i < Capacity; ++i) {
            if (valid[i] && vertices[i] == vertex) {
                return i;
            }
        }
        return Missing;
    }

    unsigned Insert(std::uint16_t vertex) {
        const auto slot = next;
        vertices[slot] = vertex;
        valid[slot] = true;
        next = (next + 1) % Capacity;
        return slot;
    }
};

static void CheckVertices() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        OriginalVertexCacheIndex original;
        Pica::VertexCacheIndex candidate;
        std::mt19937 random{5678};
        for (unsigned i = 0; i < 200000; ++i) {
            const std::uint16_t vertex = mode == 0 ? random() % 64
                                         : mode == 1 ? (random() % 64) * 257
                                         : mode == 2 ? random() % 80
                                         : mode == 3 ? i
                                                     : random();
            const auto expected = original.Find(vertex);
            const auto actual = candidate.Find(vertex);
            Check(expected == actual, "vertex hit, slot, or FIFO replacement changed");
            if (expected == OriginalVertexCacheIndex::Missing) {
                Check(original.Insert(vertex) == candidate.Insert(vertex), "vertex insertion slot");
            }
        }
    }
}

struct ControlEntry {
    std::uint32_t address;
    std::uint32_t counter;
};

template <template <typename, std::size_t> class Stack>
[[gnu::noinline]] static std::uint32_t RunStacks(std::uint32_t seed) {
    Stack<ControlEntry, 8> ifs;
    Stack<ControlEntry, 4> calls;
    Stack<ControlEntry, 4> loops;
    for (unsigned i = 0; i < (seed & 7); ++i) {
        ifs.push_back({seed + i, i});
        calls.push_back({seed ^ i, i});
        loops.push_back({seed - i, i});
    }
    std::uint32_t result = seed;
    while (!ifs.empty()) {
        result += ifs.back().address;
        ifs.pop_back();
    }
    while (!calls.empty()) {
        result ^= calls.back().address;
        calls.pop_back();
    }
    while (!loops.empty()) {
        result += loops.back().address + loops.back().counter;
        loops.pop_back();
    }
    return result;
}

template <typename Cache>
[[gnu::noinline]] static std::uint64_t RunVertices(const std::vector<std::uint16_t>& vertices,
                                                unsigned batch_size, std::uint16_t seed) {
    std::uint64_t result = 0;
    for (unsigned batch = 0; batch < vertices.size(); batch += batch_size) {
        Cache cache;
        for (unsigned i = batch; i < std::min<unsigned>(batch + batch_size, vertices.size()); ++i) {
            const auto vertex = static_cast<std::uint16_t>(vertices[i] ^ seed);
            auto slot = cache.Find(vertex);
            if (slot == Cache::Missing) {
                slot = cache.Insert(vertex);
            }
            result += slot;
        }
    }
    return result;
}

template <typename Function>
static double Measure(Function fn, std::uint64_t& checksum) {
    static volatile std::uint32_t seed = 0;
    std::array<double, 5> times;
    for (auto& time : times) {
        const auto start = std::chrono::steady_clock::now();
        checksum = fn(seed);
        asm volatile("" : "+r"(checksum) : : "memory");
        time = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    std::sort(times.begin(), times.end());
    return times[2];
}

int main(int argc, char** argv) {
    CheckStack<1>();
    CheckStack<4>();
    CheckStack<8>();
    CheckVertices();
    std::puts("PASS: 300,000 stack operations and 1,000,000 FIFO cache lookups");
    if (argc > 1 && std::string_view{argv[1]} == "--check-only") {
        return 0;
    }
    std::uint64_t old_checksum = 0, new_checksum = 0;
    const auto old_stack = Measure([](std::uint32_t seed) {
        std::uint64_t sum = 0;
        for (unsigned i = 0; i < 1000000; ++i) sum += RunStacks<OriginalStack>(i + seed);
        return sum;
    }, old_checksum);
    const auto new_stack = Measure([](std::uint32_t seed) {
        std::uint64_t sum = 0;
        for (unsigned i = 0; i < 1000000; ++i) sum += RunStacks<Pica::Shader::ShaderStack>(i + seed);
        return sum;
    }, new_checksum);
    Check(old_checksum == new_checksum, "stack benchmark checksum");
    std::printf("stacks: original=%.3f ms candidate=%.3f ms checksum=%llu\n", old_stack, new_stack,
                static_cast<unsigned long long>(new_checksum));
    for (unsigned mode = 0; mode < 3; ++mode) {
        std::vector<std::uint16_t> vertices(1000000);
        std::mt19937 random{9012};
        for (auto& vertex : vertices) {
            vertex = mode == 0 ? random() % 32 : mode == 1 ? random() % 80 : (random() % 64) * 257;
        }
        for (const unsigned batch : {48, 512}) {
            const auto old_time = Measure([&](std::uint32_t seed) { return RunVertices<OriginalVertexCacheIndex>(vertices, batch, seed); }, old_checksum);
            const auto new_time = Measure([&](std::uint32_t seed) { return RunVertices<Pica::VertexCacheIndex>(vertices, batch, seed); }, new_checksum);
            Check(old_checksum == new_checksum, "vertex benchmark checksum");
            std::printf("vertices mode=%u batch=%u: original=%.3f ms candidate=%.3f ms checksum=%llu\n",
                        mode, batch, old_time, new_time, static_cast<unsigned long long>(new_checksum));
        }
    }
}
