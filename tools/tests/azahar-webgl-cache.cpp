#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_opengl/gl_webgl_cache.h"

static void Check(bool value) {
    if (!value) std::abort();
}

int main() {
    using OpenGL::WebGLCacheData;
    WebGLCacheData data;
    Check(data.Add("vertex-a", "fragment-a"));
    Check(data.Add("vertex-a", "fragment-b"));
    Check(!data.Add("vertex-a", "fragment-a"));
    Check(data.shaders.size() == 3 && data.programs.size() == 2);
    const auto bytes = data.Encode(123);
    Check(bytes.size() == data.bytes);
    auto restored = WebGLCacheData::Decode(bytes, 123);
    Check(restored && restored->Encode(123) == bytes);
    Check(!WebGLCacheData::Decode(bytes, 124));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        Check(!WebGLCacheData::Decode(std::string_view(bytes).substr(0, i), 123));
        auto corrupt = bytes;
        corrupt[i] ^= 0x80;
        Check(!WebGLCacheData::Decode(corrupt, 123));
    }
    Check(!WebGLCacheData::Decode(bytes + "extra", 123));
    Check(!data.Add("", "fragment"));
    Check(!data.Add(std::string("bad\0source", 10), "fragment"));
    Check(!data.Add(std::string(WebGLCacheData::MaxSourceBytes + 1, 'x'), "fragment"));
    WebGLCacheData full;
    for (std::size_t i = 0; i < WebGLCacheData::MaxPrograms; ++i) {
        Check(full.Add("vs", "fs-" + std::to_string(i)));
    }
    Check(!full.Add("vs", "overflow"));
    Check(WebGLCacheData::Decode(full.Encode(123), 123).has_value());
    WebGLCacheData large;
    for (u32 i = 0; i < 32; ++i) {
        large.Add("vs", std::string(WebGLCacheData::MaxSourceBytes - 10, 'x') + std::to_string(i));
    }
    Check(large.bytes <= WebGLCacheData::MaxBytes);
    Check(WebGLCacheData::Decode(large.Encode(123), 123).has_value());
    std::puts("PASS: WebGL cache round trip, reuse, corruption, version and size limits");
}
