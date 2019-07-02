#include "cube_map_shader/cube_map_shader.fragment.h"
#include "cube_map_shader/cube_map_shader.vertex.h"
#include "irradiance_shader/irradiance_shader.fragment.h"
#include "prefilter_shader/prefilter_shader.fragment.h"

#include "stb_image.h"

#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <bgfx/platform.h>
#include <chrono>
#include <clara.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iomanip>
#include <iostream>
#include <nvtt/nvtt.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

struct TextureCompilerErrorHandler final : nvtt::ErrorHandler {
    void error(nvtt::Error e) final;
};

void TextureCompilerErrorHandler::error(nvtt::Error e) {
    std::cout << "\rTexture compiler error. " << nvtt::errorString(e) << std::endl;
}

enum class Compression {
    GOOD_BUT_SLOW,
    POOR_BUT_FAST,
    NO_COMPRESSION
};

struct RgbaWrapper final {
    explicit RgbaWrapper(const std::string& path) noexcept
            : data(stbi_load(path.c_str(), &width, &height, &channels, 4)) {
    }

    RgbaWrapper(const RgbaWrapper&) = delete;
    RgbaWrapper(RgbaWrapper&&) = delete;
    RgbaWrapper& operator=(const RgbaWrapper&) = delete;
    RgbaWrapper& operator=(RgbaWrapper&&) = delete;

    ~RgbaWrapper() {
        if (data != nullptr) {
            stbi_image_free(data);
        }
    }

    int width;
    int height;
    int channels;
    stbi_uc* data;
};

static int compile_albedo_roughness(const std::string& input, const std::string& output, Compression compression) noexcept {
    RgbaWrapper data(input);
    if (data.data == nullptr) {
        std::cout << "Texture compiler error. Failed to load a texture." << std::endl;
        return 1;
    }

    if (data.width <= 0 || data.height <= 0 || data.width > 65535 || data.height > 65535 || (data.width & (data.width - 1)) != 0 || (data.height & (data.height - 1)) != 0) {
        std::cout << "Texture compiler error. Image size is not power of two." << std::endl;
        return 1;
    }

    // Convert RGBA to BGRA.
    for (int i = 0; i < data.width * data.height; i++) {
        std::swap(data.data[i * 4], data.data[i * 4 + 2]);
    }

    nvtt::Surface surface;
    if (!surface.setImage(nvtt::InputFormat_BGRA_8UB, data.width, data.height, 1, data.data)) {
        std::cout << "Texture compiler error. Failed to set an image." << std::endl;
        return 1;
    }

    surface.setWrapMode(nvtt::WrapMode_Repeat);
    surface.setAlphaMode(nvtt::AlphaMode_Transparency);
    surface.setNormalMap(false);

    TextureCompilerErrorHandler error_handler;

    nvtt::OutputOptions output_options;
    output_options.setFileName(output.c_str());
    output_options.setContainer(nvtt::Container_DDS10);
    output_options.setErrorHandler(&error_handler);

    nvtt::CompressionOptions compression_options;
    switch (compression) {
        case Compression::GOOD_BUT_SLOW:
            compression_options.setFormat(nvtt::Format_BC7);
            break;
        case Compression::POOR_BUT_FAST:
            compression_options.setFormat(nvtt::Format_BC3);
            break;
        case Compression::NO_COMPRESSION:
            compression_options.setFormat(nvtt::Format_RGBA);
            break;
    }

    nvtt::Compressor compressor;

    const auto before = std::chrono::system_clock::now();

    std::cout << "Progress: 0%" << std::flush;

    const int total_mip_levels = surface.countMipmaps();
    if (!compressor.outputHeader(surface, total_mip_levels, compression_options, output_options)) {
        // Error is printed via `error_handler`.
        return 1;
    }

    for (int mip_level = 0; mip_level < total_mip_levels; mip_level++) {
        if (!compressor.compress(surface, 0, mip_level, compression_options, output_options)) {
            // Error is printed via `error_handler`.
            return 1;
        }

        if (mip_level + 1 < total_mip_levels && !surface.buildNextMipmap(nvtt::MipmapFilter_Box)) {
            std::cout << "\rTexture compiler error. Failed to build a mip map." << std::endl;
            return 1;
        }

        std::cout << "\rProgress: " << static_cast<int>(static_cast<float>(mip_level + 1) * 100.f / total_mip_levels) << "%" << std::flush;
    }

    const auto after = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(after - before);
    std::cout << "\rCompression took " << std::setprecision(3) << milliseconds.count() / 1000.f << " seconds." << std::endl;

    return 0;
}

static int compile_normal_metalness_ambient_occlusion(const std::string& input, const std::string& output, Compression compression) noexcept {
    RgbaWrapper data(input);
    if (data.data == nullptr) {
        std::cout << "Texture compiler error. Failed to load a texture." << std::endl;
        return 1;
    }

    if (data.width <= 0 || data.height <= 0 || data.width > 65535 || data.height > 65535 || (data.width & (data.width - 1)) != 0 || (data.height & (data.height - 1)) != 0) {
        std::cout << "Texture compiler error. Image size is not power of two." << std::endl;
        return 1;
    }

    // Convert RGBA to BGRA.
    for (int i = 0; i < data.width * data.height; i++) {
        std::swap(data.data[i * 4], data.data[i * 4 + 2]);
    }

    nvtt::Surface surface;
    if (!surface.setImage(nvtt::InputFormat_BGRA_8UB, data.width, data.height, 1, data.data)) {
        std::cout << "Texture compiler error. Failed to set an image." << std::endl;
        return 1;
    }

    const float* red_normal_channel = surface.channel(0);
    const float* green_normal_channel = surface.channel(1);

    // Reconstruct blue normal map channel.
    std::vector<float> blue_normal_channel(static_cast<size_t>(data.width) * static_cast<size_t>(data.height));
    for (size_t i = 0; i < blue_normal_channel.size(); i++) {
        const float red = red_normal_channel[i] * 2.f - 1.f;
        const float green = green_normal_channel[i] * 2.f - 1.f;
        const float dot = red * red + green * green;
        if (dot < 1.f) {
            blue_normal_channel[i] = std::sqrt(1.f - dot) * 0.5f + 0.5f;
        } else {
            // Broken normal pixel.
            blue_normal_channel[i] = 0.5f;
        }
    }

    // Opaque normal map.
    std::vector<float> alpha_normal_channel(static_cast<size_t>(data.width) * static_cast<size_t>(data.height), 1.f);

    nvtt::Surface normal;
    if (!normal.setImage(nvtt::InputFormat_RGBA_32F, data.width, data.height, 1, red_normal_channel, green_normal_channel, blue_normal_channel.data(), alpha_normal_channel.data())) {
        std::cout << "Texture compiler error. Failed to set an image." << std::endl;
        return 1;
    }

    normal.setWrapMode(nvtt::WrapMode_Repeat);
    normal.setAlphaMode(nvtt::AlphaMode_Transparency);
    normal.setNormalMap(true);

    nvtt::Surface metalness_ambient_occlusion;
    if (!metalness_ambient_occlusion.setImage(data.width, data.height, 1)) {
        std::cout << "Texture compiler error. Failed to set an image." << std::endl;
        return 1;
    }

    metalness_ambient_occlusion.copyChannel(surface, 2);
    metalness_ambient_occlusion.copyChannel(surface, 3);

    metalness_ambient_occlusion.setWrapMode(nvtt::WrapMode_Repeat);
    metalness_ambient_occlusion.setAlphaMode(nvtt::AlphaMode_Transparency);
    metalness_ambient_occlusion.setNormalMap(false);

    TextureCompilerErrorHandler error_handler;

    nvtt::OutputOptions output_options;
    output_options.setFileName(output.c_str());
    output_options.setContainer(nvtt::Container_DDS10);
    output_options.setErrorHandler(&error_handler);

    nvtt::CompressionOptions compression_options;
    switch (compression) {
        case Compression::GOOD_BUT_SLOW:
            compression_options.setFormat(nvtt::Format_BC7);
            break;
        case Compression::POOR_BUT_FAST:
            compression_options.setFormat(nvtt::Format_BC3);
            break;
        case Compression::NO_COMPRESSION:
            compression_options.setFormat(nvtt::Format_RGBA);
            break;
    }

    nvtt::Compressor compressor;

    const auto before = std::chrono::system_clock::now();

    std::cout << "Progress: 0%" << std::flush;

    const int total_mip_levels = surface.countMipmaps();
    if (!compressor.outputHeader(surface, total_mip_levels, compression_options, output_options)) {
        // Error is printed via `error_handler`.
        return 1;
    }

    for (int mip_level = 0; mip_level < total_mip_levels; mip_level++) {
        surface.setImage(nvtt::InputFormat_RGBA_32F, normal.width(), normal.height(), 1, normal.channel(0), normal.channel(1), metalness_ambient_occlusion.channel(2), metalness_ambient_occlusion.channel(3));
        if (!compressor.compress(surface, 0, mip_level, compression_options, output_options)) {
            // Error is printed via `error_handler`.
            return 1;
        }

        if (mip_level + 1 < total_mip_levels) {
            if (!normal.buildNextMipmap(nvtt::MipmapFilter_Box)) {
                std::cout << "\rTexture compiler error. Failed to build a normal mip map." << std::endl;
                return 1;
            }

            normal.expandNormals();
            normal.normalizeNormalMap();
            normal.packNormals();

            if (!metalness_ambient_occlusion.buildNextMipmap(nvtt::MipmapFilter_Box)) {
                std::cout << "\rTexture compiler error. Failed to build a metalness ambient occlusion mip map." << std::endl;
                return 1;
            }
        }

        std::cout << "\rProgress: " << static_cast<int>(static_cast<float>(mip_level + 1) * 100.f / total_mip_levels) << "%" << std::flush;
    }

    const auto after = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(after - before);
    std::cout << "\rCompression took " << std::setprecision(3) << milliseconds.count() / 1000.f << " seconds." << std::endl;

    return 0;
}

static int compile_parallax(const std::string& input, const std::string& output, Compression compression) noexcept {
    RgbaWrapper data(input);
    if (data.data == nullptr) {
        std::cout << "Texture compiler error. Failed to load a texture." << std::endl;
        return 1;
    }

    if (data.width == 0 || data.height == 0 || data.width > 65535 || data.height > 65535 || (data.width & (data.width - 1)) != 0 || (data.height & (data.height - 1)) != 0) {
        std::cout << "Texture compiler error. Image size is not power of two." << std::endl;
        return 1;
    }

    nvtt::Surface surface;
    if (!surface.setImage(nvtt::InputFormat_BGRA_8UB, data.width, data.height, 1, data.data)) {
        std::cout << "Texture compiler error. Failed to set a texture." << std::endl;
        return 1;
    }

    surface.setWrapMode(nvtt::WrapMode_Repeat);
    surface.setAlphaMode(nvtt::AlphaMode_Transparency);
    surface.setNormalMap(false);

    TextureCompilerErrorHandler error_handler;

    nvtt::OutputOptions output_options;
    output_options.setFileName(output.c_str());
    output_options.setContainer(nvtt::Container_DDS10);
    output_options.setErrorHandler(&error_handler);

    nvtt::CompressionOptions compression_options;
    switch (compression) {
        case Compression::GOOD_BUT_SLOW:
        case Compression::POOR_BUT_FAST:
            // BC4 is fast and good enough for both production and development.
            compression_options.setFormat(nvtt::Format_BC4);
            break;
        case Compression::NO_COMPRESSION:
            compression_options.setFormat(nvtt::Format_RGBA);
            compression_options.setPixelFormat(8, 0xFF, 0x00, 0x00, 0x00);
            break;
    }

    nvtt::Compressor compressor;

    const auto before = std::chrono::system_clock::now();

    std::cout << "Progress: 0%" << std::flush;

    const int total_mip_levels = surface.countMipmaps();
    if (!compressor.outputHeader(surface, total_mip_levels, compression_options, output_options)) {
        // Error is printed via `error_handler`.
        return 1;
    }

    for (int mip_level = 0; mip_level < total_mip_levels; mip_level++) {
        if (!compressor.compress(surface, 0, mip_level, compression_options, output_options)) {
            // Error is printed via `error_handler`.
            return 1;
        }

        if (mip_level + 1 < total_mip_levels && !surface.buildNextMipmap(nvtt::MipmapFilter_Box)) {
            std::cout << "\rTexture compiler error. Failed to build a mip map." << std::endl;
        }

        std::cout << "\rProgress: " << static_cast<int>(static_cast<float>(mip_level + 1) * 100.f / total_mip_levels) << "%" << std::flush;
    }

    const auto after = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(after - before);
    std::cout << "\rCompression took " << std::setprecision(3) << milliseconds.count() / 1000.f << " seconds." << std::endl;

    return 0;
}

struct SdlWrapper final {
    SdlWrapper() noexcept
            : initialized(SDL_Init(SDL_INIT_VIDEO) == 0) {
    }

    SdlWrapper(const SdlWrapper&) = delete;
    SdlWrapper(SdlWrapper&&) = delete;
    SdlWrapper& operator=(const SdlWrapper&) = delete;
    SdlWrapper& operator=(SdlWrapper&&) = delete;

    ~SdlWrapper() {
        if (initialized) {
            SDL_Quit();
        }
    }

    bool initialized;
};

struct WindowWrapper final {
    WindowWrapper() noexcept
            : window(SDL_CreateWindow("Texture Compiler", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 256, 256, SDL_WINDOW_HIDDEN)) {
    }

    WindowWrapper(const WindowWrapper&) = delete;
    WindowWrapper(WindowWrapper&&) = delete;
    WindowWrapper& operator=(const WindowWrapper&) = delete;
    WindowWrapper& operator=(WindowWrapper&&) = delete;

    ~WindowWrapper() {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }

    SDL_Window* window;
};

struct BgfxWrapper final {
    BgfxWrapper() noexcept
            : initialized(bgfx::init()) {
    }

    BgfxWrapper(const BgfxWrapper&) = delete;
    BgfxWrapper(BgfxWrapper&&) = delete;
    BgfxWrapper& operator=(const BgfxWrapper&) = delete;
    BgfxWrapper& operator=(BgfxWrapper&&) = delete;

    ~BgfxWrapper() {
        if (initialized) {
            bgfx::shutdown();
        }
    }

    bool initialized;
};

struct HdrWrapper final {
    explicit HdrWrapper(const std::string& path) noexcept
            : data(stbi_loadf(path.c_str(), &width, &height, &channels, 4)) {
    }

    HdrWrapper(const HdrWrapper&) = delete;
    HdrWrapper(HdrWrapper&&) = delete;
    HdrWrapper& operator=(const HdrWrapper&) = delete;
    HdrWrapper& operator=(HdrWrapper&&) = delete;

    ~HdrWrapper() {
        if (data != nullptr) {
            stbi_image_free(data);
        }
    }

    int width;
    int height;
    int channels;
    float* data;
};

template <typename HandleType>
struct HandleWrapper final {
    HandleWrapper() noexcept
            : handle(BGFX_INVALID_HANDLE) {
    }

    HandleWrapper(HandleWrapper&& original) noexcept
            : handle(original.handle) {
        original.handle = BGFX_INVALID_HANDLE;
    }

    HandleWrapper(HandleType handle) noexcept
            : handle(handle) {
    }

    HandleWrapper(const HandleWrapper&) = delete;
    HandleWrapper& operator=(const HandleWrapper&) = delete;

    HandleWrapper& operator=(HandleWrapper&& original) noexcept {
        this->~HandleWrapper();
        handle = original.handle;
        original.handle = BGFX_INVALID_HANDLE;
        return *this;
    }

    HandleWrapper& operator=(HandleType handle_) noexcept {
        this->~HandleWrapper();
        handle = handle_;
        return *this;
    }

    ~HandleWrapper() {
        if (bgfx::isValid(handle)) {
            bgfx::destroy(handle);
        }
    }

    operator HandleType() const noexcept {
        return handle;
    }

    HandleType handle = BGFX_INVALID_HANDLE;
};

static float CUBE_VERTICES[] = {
        // Back face.
        -1.f, -1.f, -1.f, // Bottom-left.
         1.f,  1.f, -1.f, // Top-right.
         1.f, -1.f, -1.f, // Bottom-right.
         1.f,  1.f, -1.f, // Top-right.
        -1.f, -1.f, -1.f, // Bottom-left.
        -1.f,  1.f, -1.f, // Top-left.
        // Front face.
        -1.f, -1.f,  1.f, // Bottom-left.
         1.f, -1.f,  1.f, // Bottom-right.
         1.f,  1.f,  1.f, // Top-right.
         1.f,  1.f,  1.f, // Top-right.
        -1.f,  1.f,  1.f, // Top-left.
        -1.f, -1.f,  1.f, // Bottom-left.
        // Left face.
        -1.f,  1.f,  1.f, // Top-right.
        -1.f,  1.f, -1.f, // Top-left.
        -1.f, -1.f, -1.f, // Bottom-left.
        -1.f, -1.f, -1.f, // Bottom-left.
        -1.f, -1.f,  1.f, // Bottom-right.
        -1.f,  1.f,  1.f, // Top-right.
        // Right face.
         1.f,  1.f,  1.f, // Top-left.
         1.f, -1.f, -1.f, // Bottom-right.
         1.f,  1.f, -1.f, // Top-right.
         1.f, -1.f, -1.f, // Bottom-right.
         1.f,  1.f,  1.f, // Top-left.
         1.f, -1.f,  1.f, // Bottom-left.
        // Bottom face.
        -1.f, -1.f, -1.f, // Top-right.
         1.f, -1.f, -1.f, // Top-left.
         1.f, -1.f,  1.f, // Bottom-left.
         1.f, -1.f,  1.f, // Bottom-left.
        -1.f, -1.f,  1.f, // Bottom-right.
        -1.f, -1.f, -1.f, // Top-right.
        // Top face.
        -1.f,  1.f, -1.f, // Top-left.
         1.f,  1.f , 1.f, // Bottom-right.
         1.f,  1.f, -1.f, // Top-right.
         1.f,  1.f,  1.f, // Bottom-right.
        -1.f,  1.f, -1.f, // Top-left.
        -1.f,  1.f,  1.f  // Bottom-left.
};

static const bgfx::VertexDecl CUBE_VERTEX_DECLARATION = [] {
    bgfx::VertexDecl result;
    result.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    return result;
}();

static glm::mat4 CUBE_MAP_VIEWS[] = {
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 1.f,  0.f,  0.f), glm::vec3(0.f, -1.f,  0.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3(-1.f,  0.f,  0.f), glm::vec3(0.f, -1.f,  0.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 0.f, -1.f,  0.f), glm::vec3(0.f,  0.f, -1.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 0.f,  1.f,  0.f), glm::vec3(0.f,  0.f,  1.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 0.f,  0.f,  1.f), glm::vec3(0.f, -1.f,  0.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 0.f,  0.f, -1.f), glm::vec3(0.f, -1.f,  0.f))
};

static glm::mat4 CUBE_MAP_VIEWS_GLSL[] = {
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 1.f,  0.f,  0.f), glm::vec3(0.f, -1.f,  0.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3(-1.f,  0.f,  0.f), glm::vec3(0.f, -1.f,  0.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 0.f,  1.f,  0.f), glm::vec3(0.f,  0.f,  1.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 0.f, -1.f,  0.f), glm::vec3(0.f,  0.f, -1.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 0.f,  0.f,  1.f), glm::vec3(0.f, -1.f,  0.f)),
        glm::lookAt(glm::vec3(0.f, 0.f, 0.f), glm::vec3( 0.f,  0.f, -1.f), glm::vec3(0.f, -1.f,  0.f))
};

static glm::mat4 CUBE_MAP_PROJECTION = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

static const bgfx::EmbeddedShader CUBE_MAP_SHADER[] = {
        BGFX_EMBEDDED_SHADER(cube_map_shader_vertex),
        BGFX_EMBEDDED_SHADER(cube_map_shader_fragment),
        BGFX_EMBEDDED_SHADER_END()
};

static const bgfx::EmbeddedShader IRRADIANCE_SHADER[] = {
        BGFX_EMBEDDED_SHADER(cube_map_shader_vertex),
        BGFX_EMBEDDED_SHADER(irradiance_shader_fragment),
        BGFX_EMBEDDED_SHADER_END()
};

static const bgfx::EmbeddedShader PREFILTER_SHADER[] = {
        BGFX_EMBEDDED_SHADER(cube_map_shader_vertex),
        BGFX_EMBEDDED_SHADER(prefilter_shader_fragment),
        BGFX_EMBEDDED_SHADER_END()
};

static int count_mip_maps(size_t size) noexcept {
    int result = 1;
    while (size > 1) {
        size /= 2;
        result++;
    }
    return result;
}

static int compile_cube_map(const std::string& input, const std::string& output, size_t output_size,
                            const std::string& output_irradiance, size_t irradiance_size,
                            const std::string& output_prefilter, size_t prefilter_size,
                            Compression compression) noexcept {
    SdlWrapper sdl;
    if (!sdl.initialized) {
        std::cout << "Texture compiler error. Failed to initialize a video subsystem." << std::endl;
        return 1;
    }

    WindowWrapper window;
    if (window.window == nullptr) {
        std::cout << "Texture compiler error. Failed to initialize a window." << std::endl;
        return 1;
    }

    SDL_SysWMinfo native_info;
    SDL_VERSION(&native_info.version)
    if (!SDL_GetWindowWMInfo(window.window, &native_info)) {
        std::cout << "Texture compiler error. Failed to get system window handle." << std::endl;
        return 1;
    }

    bgfx::PlatformData platform_data {};
#if BX_PLATFORM_WINDOWS
    platform_data.nwh = native_info.info.win.window;
#elif BX_PLATFORM_OSX
    platform_data.ndt = nullptr;
    platform_data.nwh = native_info.info.cocoa.window;
#elif BX_PLATFORM_LINUX
    platform_data.ndt = native_info.info.x11.display;
    platform_data.nwh = reinterpret_cast<void*>(native_info.info.x11.window);
#endif
    bgfx::setPlatformData(platform_data);

    BgfxWrapper bgfx;
    if (!bgfx.initialized) {
        std::cout << "Texture compiler error. Failed to initialize a renderer." << std::endl;
        return 1;
    }

    bgfx::reset(256, 256, BGFX_RESET_NONE);

    stbi_set_flip_vertically_on_load(1);

    HdrWrapper data(input);
    if (data.data == nullptr) {
        std::cout << "Texture compiler error. Failed to open texture file." << std::endl;
        return 1;
    }

    if (data.width <= 0 || data.height <= 0 || data.width > 65535 || data.height > 65535) {
        std::cout << "Texture compiler error. Texture is too big." << std::endl;
        return 1;
    }

    HandleWrapper<bgfx::TextureHandle> texture = bgfx::createTexture2D(data.width, data.height, false, 1, bgfx::TextureFormat::RGBA32F, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, bgfx::makeRef(data.data, data.width * data.height * 4 * 4));
    if (!bgfx::isValid(texture)) {
        std::cout << "Texture compiler error. Failed to create HDR texture." << std::endl;
        return 1;
    }
    bgfx::setName(texture, "original_texture");

    HandleWrapper<bgfx::VertexBufferHandle> vertex_buffer = bgfx::createVertexBuffer(bgfx::makeRef(CUBE_VERTICES, sizeof(CUBE_VERTICES)), CUBE_VERTEX_DECLARATION);
    if (!bgfx::isValid(vertex_buffer)) {
        std::cout << "Texture compiler error. Failed to create cube vertex buffer." << std::endl;
        return 1;
    }
    bgfx::setName(vertex_buffer, "cube_vertices");

    glm::mat4* cube_map_view_matrices;

    bgfx::RendererType::Enum renderer_type = bgfx::getRendererType();
    if (renderer_type == bgfx::RendererType::OpenGL || renderer_type == bgfx::RendererType::OpenGLES) {
        cube_map_view_matrices = CUBE_MAP_VIEWS_GLSL;
    } else {
        cube_map_view_matrices = CUBE_MAP_VIEWS;
    }

    HandleWrapper<bgfx::UniformHandle> texture_uniform = bgfx::createUniform("s_texture", bgfx::UniformType::Sampler);
    if (!bgfx::isValid(texture_uniform)) {
        std::cout << "Texture compiler error. Failed to create texture uniform." << std::endl;
        return 1;
    }

    HandleWrapper<bgfx::TextureHandle> cube_side_textures[6];
    for (size_t side = 0; side < std::size(cube_side_textures); side++) {
        const std::string cube_map_view_name = "cube_side_texture_" + std::to_string(side);

        cube_side_textures[side] = bgfx::createTexture2D(static_cast<uint16_t>(output_size), static_cast<uint16_t>(output_size), false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(cube_side_textures[side])) {
            std::cout << "Texture compiler error. Failed to create cube map side texture." << std::endl;
            return 1;
        }
        bgfx::setName(cube_side_textures[side], cube_map_view_name.c_str());
    }

    bgfx::ShaderHandle vertex_shader_handle = bgfx::createEmbeddedShader(CUBE_MAP_SHADER, renderer_type, "cube_map_shader_vertex");
    if (!bgfx::isValid(vertex_shader_handle)) {
        std::cout << "Texture compiler error. Failed to create cube map vertex shader." << std::endl;
        return 1;
    }
    bgfx::setName(vertex_shader_handle, "cube_map_shader_vertex");

    bgfx::ShaderHandle fragment_shader_handle = bgfx::createEmbeddedShader(CUBE_MAP_SHADER, renderer_type, "cube_map_shader_fragment");
    if (!bgfx::isValid(fragment_shader_handle)) {
        std::cout << "Texture compiler error. Failed to create cube map fragment shader." << std::endl;
        bgfx::destroy(vertex_shader_handle);
        return 1;
    }
    bgfx::setName(fragment_shader_handle, "cube_map_shader_fragment");

    HandleWrapper<bgfx::ProgramHandle> cube_map_program_handle = bgfx::createProgram(vertex_shader_handle, fragment_shader_handle, true);
    if (!bgfx::isValid(cube_map_program_handle)) {
        std::cout << "Texture compiler error. Failed to create cube map program." << std::endl;
        bgfx::destroy(fragment_shader_handle);
        bgfx::destroy(vertex_shader_handle);
        return 1;
    }

    bgfx::ViewId current_view = 0;
    std::vector<HandleWrapper<bgfx::FrameBufferHandle>> cube_map_frame_buffers;

    // Cube map sides to read back from.
    for (size_t side = 0; side < 6; side++, current_view++) {
        const std::string cube_side_view_name = "cube_side_view_" + std::to_string(side);

        bgfx::setViewClear(current_view, BGFX_CLEAR_COLOR);
        bgfx::setViewName(current_view, cube_side_view_name.c_str());

        bgfx::FrameBufferHandle frame_buffer = bgfx::createFrameBuffer(1, &cube_side_textures[side].handle, false);
        if (!bgfx::isValid(frame_buffer)) {
            std::cout << "Texture compiler error. Failed to create cube map side frame buffer." << std::endl;
            return 1;
        }
        cube_map_frame_buffers.emplace_back(frame_buffer);

        bgfx::setViewFrameBuffer(current_view, frame_buffer);
        bgfx::setViewRect(current_view, 0, 0, static_cast<uint16_t>(output_size), static_cast<uint16_t>(output_size));
        bgfx::setViewTransform(current_view, glm::value_ptr(cube_map_view_matrices[side]), glm::value_ptr(CUBE_MAP_PROJECTION));

        bgfx::setVertexBuffer(0, vertex_buffer, 0, static_cast<uint32_t>(std::size(CUBE_VERTICES) / 3));
        bgfx::setTexture(0, texture_uniform, texture);

        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_CULL_CCW);
        bgfx::submit(current_view, cube_map_program_handle);
    }

    TextureCompilerErrorHandler error_handler;

    nvtt::OutputOptions cube_map_output_options;
    cube_map_output_options.setFileName(output.c_str());
    cube_map_output_options.setContainer(nvtt::Container_DDS10);
    cube_map_output_options.setErrorHandler(&error_handler);

    nvtt::CompressionOptions cube_map_compression_options;
    switch (compression) {
        case Compression::GOOD_BUT_SLOW:
            cube_map_compression_options.setFormat(nvtt::Format_BC6);
            break;
        case Compression::POOR_BUT_FAST:
            // BC6 is quite slow on big HDR textures.
            cube_map_compression_options.setFormat(nvtt::Format_BC3);
            break;
        case Compression::NO_COMPRESSION:
            // R16G16B16A16.
            cube_map_compression_options.setFormat(nvtt::Format_RGB);
            cube_map_compression_options.setPixelFormat(16, 16, 16, 16);
            cube_map_compression_options.setPixelType(nvtt::PixelType_Float);
            break;
    }

    nvtt::Compressor cube_map_compressor;

    auto before = std::chrono::system_clock::now();

    std::cout << "Progress: 0%" << std::flush;

    int total_mip_levels = count_mip_maps(output_size);
    if (!cube_map_compressor.outputHeader(nvtt::TextureType_Cube, static_cast<int>(output_size), static_cast<int>(output_size), 1, 1, total_mip_levels, false, cube_map_compression_options, cube_map_output_options)) {
        // Error is printed via `error_handler`.
        return 1;
    }

    std::vector<uint16_t> data_to_save(output_size * output_size * 4);

    for (int side = 0; side < 6; side++, current_view++) {
        const std::string cube_side_read_back_view_name = "cube_side_read_back_view_" + std::to_string(side);
        bgfx::setViewName(current_view, cube_side_read_back_view_name.c_str());

        // `BGFX_TEXTURE_RT` and `BGFX_TEXTURE_READ_BACK` are not compatible, so blit the render target texture to another texture and read back from it.
        bgfx::TextureHandle blit_texture = bgfx::createTexture2D(static_cast<uint16_t>(output_size), static_cast<uint16_t>(output_size), false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(blit_texture)) {
            std::cout << "\rTexture compiler error. Failed to create a read back texture." << std::endl;
            return 1;
        }
        bgfx::blit(current_view, blit_texture, 0, 0, cube_side_textures[side], 0, 0, static_cast<uint16_t>(output_size), static_cast<uint16_t>(output_size));

        const uint32_t frame_id = bgfx::readTexture(blit_texture, data_to_save.data());

        // Wait until read back is available.
        uint32_t current_frame_id;
        do {
            current_frame_id = bgfx::frame();
        } while (current_frame_id < frame_id);

        bgfx::destroy(blit_texture);

        nvtt::Surface surface;
        if (!surface.setImage(nvtt::InputFormat_RGBA_16F, static_cast<int>(output_size), static_cast<int>(output_size), 1, data_to_save.data())) {
            std::cout << "\rTexture compiler error. Failed to set an image." << std::endl;
            return 1;
        }

        for (int mip_level = 0; mip_level < total_mip_levels; mip_level++) {
            if (!cube_map_compressor.compress(surface, side, mip_level, cube_map_compression_options, cube_map_output_options)) {
                // Error is printed via `error_handler`.
                return 1;
            }

            if (mip_level + 1 < total_mip_levels) {
                if (!surface.buildNextMipmap(nvtt::MipmapFilter_Box)) {
                    std::cout << "\rTexture compiler error. Failed to build a cube map mip map." << std::endl;
                    return 1;
                }
            }

            std::cout << "\rProgress: " << static_cast<int>(static_cast<float>(side * total_mip_levels + mip_level + 1) * 100.f / total_mip_levels / 6) << "%" << std::flush;
        }
    }

    auto after = std::chrono::system_clock::now();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(after - before);
    std::cout << "\rCube map compression took " << std::setprecision(3) << milliseconds.count() / 1000.f << " seconds." << std::endl;

    cube_map_frame_buffers.clear();

    HandleWrapper<bgfx::TextureHandle> cube_map_texture = bgfx::createTextureCube(static_cast<uint16_t>(output_size), true, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(cube_map_texture)) {
        std::cout << "Texture compiler error. Failed to create cube map texture." << std::endl;
        return 1;
    }

    // Cube map to use in other shaders.
    for (size_t side = 0; side < 6; side++) {
        for (size_t size = output_size, mip_level = 0; size >= 1; size /= 2, mip_level++, current_view++) {
            const std::string cube_map_view_name = "cube_map_view_" + std::to_string(side) + "_" + std::to_string(mip_level);

            bgfx::setViewClear(current_view, BGFX_CLEAR_COLOR);
            bgfx::setViewName(current_view, cube_map_view_name.c_str());

            bgfx::Attachment attachment;
            attachment.init(cube_map_texture, bgfx::Access::Write, static_cast<uint16_t>(side), static_cast<uint16_t>(mip_level));

            bgfx::FrameBufferHandle frame_buffer = bgfx::createFrameBuffer(1, &attachment, false);
            if (!bgfx::isValid(frame_buffer)) {
                std::cout << "Texture compiler error. Failed to create cube map frame buffer." << std::endl;
                return 1;
            }
            cube_map_frame_buffers.emplace_back(frame_buffer);

            bgfx::setViewFrameBuffer(current_view, frame_buffer);
            bgfx::setViewRect(current_view, 0, 0, static_cast<uint16_t>(size), static_cast<uint16_t>(size));
            bgfx::setViewTransform(current_view, glm::value_ptr(cube_map_view_matrices[side]), glm::value_ptr(CUBE_MAP_PROJECTION));

            bgfx::setVertexBuffer(0, vertex_buffer, 0, static_cast<uint32_t>(std::size(CUBE_VERTICES) / 3));
            bgfx::setTexture(0, texture_uniform, texture);

            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_CULL_CCW);
            bgfx::submit(current_view, cube_map_program_handle);
        }
    }

    HandleWrapper<bgfx::TextureHandle> irradiance_textures[6];
    for (size_t side = 0; side < 6; side++) {
        const std::string irradiance_texture_name = "irradiance_texture_" + std::to_string(side);

        irradiance_textures[side] = bgfx::createTexture2D(static_cast<uint16_t>(irradiance_size), static_cast<uint16_t>(irradiance_size), false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(irradiance_textures[side])) {
            std::cout << "Texture compiler error. Failed to create irradiance texture." << std::endl;
            return 1;
        }
        bgfx::setName(irradiance_textures[side], irradiance_texture_name.c_str());
    }

    vertex_shader_handle = bgfx::createEmbeddedShader(IRRADIANCE_SHADER, renderer_type, "cube_map_shader_vertex");
    if (!bgfx::isValid(vertex_shader_handle)) {
        std::cout << "Texture compiler error. Failed to create irradiance map vertex shader." << std::endl;
        return 1;
    }
    bgfx::setName(vertex_shader_handle, "irradiance_shader_vertex");

    fragment_shader_handle = bgfx::createEmbeddedShader(IRRADIANCE_SHADER, renderer_type, "irradiance_shader_fragment");
    if (!bgfx::isValid(fragment_shader_handle)) {
        std::cout << "Texture compiler error. Failed to create irradiance map fragment shader." << std::endl;
        bgfx::destroy(vertex_shader_handle);
        return 1;
    }
    bgfx::setName(fragment_shader_handle, "irradiance_shader_fragment");

    HandleWrapper<bgfx::ProgramHandle> irradiance_program_handle = bgfx::createProgram(vertex_shader_handle, fragment_shader_handle, true);
    if (!bgfx::isValid(irradiance_program_handle)) {
        std::cout << "Texture compiler error. Failed to create irradiance map program." << std::endl;
        bgfx::destroy(fragment_shader_handle);
        bgfx::destroy(vertex_shader_handle);
        return 1;
    }

    std::vector<HandleWrapper<bgfx::FrameBufferHandle>> irradiance_frame_buffers;

    for (size_t side = 0; side < 6; side++, current_view++) {
        const std::string irradiance_view_name = "irradiance_view_" + std::to_string(side);

        bgfx::setViewClear(current_view, BGFX_CLEAR_COLOR);
        bgfx::setViewName(current_view, irradiance_view_name.c_str());

        bgfx::FrameBufferHandle frame_buffer = bgfx::createFrameBuffer(1, &irradiance_textures[side].handle, false);
        if (!bgfx::isValid(frame_buffer)) {
            std::cout << "Texture compiler error. Failed to create irradiance map frame buffer." << std::endl;
            return 1;
        }
        irradiance_frame_buffers.emplace_back(frame_buffer);

        bgfx::setViewFrameBuffer(current_view, frame_buffer);
        bgfx::setViewRect(current_view, 0, 0, static_cast<uint16_t>(irradiance_size), static_cast<uint16_t>(irradiance_size));
        bgfx::setViewTransform(current_view, glm::value_ptr(cube_map_view_matrices[side]), glm::value_ptr(CUBE_MAP_PROJECTION));

        bgfx::setVertexBuffer(0, vertex_buffer, 0, static_cast<uint32_t>(std::size(CUBE_VERTICES) / 3));
        bgfx::setTexture(0, texture_uniform, cube_map_texture);

        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_CULL_CCW);
        bgfx::submit(current_view, irradiance_program_handle);
    }

    nvtt::OutputOptions irradiance_output_options;
    irradiance_output_options.setFileName(output_irradiance.c_str());
    irradiance_output_options.setContainer(nvtt::Container_DDS10);
    irradiance_output_options.setErrorHandler(&error_handler);

    // Don't apply compression to tiny irradiance texture. Use R16G16B16A16 instead.
    nvtt::CompressionOptions irradiance_compression_options;
    irradiance_compression_options.setFormat(nvtt::Format_RGB);
    irradiance_compression_options.setPixelFormat(16, 16, 16, 16);
    irradiance_compression_options.setPixelType(nvtt::PixelType_Float);

    nvtt::Compressor irradiance_compressor;

    before = std::chrono::system_clock::now();

    std::cout << "Progress: 0%" << std::flush;

    if (!irradiance_compressor.outputHeader(nvtt::TextureType_Cube, static_cast<int>(irradiance_size), static_cast<int>(irradiance_size), 1, 1, 1, false, irradiance_compression_options, irradiance_output_options)) {
        // Error is printed via `error_handler`.
        return 1;
    }

    data_to_save.resize(irradiance_size * irradiance_size * 4);

    for (int side = 0; side < 6; side++, current_view++) {
        const std::string irradiance_read_back_view_name = "irradiance_read_back_view_" + std::to_string(side);
        bgfx::setViewName(current_view, irradiance_read_back_view_name.c_str());

        // `BGFX_TEXTURE_RT` and `BGFX_TEXTURE_READ_BACK` are not compatible, so blit the render target texture to another texture and read back from it.
        bgfx::TextureHandle blit_texture = bgfx::createTexture2D(static_cast<uint16_t>(irradiance_size), static_cast<uint16_t>(irradiance_size), false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(blit_texture)) {
            std::cout << "\rTexture compiler error. Failed to create an irradiance read back texture." << std::endl;
            return 1;
        }
        bgfx::blit(current_view, blit_texture, 0, 0, irradiance_textures[side], 0, 0, static_cast<uint16_t>(irradiance_size), static_cast<uint16_t>(irradiance_size));

        const uint32_t frame_id = bgfx::readTexture(blit_texture, data_to_save.data());

        // Wait until read back is available.
        uint32_t current_frame_id;
        do {
            current_frame_id = bgfx::frame();
        } while (current_frame_id < frame_id);

        bgfx::destroy(blit_texture);

        nvtt::Surface surface;
        if (!surface.setImage(nvtt::InputFormat_RGBA_16F, static_cast<int>(irradiance_size), static_cast<int>(irradiance_size), 1, data_to_save.data())) {
            std::cout << "\rTexture compiler error. Failed to create an irradiance read back surface." << std::endl;
            return 1;
        }

        if (!irradiance_compressor.compress(surface, side, 0, irradiance_compression_options, irradiance_output_options)) {
            // Error is printed via `error_handler`.
            return 1;
        }

        std::cout << "\rProgress: " << static_cast<int>((side + 1) * 100.f / 6) << "%" << std::flush;
    }

    after = std::chrono::system_clock::now();
    milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(after - before);
    std::cout << "\rIrradiance map compression took " << std::setprecision(3) << milliseconds.count() / 1000.f << " seconds." << std::endl;

    irradiance_frame_buffers.clear();

    std::vector<HandleWrapper<bgfx::TextureHandle>> prefilter_textures[6];
    for (size_t side = 0; side < 6; side++) {
        for (uint16_t mip_size = static_cast<uint16_t>(prefilter_size), mip_level = 0; mip_size >= 1; mip_size /= 2, mip_level++) {
            const std::string prefilter_texture_name = "prefilter_texture_" + std::to_string(side) + "_" + std::to_string(mip_level);

            bgfx::TextureHandle mip_texture = bgfx::createTexture2D(mip_size, mip_size, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            if (!bgfx::isValid(mip_texture)) {
                std::cout << "Texture compiler error. Failed to create prefilter side texture." << std::endl;
                return 1;
            }
            bgfx::setName(mip_texture, prefilter_texture_name.c_str());

            prefilter_textures[side].emplace_back(mip_texture);
        }
    }

    vertex_shader_handle = bgfx::createEmbeddedShader(PREFILTER_SHADER, renderer_type, "cube_map_shader_vertex");
    if (!bgfx::isValid(vertex_shader_handle)) {
        std::cout << "Texture compiler error. Failed to create prefilter vertex shader." << std::endl;
        return 1;
    }
    bgfx::setName(vertex_shader_handle, "prefilter_map_shader_vertex");

    fragment_shader_handle = bgfx::createEmbeddedShader(PREFILTER_SHADER, renderer_type, "prefilter_shader_fragment");
    if (!bgfx::isValid(fragment_shader_handle)) {
        std::cout << "Texture compiler error. Failed to create prefilter fragment shader." << std::endl;
        bgfx::destroy(vertex_shader_handle);
        return 1;
    }
    bgfx::setName(fragment_shader_handle, "prefilter_shader_fragment");

    HandleWrapper<bgfx::ProgramHandle> prefilter_program_handle = bgfx::createProgram(vertex_shader_handle, fragment_shader_handle, true);
    if (!bgfx::isValid(prefilter_program_handle)) {
        std::cout << "Texture compiler error. Failed to create prefilter program." << std::endl;
        bgfx::destroy(fragment_shader_handle);
        bgfx::destroy(vertex_shader_handle);
        return 1;
    }

    HandleWrapper<bgfx::UniformHandle> settings_uniform = bgfx::createUniform("u_settings", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(settings_uniform)) {
        std::cout << "Texture compiler error. Failed to create a settings uniform." << std::endl;
        return 1;
    }

    std::vector<HandleWrapper<bgfx::FrameBufferHandle>> prefilter_frame_buffers;

    for (size_t side = 0; side < 6; side++) {
        for (uint16_t mip_size = static_cast<uint16_t>(prefilter_size), mip_level = 0; mip_size >= 1; mip_size /= 2, mip_level++, current_view++) {
            const std::string prefilter_view_name = "prefilter_view_" + std::to_string(side) + "_" + std::to_string(mip_level);

            bgfx::setViewClear(current_view, BGFX_CLEAR_COLOR);
            bgfx::setViewName(current_view, prefilter_view_name.c_str());

            bgfx::FrameBufferHandle frame_buffer = bgfx::createFrameBuffer(1, &prefilter_textures[side][mip_level].handle, false);
            if (!bgfx::isValid(frame_buffer)) {
                std::cout << "Texture compiler error. Failed to create prefilter frame buffer." << std::endl;
                return 1;
            }
            prefilter_frame_buffers.emplace_back(frame_buffer);

            bgfx::setViewFrameBuffer(current_view, frame_buffer);
            bgfx::setViewRect(current_view, 0, 0, mip_size, mip_size);
            bgfx::setViewTransform(current_view, glm::value_ptr(cube_map_view_matrices[side]), glm::value_ptr(CUBE_MAP_PROJECTION));

            bgfx::setVertexBuffer(0, vertex_buffer, 0, static_cast<uint32_t>(std::size(CUBE_VERTICES) / 3));
            bgfx::setTexture(0, texture_uniform, cube_map_texture);

            constexpr uint16_t MAX_MIP_LEVELS = 4;
            const float settings[4] = { static_cast<float>(std::min(mip_level, MAX_MIP_LEVELS)) / MAX_MIP_LEVELS, static_cast<float>(output_size), 0.f, 0.f };
            bgfx::setUniform(settings_uniform, settings);

            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_CULL_CCW);
            bgfx::submit(current_view, prefilter_program_handle);
        }
    }

    nvtt::OutputOptions prefilter_output_options;
    prefilter_output_options.setFileName(output_prefilter.c_str());
    prefilter_output_options.setContainer(nvtt::Container_DDS10);
    prefilter_output_options.setErrorHandler(&error_handler);

    nvtt::CompressionOptions prefilter_compression_options;
    switch (compression) {
        case Compression::GOOD_BUT_SLOW:
        case Compression::POOR_BUT_FAST:
            // BC6 is quite fast for small prefilter texture.
            prefilter_compression_options.setFormat(nvtt::Format_BC6);
            break;
        case Compression::NO_COMPRESSION:
            // R16G16B16A16.
            prefilter_compression_options.setFormat(nvtt::Format_RGBA);
            prefilter_compression_options.setPixelFormat(16, 16, 16, 16);
            prefilter_compression_options.setPixelType(nvtt::PixelType_Float);
            break;
    }

    nvtt::Compressor prefilter_compressor;

    before = std::chrono::system_clock::now();

    std::cout << "Progress: 0%" << std::flush;

    total_mip_levels = count_mip_maps(prefilter_size);
    if (!prefilter_compressor.outputHeader(nvtt::TextureType_Cube, static_cast<int>(prefilter_size), static_cast<int>(prefilter_size), 1, 1, total_mip_levels, false, prefilter_compression_options, prefilter_output_options)) {
        // Error is printed via `error_handler`.
        return 1;
    }

    data_to_save.resize(prefilter_size * prefilter_size * 4);

    for (int side = 0; side < 6; side++) {
        for (uint16_t mip_size = static_cast<uint16_t>(prefilter_size), mip_level = 0; mip_size >= 1; mip_size /= 2, mip_level++, current_view++) {
            const std::string prefilter_read_back_view_name = "prefilter_read_back_view_" + std::to_string(side);
            bgfx::setViewName(current_view, prefilter_read_back_view_name.c_str());

            // `BGFX_TEXTURE_RT` and `BGFX_TEXTURE_READ_BACK` are not compatible, so blit the render target texture to another texture and read back from it.
            bgfx::TextureHandle blit_texture = bgfx::createTexture2D(mip_size, mip_size, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            if (!bgfx::isValid(blit_texture)) {
                std::cout << "\rTexture compiler error. Failed to create a prefilter read back texture." << std::endl;
                return 1;
            }
            bgfx::blit(current_view, blit_texture, 0, 0, prefilter_textures[side][mip_level], 0, 0, mip_size, mip_size);

            const uint32_t frame_id = bgfx::readTexture(blit_texture, data_to_save.data());

            // Wait until read back is available.
            uint32_t current_frame_id;
            do {
                current_frame_id = bgfx::frame();
            } while (current_frame_id < frame_id);

            bgfx::destroy(blit_texture);

            nvtt::Surface surface;
            if (!surface.setImage(nvtt::InputFormat_RGBA_16F, static_cast<int>(mip_size), static_cast<int>(mip_size), 1, data_to_save.data())) {
                std::cout << "\rTexture compiler error. Failed to create a prefilter read back surface." << std::endl;
                return 1;
            }

            if (!prefilter_compressor.compress(surface, side, mip_level, prefilter_compression_options, prefilter_output_options)) {
                // Error is printed via `error_handler`.
                return 1;
            }

            std::cout << "\rProgress: " << static_cast<int>((side * total_mip_levels + mip_level + 1) * 100.f / 6 / total_mip_levels) << "%" << std::flush;
        }
    }

    after = std::chrono::system_clock::now();
    milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(after - before);
    std::cout << "\rPrefilter map compression took " << std::setprecision(3) << milliseconds.count() / 1000.f << " seconds." << std::endl;

    return 0;
}

int main(int argc, char* argv[]) {
    bool is_albedo_roughness = false;                   // PNG
    bool is_normal_metalness_ambient_occlusion = false; // PNG
    bool is_parallax = false;                           // PNG
    bool is_cube_map = false;                           // HDR

    bool is_production = false;
    bool is_development = false;
    bool is_no_compression = false;

    std::string input;
    std::string output;
    size_t output_size = 0;            // Cube map only
    std::string output_irradiance;     // Cube map only
    size_t output_irradiance_size = 0; // Cube map only
    std::string output_prefilter;      // Cube map only
    size_t output_prefilter_size = 0;  // Cube map only

    bool is_help = false;

    auto cli = clara::Opt(is_albedo_roughness)["--albedo-roughness"]("Input contains albedo map and roughness map") |
            clara::Opt(is_normal_metalness_ambient_occlusion)["--normal-metalness-ambient-occlusion"]("Input contains normal, metalness and ambient occlusion maps") |
            clara::Opt(is_parallax)["--parallax"]("Input contains parallax map") |
            clara::Opt(is_cube_map)["--cube-map"]("Input contains cube map") |
            clara::Opt(input, "example.png")["--input"]("Input texture path") |
            clara::Opt(output, "example.texture")["--output"]("Output texture path") |
            clara::Opt(output_size, "1024")["--output-size"]("Output texture size (needed only for cube map, for other textures output texture size is equal to input texture size)") |
            clara::Opt(output_irradiance, "irradiance.texture")["--irradiance"]("Output irradiance texture path (needed only for cube map)") |
            clara::Opt(output_irradiance_size, "32")["--irradiance-size"]("Output irradiance texture size (needed only for cube map)") |
            clara::Opt(output_prefilter, "prefilter.texture")["--prefilter"]("Output prefilter texture path (needed only for cube map)") |
            clara::Opt(output_prefilter_size, "128")["--prefilter-size"]("Output prefilter texture size (needed only for cube map)") |
            clara::Opt(is_production)["--production"]("Good but slow texture compression") |
            clara::Opt(is_development)["--development"]("Poor but quick texture compression") |
            clara::Opt(is_no_compression)["--no-compression"]("No texture compression") |
            clara::Help(is_help);

    if (auto result = cli.parse(clara::Args(argc, argv)); !result) {
        std::cout << "Texture compiler error. Failed to parse command line arguments: " << result.errorMessage() << std::endl;
        return 1;
    }

    if (is_help) {
        std::cout << cli << std::endl;
        return 1;
    }

    if (input.empty()) {
        std::cout << "Texture compiler error. Input file is not specified." << std::endl;
        return 1;
    }

    if (output.empty()) {
        std::cout << "Texture compiler error. Output file is not specified." << std::endl;
        return 1;
    }

    if (static_cast<int32_t>(is_albedo_roughness) + static_cast<int32_t>(is_normal_metalness_ambient_occlusion) + static_cast<int32_t>(is_parallax) + static_cast<int32_t>(is_cube_map) != 1) {
        std::cout << "Texture compiler error. Invalid number of flags, one is required." << std::endl;
        return 1;
    }

    if (static_cast<int32_t>(is_production) + static_cast<int32_t>(is_development) + static_cast<int32_t>(is_no_compression) != 1) {
        std::cout << "Texture compiler error. Either --development, --production or --no-compression command line argument must be set." << std::endl;
        return 1;
    }

    Compression compression;
    if (is_production) {
        compression = Compression::GOOD_BUT_SLOW;
    } else if (is_development) {
        compression = Compression::POOR_BUT_FAST;
    } else {
        compression = Compression::NO_COMPRESSION;
    }

    if (is_cube_map) {
        if (output_size == 0 || output_irradiance.empty() || output_irradiance_size == 0 || output_prefilter.empty() || output_prefilter_size == 0) {
            std::cout << "Texture compiler error. Cube map requires --output-size, --irradiance, --irradiance-size, --prefilter, --prefilter-size command line arguments to be set." << std::endl;
            return 1;
        }

        if (output_size > 65535 || output_irradiance_size > 65535 || output_prefilter_size > 65535) {
            std::cout << "Texture compiler error. Invalid output size." << std::endl;
            return 1;
        }
    } else {
        if (output_size != 0 || !output_irradiance.empty() || output_irradiance_size != 0 || !output_prefilter.empty() || output_prefilter_size != 0) {
            std::cout << "Texture compiler error. Command line arguments --output-size, --irradiance, --irradiance-size, --prefilter, --prefilter-size are required only for cube map textures." << std::endl;
            return 1;
        }
    }

    if (is_albedo_roughness) {
        return compile_albedo_roughness(input, output, compression);
    }

    if (is_normal_metalness_ambient_occlusion) {
        return compile_normal_metalness_ambient_occlusion(input, output, compression);
    }

    if (is_parallax) {
        return compile_parallax(input, output, compression);
    }

    if (is_cube_map) {
        return compile_cube_map(input, output, output_size, output_irradiance, output_irradiance_size, output_prefilter, output_prefilter_size, compression);
    }

    // Should never happen.
    return 1;
}
