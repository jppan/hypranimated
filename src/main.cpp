#include <hyprgraphics/color/Color.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Transformer.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprlang.hpp>

#include <GLES3/gl32.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

inline HANDLE         PHANDLE                        = nullptr;
inline CFunctionHook* g_pRenderSnapshotHook          = nullptr;
inline CFunctionHook* g_pStartWindowAnimationHook    = nullptr;
inline CFunctionHook* g_pStartWorkspaceAnimationHook = nullptr;
inline bool           g_unloading                    = false;

constexpr auto CONFIG_NS       = "plugin:hypranimated:";
constexpr auto DEFAULT_SHADERS = "/home/jppan/.local/bin/jpOSh/shaders";

enum class EAnimationKind : uint8_t {
    OPEN,
    CLOSE,
};

struct SConfig {
    Hyprlang::INT*          enabled         = nullptr;
    Hyprlang::STRING const* effect          = nullptr;
    Hyprlang::STRING const* shadersDir      = nullptr;
    Hyprlang::INT*          durationMs      = nullptr;
    Hyprlang::INT*          workspaceSwitch = nullptr;
    Hyprlang::INT*          syncHyprland    = nullptr;
};

struct SEffectConfig {
    int         durationMs = 350;
    std::string curve      = "ease-out-cubic";
};

struct SShaderKey {
    std::string effect;
    std::string kind;

    bool operator==(const SShaderKey& other) const {
        return effect == other.effect && kind == other.kind;
    }
};

struct SShaderKeyHash {
    size_t operator()(const SShaderKey& key) const {
        return std::hash<std::string>{}(key.effect) ^ (std::hash<std::string>{}(key.kind) << 1U);
    }
};

struct SClosingAnimation {
    PHLWINDOWREF                              window;
    std::optional<std::chrono::steady_clock::time_point> startedAt;
    float                                    seed = 0.F;
    bool                                     suppressedHyprland = false;
};

struct SMonitorShaderState {
    PHLMONITORREF   monitor;
    PHLWORKSPACEREF activeWorkspace;
};

struct SWindowRenderTarget {
    CFramebuffer sourceFramebuffer;
    CFramebuffer passthroughFramebuffer;
};

struct SWorkspaceSwitchRenderState {
    PHLMONITORREF                         monitor;
    PHLWORKSPACEREF                       fromWorkspace;
    PHLWORKSPACEREF                       toWorkspace;
    std::chrono::steady_clock::time_point startedAt;
    SEffectConfig                         cfg;
    float                                 seed                  = 0.F;
    bool                                  previousForceRendering = false;
    bool                                  finished              = false;
    bool                                  restored              = false;
};

struct SAnimationConfigBackup {
    SP<Hyprutils::Animation::SAnimationPropertyConfig> node;
    Hyprutils::Animation::SAnimationPropertyConfig     value;
};

inline SConfig                                                                        g_config;
inline std::vector<Hyprutils::Signal::CHyprSignalListener>                            g_listeners;
inline std::unordered_map<uintptr_t, SClosingAnimation>                               g_closing;
inline std::unordered_map<MONITORID, UP<SMonitorShaderState>>                          g_monitorShaderStates;
inline std::unordered_map<MONITORID, PHLWORKSPACEREF>                                  g_pendingWorkspaceSwitchFrom;
inline std::vector<SP<SWorkspaceSwitchRenderState>>                                   g_workspaceSwitches;
inline std::unordered_set<IWindowTransformer*>                                        g_animatedTransformers;
inline std::unordered_map<std::string, SAnimationConfigBackup>                         g_animationBackups;
inline bool                                                                           g_reloadShaders = false;
inline std::optional<SEffectConfig>                                                   g_effectConfig;
inline std::string                                                                    g_effectConfigKey;

constexpr DRMFormat ANIMATION_FB_FORMAT = DRM_FORMAT_ARGB8888;

using origRenderSnapshot = void (*)(void*, PHLWINDOW);
using origStartWindowAnimation = void (*)(void*, PHLWINDOW, CDesktopAnimationManager::eAnimationType, bool);
using origStartWorkspaceAnimation = void (*)(void*, PHLWORKSPACE, CDesktopAnimationManager::eAnimationType, bool, bool);

} // namespace

// Hyprland 0.54.3 does not export this key function, but the plugin needs the
// interface typeinfo for a derived transformer. Keep the implementation exactly
// equivalent to Hyprland's own no-op definition.
void IWindowTransformer::preWindowRender(CSurfacePassElement::SRenderData*) {
}

namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos)
        return "";

    const auto last = value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}

std::string stripQuotes(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
        return value.substr(1, value.size() - 2);

    return value;
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

Hyprlang::CConfigValue* configValue(const std::string& name) {
    return HyprlandAPI::getConfigValue(PHANDLE, std::string{CONFIG_NS} + name);
}

template <typename T>
T* configPtr(const std::string& name) {
    auto* value = configValue(name);
    return value ? *reinterpret_cast<T* const*>(value->getDataStaticPtr()) : nullptr;
}

Hyprlang::STRING const* configStringPtr(const std::string& name) {
    auto* value = configValue(name);
    return value ? reinterpret_cast<Hyprlang::STRING const*>(value->getDataStaticPtr()) : nullptr;
}

void refreshConfigPtrs() {
    g_config.enabled         = configPtr<Hyprlang::INT>("enabled");
    g_config.effect          = configStringPtr("effect");
    g_config.shadersDir      = configStringPtr("shaders_dir");
    g_config.durationMs      = configPtr<Hyprlang::INT>("duration_ms");
    g_config.workspaceSwitch = configPtr<Hyprlang::INT>("workspace_switch");
    g_config.syncHyprland    = configPtr<Hyprlang::INT>("sync_hyprland");
}

bool enabled() {
    return !g_unloading && g_config.enabled && *g_config.enabled && g_pHyprRenderer && g_pHyprOpenGL;
}

bool workspaceSwitchEnabled() {
    return enabled() && g_config.workspaceSwitch && *g_config.workspaceSwitch;
}

bool syncHyprlandEnabled() {
    return enabled() && (!g_config.syncHyprland || *g_config.syncHyprland);
}

std::string effectName() {
    const std::string configured = g_config.effect ? trim(*g_config.effect) : "";
    return configured.empty() ? "fade" : configured;
}

std::filesystem::path shadersDir() {
    const std::string configured = g_config.shadersDir ? trim(*g_config.shadersDir) : "";
    return configured.empty() ? std::filesystem::path{DEFAULT_SHADERS} : std::filesystem::path{configured};
}

std::string kindName(EAnimationKind kind) {
    return kind == EAnimationKind::OPEN ? "open" : "close";
}

std::string shaderFunction(EAnimationKind kind) {
    return kind == EAnimationKind::OPEN ? "open_color" : "close_color";
}

std::filesystem::path shaderPath(EAnimationKind kind) {
    return shadersDir() / effectName() / (kindName(kind) + ".glsl");
}

bool shaderFileAvailable(EAnimationKind kind) {
    std::error_code ec;
    return std::filesystem::is_regular_file(shaderPath(kind), ec) && !ec;
}

std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.good())
        return std::nullopt;

    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

SEffectConfig readEffectConfig() {
    SEffectConfig out;

    const int configuredDuration = g_config.durationMs ? *g_config.durationMs : 350;
    if (configuredDuration > 0)
        out.durationMs = std::clamp(configuredDuration, 1, 10000);

    const auto cfg = readFile(shadersDir() / effectName() / "config");
    if (!cfg)
        return out;

    std::istringstream stream(*cfg);
    std::string        line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line.starts_with('#'))
            continue;

        if (line.starts_with("duration-ms") && configuredDuration <= 0) {
            std::istringstream valueStream(line.substr(std::string_view{"duration-ms"}.size()));
            int                value = 0;
            if (valueStream >> value)
                out.durationMs = std::clamp(value, 1, 10000);
        } else if (line.starts_with("curve")) {
            out.curve = stripQuotes(line.substr(std::string_view{"curve"}.size()));
        }
    }

    return out;
}

std::string effectConfigKey() {
    const int configuredDuration = g_config.durationMs ? *g_config.durationMs : 350;
    return std::format("{}\n{}\n{}", shadersDir().string(), effectName(), configuredDuration);
}

const SEffectConfig& effectConfig() {
    const auto key = effectConfigKey();
    if (!g_effectConfig || g_effectConfigKey != key) {
        g_effectConfig    = readEffectConfig();
        g_effectConfigKey = key;
    }

    return *g_effectConfig;
}

float ease(float progress, std::string curve) {
    progress = std::clamp(progress, 0.F, 1.F);
    curve    = lower(trim(std::move(curve)));

    if (curve == "linear")
        return progress;
    if (curve == "ease-out-quad")
        return 1.F - (1.F - progress) * (1.F - progress);
    if (curve == "ease-out-expo")
        return progress >= 1.F ? 1.F : 1.F - std::pow(2.F, -10.F * progress);

    return 1.F - std::pow(1.F - progress, 3.F);
}

float elapsedProgress(const std::chrono::steady_clock::time_point& startedAt, const SEffectConfig& cfg) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();
    return std::clamp(static_cast<float>(elapsed) / static_cast<float>(cfg.durationMs), 0.F, 1.F);
}

float elapsedProgress(std::optional<std::chrono::steady_clock::time_point>& startedAt, const SEffectConfig& cfg) {
    if (!startedAt)
        startedAt = std::chrono::steady_clock::now();

    return elapsedProgress(*startedAt, cfg);
}

float randomSeed(uintptr_t value, EAnimationKind kind) {
    uint64_t x = value ^ (kind == EAnimationKind::OPEN ? 0x9e3779b97f4a7c15ULL : 0xd1b54a32d192ed03ULL);
    x ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    x ^= x >> 30U;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31U;
    return static_cast<float>(x & 0xFFFFFFU) / static_cast<float>(0x1000000U);
}

template <typename T>
void* pmfAddress(T pmf) {
    struct PMF {
        uintptr_t ptr;
        ptrdiff_t adj;
    };

    static_assert(std::is_member_function_pointer_v<T>);
    static_assert(sizeof(T) == sizeof(PMF));

    const auto representation = std::bit_cast<PMF>(pmf);
    if (representation.ptr & 0x01U)
        throw std::runtime_error("unexpected virtual function");

    return reinterpret_cast<void*>(representation.ptr);
}

CFunctionHook* hook(void* target, const std::string& signature, void* handler) {
    Dl_info info = {};
    if (!dladdr(target, &info))
        throw std::runtime_error("symbol not available");

#ifdef __GLIBCXX__
    if (signature != info.dli_sname)
        throw std::runtime_error(std::format("unexpected symbol {}", info.dli_sname));
#endif

    auto* hook = HyprlandAPI::createFunctionHook(PHANDLE, target, handler);
    if (!hook || !hook->hook())
        throw std::runtime_error("hooking failed");

    return hook;
}

void callOriginalRenderSnapshot(void* thisptr, PHLWINDOW window) {
    if (!g_pRenderSnapshotHook || !g_pRenderSnapshotHook->m_original)
        return;

    reinterpret_cast<origRenderSnapshot>(g_pRenderSnapshotHook->m_original)(thisptr, window);
}

void callOriginalStartWindowAnimation(void* thisptr, PHLWINDOW window, CDesktopAnimationManager::eAnimationType type, bool force) {
    if (!g_pStartWindowAnimationHook || !g_pStartWindowAnimationHook->m_original)
        return;

    reinterpret_cast<origStartWindowAnimation>(g_pStartWindowAnimationHook->m_original)(thisptr, window, type, force);
}

void callOriginalStartWorkspaceAnimation(void* thisptr, PHLWORKSPACE workspace, CDesktopAnimationManager::eAnimationType type, bool left, bool instant) {
    if (!g_pStartWorkspaceAnimationHook || !g_pStartWorkspaceAnimationHook->m_original)
        return;

    reinterpret_cast<origStartWorkspaceAnimation>(g_pStartWorkspaceAnimationHook->m_original)(thisptr, workspace, type, left, instant);
}

std::string glInfoLog(GLuint object, bool program) {
    GLint len = 0;
    if (program)
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &len);
    else
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &len);

    if (len <= 1)
        return "";

    std::string out(static_cast<size_t>(len), '\0');
    if (program)
        glGetProgramInfoLog(object, len, nullptr, out.data());
    else
        glGetShaderInfoLog(object, len, nullptr, out.data());

    return trim(out);
}

GLuint compileStage(GLenum type, const std::string& source, const std::filesystem::path& path) {
    const GLuint shader = glCreateShader(type);
    const char*  src    = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE)
        return shader;

    const auto log = glInfoLog(shader, false);
    glDeleteShader(shader);
    Log::logger->log(Log::ERR, "[hypranimated] failed to compile {}: {}", path.string(), log);
    return 0;
}

class CAnimationShader {
  public:
    CAnimationShader(EAnimationKind kind, std::filesystem::path path, std::string userSource) : m_kind(kind), m_path(std::move(path)) {
        create(std::move(userSource));
    }

    ~CAnimationShader() {
        destroy();
    }

    bool valid() const {
        return m_program != 0 && m_vao != 0 && m_vbo != 0;
    }

    void render(SP<CTexture> texture, const CBox& geometryPx, const CBox& sourceGeometryPx, const Vector2D& monitorSize, float progress, float seed,
                const CRegion& damage) {
        if (!valid() || !texture || texture->m_texID == 0 || !g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.pMonitor)
            return;

        const CBox monbox = {0, 0, monitorSize.x, monitorSize.y};
        const auto transform =
            Math::wlTransformToHyprutils(Math::invertTransform(g_pHyprOpenGL->m_renderData.pMonitor->m_transform));
        const Mat3x3 matrix   = g_pHyprOpenGL->m_renderData.monitorProjection.projectBox(monbox, transform, monbox.rot);
        const Mat3x3 glMatrix = g_pHyprOpenGL->m_renderData.projection.copy().multiply(matrix);

        glUseProgram(m_program);
        glUniformMatrix3fv(m_uniformProj, 1, GL_TRUE, glMatrix.getMatrix().data());
        glUniform1i(m_uniformTex, 0);
        glUniform1f(m_uniformProgress, progress);
        glUniform1f(m_uniformClampedProgress, std::clamp(progress, 0.F, 1.F));
        glUniform1f(m_uniformSeed, seed);
        glUniform2f(m_uniformMonitorSize, monitorSize.x, monitorSize.y);
        glUniform4f(m_uniformGeometry, geometryPx.x, geometryPx.y, std::max(1.0, geometryPx.w), std::max(1.0, geometryPx.h));

        const float sx = static_cast<float>(sourceGeometryPx.w / std::max(1.0, monitorSize.x));
        const float sy = static_cast<float>(sourceGeometryPx.h / std::max(1.0, monitorSize.y));
        const float tx = static_cast<float>(sourceGeometryPx.x / std::max(1.0, monitorSize.x));
        const float ty = static_cast<float>(sourceGeometryPx.y / std::max(1.0, monitorSize.y));
        const std::array<GLfloat, 9> geoToTex = {sx, 0.F, 0.F, 0.F, sy, 0.F, tx, ty, 1.F};
        glUniformMatrix3fv(m_uniformGeoToTex, 1, GL_FALSE, geoToTex.data());

        g_pHyprOpenGL->blend(true);

        glActiveTexture(GL_TEXTURE0);
        texture->bind();
        texture->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        texture->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        texture->setTexParameter(GL_TEXTURE_MAG_FILTER, texture->magFilter);
        texture->setTexParameter(GL_TEXTURE_MIN_FILTER, texture->minFilter);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

        const CRegion drawDamage = damage.empty() ? CRegion{CBox{0, 0, monitorSize.x, monitorSize.y}} : damage;
        drawDamage.forEachRect([](const auto& rect) {
            g_pHyprOpenGL->scissor(&rect, g_pHyprOpenGL->m_renderData.transformDamage);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        });

        g_pHyprOpenGL->scissor(nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        texture->unbind();
        glUseProgram(0);
    }

  private:
    EAnimationKind       m_kind;
    std::filesystem::path m_path;
    GLuint               m_program = 0;
    GLuint               m_vao     = 0;
    GLuint               m_vbo     = 0;

    GLint m_uniformProj            = -1;
    GLint m_uniformTex             = -1;
    GLint m_uniformGeoToTex        = -1;
    GLint m_uniformProgress        = -1;
    GLint m_uniformClampedProgress = -1;
    GLint m_uniformSeed            = -1;
    GLint m_uniformMonitorSize     = -1;
    GLint m_uniformGeometry        = -1;

    void create(std::string userSource) {
        const std::string vertexSource = R"GLSL(
#version 300 es
uniform mat3 proj;
in vec2 pos;
in vec2 texcoord;
out vec2 v_texcoord;
void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = texcoord;
}
)GLSL";

        const std::string fragmentSource = std::format(R"GLSL(
#version 300 es
precision highp float;

in vec2 v_texcoord;
uniform sampler2D niri_tex;
uniform mat3 niri_geo_to_tex;
uniform float niri_progress;
uniform float niri_clamped_progress;
uniform float niri_random_seed;
uniform vec2 hypr_monitor_size;
uniform vec4 hypr_geometry;

layout(location = 0) out vec4 fragColor;

#define texture2D texture

{}

void main() {{
    vec2 pixel = v_texcoord * hypr_monitor_size;
    vec2 safe_size = max(hypr_geometry.zw, vec2(1.0));
    vec3 coords_geo = vec3((pixel - hypr_geometry.xy) / safe_size, 1.0);
    vec3 size_geo = vec3(safe_size, 1.0);
    fragColor = {}(coords_geo, size_geo);
}}
)GLSL",
                                                        userSource, shaderFunction(m_kind));

        const GLuint vert = compileStage(GL_VERTEX_SHADER, vertexSource, m_path);
        if (!vert)
            return;

        const GLuint frag = compileStage(GL_FRAGMENT_SHADER, fragmentSource, m_path);
        if (!frag) {
            glDeleteShader(vert);
            return;
        }

        m_program = glCreateProgram();
        glAttachShader(m_program, vert);
        glAttachShader(m_program, frag);
        glLinkProgram(m_program);
        glDetachShader(m_program, vert);
        glDetachShader(m_program, frag);
        glDeleteShader(vert);
        glDeleteShader(frag);

        GLint ok = GL_FALSE;
        glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) {
            Log::logger->log(Log::ERR, "[hypranimated] failed to link {}: {}", m_path.string(), glInfoLog(m_program, true));
            destroy();
            return;
        }

        m_uniformProj            = glGetUniformLocation(m_program, "proj");
        m_uniformTex             = glGetUniformLocation(m_program, "niri_tex");
        m_uniformGeoToTex        = glGetUniformLocation(m_program, "niri_geo_to_tex");
        m_uniformProgress        = glGetUniformLocation(m_program, "niri_progress");
        m_uniformClampedProgress = glGetUniformLocation(m_program, "niri_clamped_progress");
        m_uniformSeed            = glGetUniformLocation(m_program, "niri_random_seed");
        m_uniformMonitorSize     = glGetUniformLocation(m_program, "hypr_monitor_size");
        m_uniformGeometry        = glGetUniformLocation(m_program, "hypr_geometry");

        const GLint posAttr = glGetAttribLocation(m_program, "pos");
        const GLint texAttr = glGetAttribLocation(m_program, "texcoord");
        if (m_uniformProj < 0 || m_uniformMonitorSize < 0 || m_uniformGeometry < 0 || posAttr < 0 || texAttr < 0) {
            Log::logger->log(Log::ERR, "[hypranimated] shader {} is missing required wrapper uniforms or attributes", m_path.string());
            destroy();
            return;
        }

        glGenVertexArrays(1, &m_vao);
        glBindVertexArray(m_vao);
        glGenBuffers(1, &m_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(fullVerts), fullVerts.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(posAttr);
        glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, sizeof(SVertex), reinterpret_cast<void*>(offsetof(SVertex, x)));
        glEnableVertexAttribArray(texAttr);
        glVertexAttribPointer(texAttr, 2, GL_FLOAT, GL_FALSE, sizeof(SVertex), reinterpret_cast<void*>(offsetof(SVertex, u)));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }

    void destroy() {
        if (m_vbo) {
            glDeleteBuffers(1, &m_vbo);
            m_vbo = 0;
        }
        if (m_vao) {
            glDeleteVertexArrays(1, &m_vao);
            m_vao = 0;
        }
        if (m_program) {
            glDeleteProgram(m_program);
            m_program = 0;
        }
    }
};

inline std::unordered_map<SShaderKey, UP<CAnimationShader>, SShaderKeyHash> g_shaders;

void retireShadersInGLContext() {
    if (!g_reloadShaders)
        return;

    if (g_pHyprRenderer)
        g_pHyprRenderer->makeEGLCurrent();

    g_shaders.clear();
    g_reloadShaders = false;
}

CAnimationShader* shaderFor(EAnimationKind kind) {
    retireShadersInGLContext();

    const auto key = SShaderKey{effectName(), kindName(kind)};
    if (auto it = g_shaders.find(key); it != g_shaders.end())
        return it->second && it->second->valid() ? it->second.get() : nullptr;

    const auto path = shaderPath(kind);
    const auto src  = readFile(path);
    if (!src) {
        Log::logger->log(Log::ERR, "[hypranimated] shader file not found: {}", path.string());
        g_shaders.emplace(key, nullptr);
        return nullptr;
    }

    auto shader = makeUnique<CAnimationShader>(kind, path, *src);
    auto* ptr   = shader && shader->valid() ? shader.get() : nullptr;
    g_shaders.emplace(key, std::move(shader));
    return ptr;
}

CBox scaledGeometry(const CBox& logicalBox, PHLMONITOR monitor) {
    return logicalBox.copy().translate(-monitor->m_position).scale(monitor->m_scale);
}

bool ensureFramebuffer(CFramebuffer& fb, const Vector2D& size, DRMFormat format) {
    if (size.x <= 0 || size.y <= 0)
        return false;

    if (fb.isAllocated() && fb.m_size == size && fb.m_drmFormat == format)
        return true;

    if (fb.isAllocated())
        fb.release();

    return fb.alloc(static_cast<int>(size.x), static_cast<int>(size.y), format);
}

bool copyFramebufferColor(CFramebuffer* source, CFramebuffer& target) {
    if (!g_pHyprOpenGL || !source || !source->isAllocated() || !source->getTexture() || source->m_size.x <= 0 || source->m_size.y <= 0)
        return false;

    const DRMFormat format = source->m_drmFormat ? source->m_drmFormat : DRM_FORMAT_ARGB8888;
    if (!ensureFramebuffer(target, source->m_size, format))
        return false;

    auto* previousCurrentFB = g_pHyprOpenGL->m_renderData.currentFB;
    GLint previousReadFB   = 0;
    GLint previousDrawFB   = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFB);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFB);

    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, source->getFBID());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.getFBID());
    glBlitFramebuffer(0, 0, static_cast<GLint>(source->m_size.x), static_cast<GLint>(source->m_size.y), 0, 0, static_cast<GLint>(source->m_size.x),
                      static_cast<GLint>(source->m_size.y), GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if (previousCurrentFB)
        previousCurrentFB->bind();
    else
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFB);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFB);

    g_pHyprOpenGL->m_renderData.currentFB = previousCurrentFB;
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, scissorEnabled == GL_TRUE);

    return true;
}

CBox clippedAnimationBox(CBox box, const Vector2D& monitorSize) {
    const double x1 = std::clamp(box.x, 0.0, monitorSize.x);
    const double y1 = std::clamp(box.y, 0.0, monitorSize.y);
    const double x2 = std::clamp(box.x + box.w, 0.0, monitorSize.x);
    const double y2 = std::clamp(box.y + box.h, 0.0, monitorSize.y);

    if (x2 <= x1 || y2 <= y1)
        return {0, 0, 0, 0};

    return {std::floor(x1), std::floor(y1), std::ceil(x2) - std::floor(x1), std::ceil(y2) - std::floor(y1)};
}

CRegion animationDamageForGeometry(const CBox& geometryPx, const Vector2D& monitorSize) {
    const auto box = clippedAnimationBox(geometryPx, monitorSize);
    if (box.w <= 0 || box.h <= 0)
        return {};

    return CRegion{box};
}

CBox monitorPixelBoxToLayoutBox(const CBox& geometryPx, PHLMONITOR monitor) {
    const double scale = std::max(0.01, monitor ? monitor->m_scale : 1.0);
    const auto   pos   = monitor ? monitor->m_position : Vector2D{};

    return CBox{geometryPx.x / scale + pos.x, geometryPx.y / scale + pos.y, geometryPx.w / scale, geometryPx.h / scale};
}

bool coversMonitor(const CBox& geometryPx, const Vector2D& monitorSize) {
    return geometryPx.x <= 0.5 && geometryPx.y <= 0.5 && geometryPx.x + geometryPx.w >= monitorSize.x - 0.5 &&
        geometryPx.y + geometryPx.h >= monitorSize.y - 0.5;
}

void damageAnimationGeometry(PHLMONITOR monitor, const CBox& geometryPx) {
    if (!monitor || !g_pHyprRenderer)
        return;

    if (coversMonitor(geometryPx, monitor->m_transformedSize)) {
        g_pHyprRenderer->damageMonitor(monitor);
        return;
    }

    const auto clipped = clippedAnimationBox(geometryPx, monitor->m_transformedSize);
    if (clipped.w > 0 && clipped.h > 0)
        g_pHyprRenderer->damageBox(monitorPixelBoxToLayoutBox(clipped, monitor));
}

void forceCurrentRenderDamage(PHLMONITOR monitor, const CRegion& damage) {
    if (!monitor || !g_pHyprOpenGL || damage.empty())
        return;

    const auto currentMonitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!currentMonitor || currentMonitor.get() != monitor.get())
        return;

    auto combinedDamage = g_pHyprOpenGL->m_renderData.damage.copy();
    combinedDamage.add(damage);

    auto combinedFinalDamage = g_pHyprOpenGL->m_renderData.finalDamage.copy();
    combinedFinalDamage.add(damage);

    g_pHyprOpenGL->setDamage(combinedDamage, combinedFinalDamage);
}

class CAnimatedShaderPassElement : public IPassElement {
  public:
    CAnimatedShaderPassElement(SP<CTexture> texture, CAnimationShader* shader, CBox geometryPx, CBox sourceGeometryPx, Vector2D monitorSize, float progress, float seed,
                               CRegion damage) :
        m_texture(std::move(texture)),
        m_shader(shader),
        m_geometryPx(std::move(geometryPx)),
        m_sourceGeometryPx(std::move(sourceGeometryPx)),
        m_monitorSize(std::move(monitorSize)),
        m_progress(progress),
        m_seed(seed),
        m_damage(std::move(damage)) {
    }

    CAnimatedShaderPassElement(CFramebuffer* liveSourceFramebuffer, CAnimationShader* shader, CBox geometryPx, CBox sourceGeometryPx, Vector2D monitorSize, float progress,
                               float seed, CRegion damage) :
        m_liveSourceFramebuffer(liveSourceFramebuffer),
        m_shader(shader),
        m_geometryPx(std::move(geometryPx)),
        m_sourceGeometryPx(std::move(sourceGeometryPx)),
        m_monitorSize(std::move(monitorSize)),
        m_progress(progress),
        m_seed(seed),
        m_damage(std::move(damage)) {
    }

    void draw(const CRegion& damage) override {
        if (!m_shader)
            return;

        if (g_pHyprOpenGL && g_pHyprOpenGL->m_renderData.mainFB)
            g_pHyprOpenGL->bindBackOnMain();

        auto texture = m_texture;
        if (m_liveSourceFramebuffer) {
            if (!copyFramebufferColor(m_liveSourceFramebuffer, m_stableSourceFramebuffer))
                return;

            texture = m_stableSourceFramebuffer.getTexture();
        }

        m_shader->render(texture, m_geometryPx, m_sourceGeometryPx, m_monitorSize, m_progress, m_seed, m_damage.empty() ? damage : m_damage);
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    bool undiscardable() override {
        return true;
    }

    std::optional<CBox> boundingBox() override {
        if (!g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.pMonitor)
            return std::nullopt;

        const auto scale = std::max(0.01F, g_pHyprOpenGL->m_renderData.pMonitor->m_scale);
        return CBox{m_geometryPx.x / scale, m_geometryPx.y / scale, m_geometryPx.w / scale, m_geometryPx.h / scale};
    }

    CRegion opaqueRegion() override {
        return {};
    }

    const char* passName() override {
        return "CAnimatedShaderPassElement";
    }

  private:
    SP<CTexture>       m_texture;
    CFramebuffer*      m_liveSourceFramebuffer = nullptr;
    CFramebuffer       m_stableSourceFramebuffer;
    CAnimationShader*  m_shader = nullptr;
    CBox               m_geometryPx;
    CBox               m_sourceGeometryPx;
    Vector2D           m_monitorSize;
    float              m_progress = 0.F;
    float              m_seed     = 0.F;
    CRegion            m_damage;
};

class CBindOffMainPassElement : public IPassElement {
  public:
    explicit CBindOffMainPassElement(SP<SWindowRenderTarget> renderTarget = {}) : m_renderTarget(std::move(renderTarget)) {
    }

    void draw(const CRegion&) override {
        if (!g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.pMonitor || !g_pHyprOpenGL->m_renderData.pCurrentMonData)
            return;

        const auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
        if (!monitor)
            return;

        CFramebuffer* target = nullptr;
        if (m_renderTarget) {
            target = &m_renderTarget->sourceFramebuffer;
            if (!ensureFramebuffer(*target, monitor->m_pixelSize, ANIMATION_FB_FORMAT))
                return;

            if (!target->getStencilTex() && g_pHyprOpenGL->m_renderData.pCurrentMonData->stencilTex)
                target->addStencil(g_pHyprOpenGL->m_renderData.pCurrentMonData->stencilTex);
        } else {
            auto& offMain = g_pHyprOpenGL->m_renderData.pCurrentMonData->offMainFB;
            if (!offMain.isAllocated()) {
                if (!offMain.alloc(monitor->m_pixelSize.x, monitor->m_pixelSize.y, monitor->m_output->state->state().drmFormat))
                    return;

                offMain.addStencil(g_pHyprOpenGL->m_renderData.pCurrentMonData->stencilTex);
            }

            target = &offMain;
        }

        target->bind();
        g_pHyprOpenGL->m_renderData.currentFB = target;
        glClearColor(0.F, 0.F, 0.F, 0.F);
        g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    bool undiscardable() override {
        return true;
    }

    std::optional<CBox> boundingBox() override {
        return std::nullopt;
    }

    CRegion opaqueRegion() override {
        return {};
    }

    bool disableSimplification() override {
        // The following window surface passes render into this private capture
        // framebuffer, not the main framebuffer, so they must not occlude main
        // pass damage behind the animated window.
        return true;
    }

    const char* passName() override {
        return "CBindOffMainPassElement";
    }

  private:
    SP<SWindowRenderTarget> m_renderTarget;
};

class CWindowShaderTransformer : public IWindowTransformer {
  public:
    explicit CWindowShaderTransformer(PHLWINDOW window) :
        m_window(window),
        m_renderTarget(makeShared<SWindowRenderTarget>()),
        m_seed(randomSeed(reinterpret_cast<uintptr_t>(window.get()), EAnimationKind::OPEN)) {
    }

    CWindowShaderTransformer(PHLWINDOW window, EAnimationKind kind, SP<SWorkspaceSwitchRenderState> workspaceSwitch) :
        m_window(window),
        m_kind(kind),
        m_workspaceSwitch(std::move(workspaceSwitch)),
        m_renderTarget(makeShared<SWindowRenderTarget>()) {
    }

    void preWindowRender(CSurfacePassElement::SRenderData* renderData) override {
        if (!enabled() || !renderData || !renderData->pMonitor)
            return;

        const auto monitor = renderData->pMonitor.lock();
        if (!monitor)
            return;

        m_cfg      = m_workspaceSwitch ? m_workspaceSwitch->cfg : effectConfig();
        m_shader   = shaderFor(m_kind);
        m_monitor  = renderData->pMonitor;
        m_geometry = scaledGeometry(CBox{renderData->pos.x, renderData->pos.y, renderData->w, renderData->h}, monitor);

        if (animationComplete()) {
            m_done = true;
            if (m_workspaceSwitch)
                m_workspaceSwitch->finished = true;
            return;
        }

        if (m_shader && m_renderTarget) {
            const CBox geometry = m_workspaceSwitch ? CBox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y} : m_geometry;
            forceCurrentRenderDamage(monitor, animationDamageForGeometry(geometry, monitor->m_transformedSize));
            g_pHyprRenderer->m_renderPass.add(makeUnique<CBindOffMainPassElement>(m_renderTarget));
            renderData->fadeAlpha = 1.F;
        } else {
            m_done = true;
        }
    }

    CFramebuffer* transform(CFramebuffer* in) override {
        if (!enabled() || !in || !in->getTexture() || !m_monitor || !m_shader || !m_renderTarget) {
            m_done = true;
            return in;
        }

        const auto monitor = m_monitor.lock();
        if (!monitor) {
            m_done = true;
            return in;
        }

        if (m_workspaceSwitch && m_workspaceSwitch->finished) {
            m_done = true;
            return in;
        }

        const float rawProgress = rawAnimationProgress();
        if (rawProgress >= 1.F) {
            if (m_workspaceSwitch)
                m_workspaceSwitch->finished = true;

            m_done = true;
            return in;
        }

        const float progress    = ease(rawProgress, m_cfg.curve);
        const CBox  monbox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y};
        const auto  geometry    = m_workspaceSwitch ? monbox : m_geometry;
        const auto  monitorSize = monitor->m_transformedSize;
        const auto  seed        = m_workspaceSwitch ? m_workspaceSwitch->seed : m_seed;
        const auto  damage      = animationDamageForGeometry(geometry, monitorSize);

        if (damage.empty()) {
            m_done = true;
            return in;
        }

        auto* passthrough = clearPassthroughFramebuffer(monitor);
        if (!passthrough) {
            m_done = true;
            return in;
        }

        g_pHyprRenderer->m_renderPass.add(makeUnique<CAnimatedShaderPassElement>(&m_renderTarget->sourceFramebuffer, m_shader, geometry, geometry, monitorSize, progress,
                                                                                 seed, damage));

        damageAnimationGeometry(monitor, geometry);
        g_pHyprOpenGL->blend(true);
        return passthrough;
    }

    bool done() const {
        return m_done || !enabled() || !m_window.lock() || animationCompleteByClock();
    }

    SP<SWorkspaceSwitchRenderState> workspaceSwitch() const {
        return m_workspaceSwitch;
    }

  private:
    PHLWINDOWREF                              m_window;
    PHLMONITORREF                            m_monitor;
    std::optional<std::chrono::steady_clock::time_point> m_startedAt;
    EAnimationKind                            m_kind = EAnimationKind::OPEN;
    SP<SWorkspaceSwitchRenderState>           m_workspaceSwitch;
    SP<SWindowRenderTarget>                  m_renderTarget;
    float                                    m_seed = 0.F;
    SEffectConfig                            m_cfg;
    CAnimationShader*                        m_shader = nullptr;
    CBox                                     m_geometry;
    bool                                     m_done = false;

    CFramebuffer* clearPassthroughFramebuffer(PHLMONITOR monitor) {
        if (!monitor || !g_pHyprOpenGL || !m_renderTarget)
            return nullptr;

        auto& passthrough = m_renderTarget->passthroughFramebuffer;
        if (!ensureFramebuffer(passthrough, monitor->m_pixelSize, ANIMATION_FB_FORMAT))
            return nullptr;

        auto* previousCurrentFB = g_pHyprOpenGL->m_renderData.currentFB;
        GLint previousDrawFB   = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFB);

        const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

        passthrough.bind();
        g_pHyprOpenGL->m_renderData.currentFB = &passthrough;
        glClearColor(0.F, 0.F, 0.F, 0.F);
        g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
        glClear(GL_COLOR_BUFFER_BIT);

        if (previousCurrentFB)
            previousCurrentFB->bind();
        else
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFB);

        g_pHyprOpenGL->m_renderData.currentFB = previousCurrentFB;
        g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, scissorEnabled == GL_TRUE);
        return &passthrough;
    }

    float rawAnimationProgress() {
        if (m_workspaceSwitch)
            return elapsedProgress(m_workspaceSwitch->startedAt, m_workspaceSwitch->cfg);

        return elapsedProgress(m_startedAt, m_cfg);
    }

    bool animationComplete() {
        if (m_workspaceSwitch)
            return m_workspaceSwitch->finished || elapsedProgress(m_workspaceSwitch->startedAt, m_workspaceSwitch->cfg) >= 1.F;

        return m_startedAt && elapsedProgress(*m_startedAt, m_cfg) >= 1.F;
    }

    bool animationCompleteByClock() const {
        if (m_workspaceSwitch)
            return m_workspaceSwitch->finished || elapsedProgress(m_workspaceSwitch->startedAt, m_workspaceSwitch->cfg) >= 1.F;

        return m_startedAt && elapsedProgress(*m_startedAt, m_cfg) >= 1.F;
    }

};

void backupAnimationConfig(const std::string& name, SP<Hyprutils::Animation::SAnimationPropertyConfig> node) {
    if (!node || g_animationBackups.contains(name))
        return;

    g_animationBackups.emplace(name, SAnimationConfigBackup{.node = node, .value = *node});
}

void restoreAnimationConfigs() {
    for (auto& [_, backup] : g_animationBackups) {
        if (backup.node)
            *backup.node = backup.value;
    }

    g_animationBackups.clear();
}

void syncWindowsMoveConfig() {
    if (!syncHyprlandEnabled() || !g_pConfigManager)
        return;

    const auto moveConfig = g_pConfigManager->getAnimationPropertyConfig("windowsMove");
    if (!moveConfig || !moveConfig->pValues)
        return;

    backupAnimationConfig("windowsMove", moveConfig);

    const auto values = moveConfig->pValues;
    auto       parent = moveConfig->pParentAnimation;
    if (!parent)
        parent = moveConfig;

    const float speed = std::clamp(static_cast<float>(effectConfig().durationMs) / 100.F, 0.01F, 100.F);
    *moveConfig       = Hyprutils::Animation::SAnimationPropertyConfig{
              .overridden       = true,
              .internalBezier   = values->internalBezier.empty() ? "default" : values->internalBezier,
              .internalStyle    = values->internalStyle,
              .internalSpeed    = speed,
              .internalEnabled  = values->internalEnabled < 0 ? 1 : values->internalEnabled,
              .pValues          = moveConfig,
              .pParentAnimation = parent,
    };
}

void disableWorkspaceAnimationConfig(const std::string& name) {
    if (!workspaceSwitchEnabled() || !g_pConfigManager)
        return;

    const auto config = g_pConfigManager->getAnimationPropertyConfig(name);
    if (!config || !config->pValues)
        return;

    backupAnimationConfig(name, config);

    const auto values = config->pValues;
    auto       parent = config->pParentAnimation;
    if (!parent)
        parent = config;

    *config = Hyprutils::Animation::SAnimationPropertyConfig{
        .overridden       = true,
        .internalBezier   = values->internalBezier.empty() ? "default" : values->internalBezier,
        .internalStyle    = values->internalStyle,
        .internalSpeed    = values->internalSpeed <= 0.F ? 1.F : values->internalSpeed,
        .internalEnabled  = 0,
        .pValues          = config,
        .pParentAnimation = parent,
    };
}

void disableWorkspaceSlideAnimations() {
    disableWorkspaceAnimationConfig("workspaces");
    disableWorkspaceAnimationConfig("workspacesIn");
    disableWorkspaceAnimationConfig("workspacesOut");
}

void applyAnimationOverrides() {
    syncWindowsMoveConfig();
    disableWorkspaceSlideAnimations();
}

void restoreWorkspaceSwitchState(const SP<SWorkspaceSwitchRenderState>& state) {
    if (!state || state->restored)
        return;

    if (const auto fromWorkspace = state->fromWorkspace.lock(); fromWorkspace && !fromWorkspace->inert())
        fromWorkspace->m_forceRendering = state->previousForceRendering;

    state->restored = true;
}

void finishAllWorkspaceSwitches() {
    for (auto& state : g_workspaceSwitches) {
        if (!state)
            continue;

        state->finished = true;
        restoreWorkspaceSwitchState(state);
    }

    g_workspaceSwitches.clear();
}

SMonitorShaderState& monitorShaderState(PHLMONITOR monitor) {
    auto& state = g_monitorShaderStates[monitor->m_id];
    if (!state)
        state = makeUnique<SMonitorShaderState>();

    state->monitor = monitor;
    return *state;
}

void rememberActiveWorkspaceForCurrentMonitor() {
    if (!g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.pMonitor)
        return;

    const auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!monitor || monitor->isMirror() || !monitor->m_activeWorkspace)
        return;

    monitorShaderState(monitor).activeWorkspace = monitor->m_activeWorkspace;
}

void rememberActiveWorkspacesForAllMonitors() {
    if (!g_pCompositor)
        return;

    for (auto const& monitor : g_pCompositor->m_monitors) {
        if (!monitor || monitor->isMirror() || !monitor->m_activeWorkspace)
            continue;

        monitorShaderState(monitor).activeWorkspace = monitor->m_activeWorkspace;
    }
}

void forceWorkspaceInstant(PHLWORKSPACE workspace, bool visible) {
    if (!workspace)
        return;

    workspace->m_renderOffset->setValueAndWarp(Vector2D{0.F, 0.F});
    workspace->m_alpha->setValueAndWarp(visible ? 1.F : 0.F);
}

bool windowBelongsToWorkspace(PHLWINDOW window, PHLWORKSPACE workspace) {
    return window && workspace && window->m_workspace && window->m_workspace.get() == workspace.get();
}

bool canAnimateWorkspaceWindow(PHLWINDOW window, PHLWORKSPACE workspace) {
    if (!windowBelongsToWorkspace(window, workspace))
        return false;

    if (!window->m_isMapped || window->m_fadingOut || window->m_readyToDelete || window->m_pinned || window->isHidden() || window->desktopComponent())
        return false;

    return true;
}

bool hasAnimatedTransformer(PHLWINDOW window);
void sweepAnimations();

void cancelWorkspaceSwitchesForMonitor(PHLMONITOR monitor) {
    if (!monitor)
        return;

    for (auto& state : g_workspaceSwitches) {
        const auto stateMonitor = state ? state->monitor.lock() : nullptr;
        if (stateMonitor && stateMonitor->m_id == monitor->m_id) {
            if (state)
                state->finished = true;
            restoreWorkspaceSwitchState(state);
        }
    }

    if (g_pCompositor) {
        for (auto const& window : g_pCompositor->m_windows) {
            if (!window)
                continue;

            auto& transformers = window->m_transformers;
            transformers.erase(std::remove_if(transformers.begin(), transformers.end(), [&](const auto& transformer) {
                                   if (!g_animatedTransformers.contains(transformer.get()))
                                       return false;

                                   auto* animated = static_cast<CWindowShaderTransformer*>(transformer.get());
                                   const auto state = animated->workspaceSwitch();
                                   const auto stateMonitor = state ? state->monitor.lock() : nullptr;
                                   if (!stateMonitor || stateMonitor->m_id != monitor->m_id)
                                       return false;

                                   g_animatedTransformers.erase(transformer.get());
                                   return true;
                               }),
                               transformers.end());
        }
    }

    g_workspaceSwitches.erase(std::remove_if(g_workspaceSwitches.begin(), g_workspaceSwitches.end(), [&](const auto& state) {
                                  const auto stateMonitor = state ? state->monitor.lock() : nullptr;
                                  return !state || (stateMonitor && stateMonitor->m_id == monitor->m_id);
                              }),
                              g_workspaceSwitches.end());
}

bool attachWorkspaceTransformer(PHLWINDOW window, EAnimationKind kind, const SP<SWorkspaceSwitchRenderState>& state) {
    if (!window || !state || hasAnimatedTransformer(window))
        return false;

    auto transformer = makeUnique<CWindowShaderTransformer>(window, kind, state);
    g_animatedTransformers.insert(transformer.get());
    window->m_transformers.emplace_back(std::move(transformer));
    return true;
}

void startWorkspaceSwitchAnimation(PHLWORKSPACE workspace, PHLWORKSPACE fromWorkspaceOverride = nullptr) {
    if (!workspaceSwitchEnabled() || !workspace || workspace->m_isSpecialWorkspace || !shaderFileAvailable(EAnimationKind::OPEN) ||
        !shaderFileAvailable(EAnimationKind::CLOSE))
        return;

    sweepAnimations();

    const auto monitor = workspace->m_monitor.lock();
    if (!monitor || monitor->isMirror())
        return;

    auto& state = monitorShaderState(monitor);
    auto fromWorkspace     = fromWorkspaceOverride ? fromWorkspaceOverride : state.activeWorkspace.lock();
    state.activeWorkspace  = workspace;
    const bool sameWorkspace = fromWorkspace && fromWorkspace.get() == workspace.get();
    if (sameWorkspace)
        return;

    cancelWorkspaceSwitchesForMonitor(monitor);

    if (fromWorkspace && fromWorkspace->m_isSpecialWorkspace)
        fromWorkspace.reset();

    forceWorkspaceInstant(workspace, true);
    if (fromWorkspace)
        forceWorkspaceInstant(fromWorkspace, false);

    const auto fromWorkspaceId = fromWorkspace ? fromWorkspace->m_id : WORKSPACE_INVALID;
    const auto seedKey = (static_cast<uintptr_t>(monitor->m_id) << 32U) ^ static_cast<uintptr_t>(fromWorkspaceId) ^
        (static_cast<uintptr_t>(workspace->m_id) << 1U);
    auto switchState = makeShared<SWorkspaceSwitchRenderState>();
    switchState->monitor                = PHLMONITORREF{monitor};
    switchState->fromWorkspace          = fromWorkspace ? PHLWORKSPACEREF{fromWorkspace} : PHLWORKSPACEREF{};
    switchState->toWorkspace            = PHLWORKSPACEREF{workspace};
    switchState->startedAt              = std::chrono::steady_clock::now();
    switchState->cfg                    = effectConfig();
    switchState->seed                   = randomSeed(seedKey, EAnimationKind::OPEN);
    switchState->previousForceRendering = fromWorkspace ? fromWorkspace->m_forceRendering : false;

    size_t attached = 0;
    if (fromWorkspace)
        fromWorkspace->m_forceRendering = true;

    if (g_pCompositor) {
        for (auto const& window : g_pCompositor->m_windows) {
            if (!window)
                continue;

            if (fromWorkspace && canAnimateWorkspaceWindow(window, fromWorkspace) && attachWorkspaceTransformer(window, EAnimationKind::CLOSE, switchState))
                ++attached;
            else if (canAnimateWorkspaceWindow(window, workspace) && attachWorkspaceTransformer(window, EAnimationKind::OPEN, switchState))
                ++attached;
        }
    }

    if (attached == 0) {
        restoreWorkspaceSwitchState(switchState);
        return;
    }

    g_workspaceSwitches.emplace_back(std::move(switchState));
    g_pHyprRenderer->damageMonitor(monitor);
}

void sweepWorkspaceSwitches() {
    for (auto& state : g_workspaceSwitches) {
        if (!state)
            continue;

        if (elapsedProgress(state->startedAt, state->cfg) >= 1.F)
            state->finished = true;

        if (state->finished)
            restoreWorkspaceSwitchState(state);
        else if (const auto monitor = state->monitor.lock(); monitor)
            g_pHyprRenderer->damageMonitor(monitor);
    }

    g_workspaceSwitches.erase(std::remove_if(g_workspaceSwitches.begin(), g_workspaceSwitches.end(), [](const auto& state) {
                                  return !state || state->finished;
                              }),
                              g_workspaceSwitches.end());
}

void renderWorkspaceSwitchForCurrentMonitor() {
    rememberActiveWorkspaceForCurrentMonitor();
}

void detectWorkspaceSwitchForCurrentMonitor() {
    if (!g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.pMonitor)
        return;

    const auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!monitor || monitor->isMirror() || !monitor->m_activeWorkspace)
        return;

    auto&      state = monitorShaderState(monitor);
    const auto previousWorkspace = state.activeWorkspace.lock();
    if (workspaceSwitchEnabled() && previousWorkspace && previousWorkspace.get() != monitor->m_activeWorkspace.get()) {
        startWorkspaceSwitchAnimation(monitor->m_activeWorkspace);
        return;
    }

    state.activeWorkspace = monitor->m_activeWorkspace;
}

uintptr_t windowKey(PHLWINDOW window) {
    return reinterpret_cast<uintptr_t>(window.get());
}

bool hasAnimatedTransformer(PHLWINDOW window) {
    if (!window)
        return false;

    auto& transformers = window->m_transformers;
    transformers.erase(std::remove_if(transformers.begin(), transformers.end(), [](const auto& transformer) {
                           if (!g_animatedTransformers.contains(transformer.get()))
                               return false;

                           auto* animated = static_cast<CWindowShaderTransformer*>(transformer.get());
                           if (!animated->done())
                               return false;

                           g_animatedTransformers.erase(transformer.get());
                           return true;
                       }),
                       transformers.end());

    return std::ranges::any_of(transformers, [](const auto& transformer) { return g_animatedTransformers.contains(transformer.get()); });
}

void attachOpenTransformer(PHLWINDOW window) {
    if (!window || hasAnimatedTransformer(window))
        return;

    auto transformer = makeUnique<CWindowShaderTransformer>(window);
    g_animatedTransformers.insert(transformer.get());
    window->m_transformers.emplace_back(std::move(transformer));

    g_pHyprRenderer->damageWindow(window, true);
}

void ensureClosingAnimation(PHLWINDOW window) {
    if (!window || g_closing.contains(windowKey(window)))
        return;

    g_closing[windowKey(window)] = SClosingAnimation{
        .window = PHLWINDOWREF{window},
        .seed   = randomSeed(windowKey(window), EAnimationKind::CLOSE),
    };

    g_pHyprRenderer->damageWindow(window, true);
}

void setMoveAnimationConfig(PHLWINDOW window) {
    if (!window || !g_pConfigManager)
        return;

    const auto moveConfig = g_pConfigManager->getAnimationPropertyConfig("windowsMove");
    if (moveConfig) {
        window->m_realPosition->setConfig(moveConfig);
        window->m_realSize->setConfig(moveConfig);
    }
}

void keepWindowStaticForShaderOpen(PHLWINDOW window) {
    if (!window)
        return;

    window->m_realPosition->setValueAndWarp(window->m_realPosition->goal());
    window->m_realSize->setValueAndWarp(window->m_realSize->goal());
    window->m_alpha->setValueAndWarp(1.F);
    window->m_animatingIn = false;
    setMoveAnimationConfig(window);

    g_pHyprRenderer->damageWindow(window, true);
}

void keepWindowStaticForShaderClose(PHLWINDOW window) {
    if (!window)
        return;

    window->m_realPosition->setValueAndWarp(window->m_originalClosedPos);
    window->m_realSize->setValueAndWarp(window->m_originalClosedSize);
    window->m_alpha->setValueAndWarp(1.F);
    setMoveAnimationConfig(window);

    g_pHyprRenderer->damageWindow(window, true);
}

void finishShaderClose(PHLWINDOW window) {
    if (!window)
        return;

    window->m_alpha->setValueAndWarp(0.F);

    g_pHyprRenderer->damageWindow(window, true);
}

bool shouldOwnHyprlandWindowAnimation(PHLWINDOW window, CDesktopAnimationManager::eAnimationType type) {
    if (!enabled() || !window)
        return false;

    if (type == CDesktopAnimationManager::ANIMATION_TYPE_IN) {
        if (!shaderFileAvailable(EAnimationKind::OPEN))
            return false;

        attachOpenTransformer(window);
        return hasAnimatedTransformer(window);
    }

    if (!shaderFileAvailable(EAnimationKind::CLOSE))
        return false;

    ensureClosingAnimation(window);
    return g_closing.contains(windowKey(window));
}

void onWindowOpen(PHLWINDOW window) {
    if (!enabled() || !window)
        return;

    if (!shaderFileAvailable(EAnimationKind::OPEN))
        return;

    attachOpenTransformer(window);
}

void onWindowClose(PHLWINDOW window) {
    if (!window)
        return;

    if (!enabled()) {
        g_closing.erase(windowKey(window));
        return;
    }

    ensureClosingAnimation(window);
}

void sweepAnimations() {
    if (!g_pCompositor)
        return;

    for (auto const& window : g_pCompositor->m_windows) {
        if (!window)
            continue;

        auto& transformers = window->m_transformers;
        transformers.erase(std::remove_if(transformers.begin(), transformers.end(), [](const auto& transformer) {
                               if (!g_animatedTransformers.contains(transformer.get()))
                                   return false;

                               auto* animated = static_cast<CWindowShaderTransformer*>(transformer.get());
                               if (!animated->done())
                                   return false;

                               g_animatedTransformers.erase(transformer.get());
                               return true;
                           }),
                           transformers.end());
    }

    for (auto it = g_closing.begin(); it != g_closing.end();) {
        const auto window = it->second.window.lock();
        if (!window || (!window->m_fadingOut && !window->m_isMapped))
            it = g_closing.erase(it);
        else
            ++it;
    }
}

void renderAnimatedSnapshot(void* thisptr, PHLWINDOW window) {
    if (!enabled() || !window) {
        callOriginalRenderSnapshot(thisptr, window);
        return;
    }

    auto it = g_closing.find(windowKey(window));
    if (it == g_closing.end()) {
        callOriginalRenderSnapshot(thisptr, window);
        return;
    }

    const auto monitor = window->m_monitor.lock();
    if (!monitor || !g_pHyprOpenGL->m_windowFramebuffers.contains(PHLWINDOWREF{window})) {
        callOriginalRenderSnapshot(thisptr, window);
        return;
    }

    auto* fbData = &g_pHyprOpenGL->m_windowFramebuffers.at(PHLWINDOWREF{window});
    if (!fbData->getTexture()) {
        callOriginalRenderSnapshot(thisptr, window);
        return;
    }

    auto* shader = shaderFor(EAnimationKind::CLOSE);
    if (!shader) {
        if (it->second.suppressedHyprland) {
            finishShaderClose(window);
            g_closing.erase(it);
        } else {
            g_closing.erase(it);
            callOriginalRenderSnapshot(thisptr, window);
        }
        return;
    }

    const auto cfg         = effectConfig();
    const auto rawProgress = elapsedProgress(it->second.startedAt, cfg);
    const auto progress    = ease(rawProgress, cfg.curve);

    if (rawProgress >= 1.F) {
        finishShaderClose(window);
        g_closing.erase(it);
        return;
    }

    static auto PDIMAROUND = CConfigValue<Hyprlang::FLOAT>("decoration:dim_around");
    if (*PDIMAROUND && window->m_ruleApplicator->dimAround().valueOrDefault()) {
        CRectPassElement::SRectData data;
        data.box   = {0, 0, g_pHyprOpenGL->m_renderData.pMonitor->m_pixelSize.x, g_pHyprOpenGL->m_renderData.pMonitor->m_pixelSize.y};
        data.color = CHyprColor(0, 0, 0, *PDIMAROUND * (1.F - progress));
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(std::move(data)));
    }

    const CBox geometryPx =
        scaledGeometry(CBox{window->m_realPosition->value().x, window->m_realPosition->value().y, window->m_realSize->value().x, window->m_realSize->value().y}, monitor);
    const CBox sourceGeometryPx =
        CBox{window->m_originalClosedPos.x * monitor->m_scale, window->m_originalClosedPos.y * monitor->m_scale, window->m_originalClosedSize.x * monitor->m_scale,
             window->m_originalClosedSize.y * monitor->m_scale};
    const CRegion damage = animationDamageForGeometry(geometryPx, monitor->m_transformedSize);
    if (damage.empty()) {
        finishShaderClose(window);
        g_closing.erase(it);
        return;
    }

    g_pHyprRenderer->m_renderPass.add(makeUnique<CAnimatedShaderPassElement>(fbData->getTexture(), shader, geometryPx, sourceGeometryPx, monitor->m_transformedSize,
                                                                             progress, it->second.seed, damage));
    damageAnimationGeometry(monitor, geometryPx);
}

void hkRenderSnapshot(void* thisptr, PHLWINDOW window) {
    if (g_unloading) {
        callOriginalRenderSnapshot(thisptr, window);
        return;
    }

    renderAnimatedSnapshot(thisptr, window);
}

void hkStartWindowAnimation(void* thisptr, PHLWINDOW window, CDesktopAnimationManager::eAnimationType type, bool force) {
    if (g_unloading || !shouldOwnHyprlandWindowAnimation(window, type)) {
        callOriginalStartWindowAnimation(thisptr, window, type, force);
        return;
    }

    if (type == CDesktopAnimationManager::ANIMATION_TYPE_IN) {
        keepWindowStaticForShaderOpen(window);
        return;
    }

    if (auto it = g_closing.find(windowKey(window)); it != g_closing.end())
        it->second.suppressedHyprland = true;

    keepWindowStaticForShaderClose(window);
}

void hkStartWorkspaceAnimation(void* thisptr, PHLWORKSPACE workspace, CDesktopAnimationManager::eAnimationType type, bool left, bool instant) {
    if (g_unloading || !workspaceSwitchEnabled() || !workspace || workspace->m_isSpecialWorkspace || !shaderFileAvailable(EAnimationKind::OPEN) ||
        !shaderFileAvailable(EAnimationKind::CLOSE)) {
        callOriginalStartWorkspaceAnimation(thisptr, workspace, type, left, instant);
        return;
    }

    const auto monitor = workspace->m_monitor.lock();
    if (!monitor || monitor->isMirror()) {
        callOriginalStartWorkspaceAnimation(thisptr, workspace, type, left, instant);
        return;
    }

    callOriginalStartWorkspaceAnimation(thisptr, workspace, type, left, true);

    if (type == CDesktopAnimationManager::ANIMATION_TYPE_OUT) {
        g_pendingWorkspaceSwitchFrom[monitor->m_id] = PHLWORKSPACEREF{workspace};
        forceWorkspaceInstant(workspace, false);
        g_pHyprRenderer->damageMonitor(monitor);
        return;
    }

    PHLWORKSPACE fromWorkspace = nullptr;
    if (auto it = g_pendingWorkspaceSwitchFrom.find(monitor->m_id); it != g_pendingWorkspaceSwitchFrom.end()) {
        fromWorkspace = it->second.lock();
        g_pendingWorkspaceSwitchFrom.erase(it);
    }

    if (!fromWorkspace)
        fromWorkspace = monitorShaderState(monitor).activeWorkspace.lock();

    startWorkspaceSwitchAnimation(workspace, fromWorkspace);
    forceWorkspaceInstant(workspace, true);
    g_pHyprRenderer->damageMonitor(monitor);
}

void removeAnimatedTransformers() {
    if (!g_pCompositor)
        return;

    for (auto const& window : g_pCompositor->m_windows) {
        if (!window)
            continue;

        auto& transformers = window->m_transformers;
        transformers.erase(std::remove_if(transformers.begin(), transformers.end(), [](const auto& transformer) {
                               if (!g_animatedTransformers.contains(transformer.get()))
                                   return false;

                               g_animatedTransformers.erase(transformer.get());
                               return true;
                           }),
                           transformers.end());
    }
}

void destroyPluginState() {
    g_unloading = true;
    finishAllWorkspaceSwitches();
    restoreAnimationConfigs();

    if (g_pHyprRenderer)
        g_pHyprRenderer->makeEGLCurrent();

    removeAnimatedTransformers();
    g_animatedTransformers.clear();
    g_closing.clear();
    g_monitorShaderStates.clear();
    g_pendingWorkspaceSwitchFrom.clear();
    g_workspaceSwitches.clear();
    g_listeners.clear();

    g_shaders.clear();
    g_reloadShaders = false;
    g_effectConfig.reset();
    g_effectConfigKey.clear();
}

PLUGIN_DESCRIPTION_INFO pluginInitImpl(HANDLE handle) {
    PHANDLE     = handle;
    g_unloading = false;

    const std::string compositorHash = __hyprland_api_get_hash();
    const std::string clientHash     = __hyprland_api_get_client_hash();
    if (compositorHash != clientHash) {
        HyprlandAPI::addNotification(PHANDLE, "[hypranimated] failed to load, version mismatch", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error(std::format("version mismatch, built against {}, running {}", clientHash, compositorHash));
    }

    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"enabled"}, Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"effect"}, Hyprlang::STRING{"fade"});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"shaders_dir"}, Hyprlang::STRING{DEFAULT_SHADERS});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"duration_ms"}, Hyprlang::INT{350});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"workspace_switch"}, Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"sync_hyprland"}, Hyprlang::INT{1});
    HyprlandAPI::reloadConfig();
    refreshConfigPtrs();
    applyAnimationOverrides();
    rememberActiveWorkspacesForAllMonitors();

    g_listeners.emplace_back(Event::bus()->m_events.window.open.listen([](PHLWINDOW window) { onWindowOpen(window); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.close.listen([](PHLWINDOW window) { onWindowClose(window); }));
    g_listeners.emplace_back(Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE workspace) { startWorkspaceSwitchAnimation(workspace); }));
    g_listeners.emplace_back(Event::bus()->m_events.config.reloaded.listen([] {
        refreshConfigPtrs();
        g_reloadShaders = true;
        g_effectConfig.reset();
        g_effectConfigKey.clear();
        g_animationBackups.clear();
        applyAnimationOverrides();
        rememberActiveWorkspacesForAllMonitors();
        if (!workspaceSwitchEnabled())
            finishAllWorkspaceSwitches();
    }));
    g_listeners.emplace_back(Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage == RENDER_PRE_WINDOWS) {
            detectWorkspaceSwitchForCurrentMonitor();
            sweepAnimations();
            sweepWorkspaceSwitches();
        } else if (stage == RENDER_POST_WINDOWS) {
            sweepAnimations();
            sweepWorkspaceSwitches();
            renderWorkspaceSwitchForCurrentMonitor();
        }
    }));

    try {
        g_pRenderSnapshotHook = hook(
            pmfAddress(static_cast<void (CHyprRenderer::*)(PHLWINDOW)>(&CHyprRenderer::renderSnapshot)),
            "_ZN13CHyprRenderer14renderSnapshotEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEE",
            reinterpret_cast<void*>(&hkRenderSnapshot));
        g_pStartWindowAnimationHook = hook(
            pmfAddress(static_cast<void (CDesktopAnimationManager::*)(PHLWINDOW, CDesktopAnimationManager::eAnimationType, bool)>(&CDesktopAnimationManager::startAnimation)),
            "_ZN24CDesktopAnimationManager14startAnimationEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEENS_14eAnimationTypeEb",
            reinterpret_cast<void*>(&hkStartWindowAnimation));
        g_pStartWorkspaceAnimationHook = hook(
            pmfAddress(static_cast<void (CDesktopAnimationManager::*)(PHLWORKSPACE, CDesktopAnimationManager::eAnimationType, bool, bool)>(&CDesktopAnimationManager::startAnimation)),
            "_ZN24CDesktopAnimationManager14startAnimationEN9Hyprutils6Memory14CSharedPointerI10CWorkspaceEENS_14eAnimationTypeEbb",
            reinterpret_cast<void*>(&hkStartWorkspaceAnimation));
    } catch (const std::exception& e) {
        if (g_pStartWorkspaceAnimationHook) {
            HyprlandAPI::removeFunctionHook(PHANDLE, g_pStartWorkspaceAnimationHook);
            g_pStartWorkspaceAnimationHook = nullptr;
        }
        if (g_pStartWindowAnimationHook) {
            HyprlandAPI::removeFunctionHook(PHANDLE, g_pStartWindowAnimationHook);
            g_pStartWindowAnimationHook = nullptr;
        }
        if (g_pRenderSnapshotHook) {
            HyprlandAPI::removeFunctionHook(PHANDLE, g_pRenderSnapshotHook);
            g_pRenderSnapshotHook = nullptr;
        }
        destroyPluginState();
        HyprlandAPI::addNotification(PHANDLE, std::format("[hypranimated] cannot load: {}", e.what()), CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw;
    }

    return {"hypranimated", "Niri-style shader window animations for Hyprland", "jppan", "1.0"};
}

void pluginExitImpl() {
    if (g_pStartWorkspaceAnimationHook) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pStartWorkspaceAnimationHook);
        g_pStartWorkspaceAnimationHook = nullptr;
    }

    if (g_pStartWindowAnimationHook) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pStartWindowAnimationHook);
        g_pStartWindowAnimationHook = nullptr;
    }

    if (g_pRenderSnapshotHook) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pRenderSnapshotHook);
        g_pRenderSnapshotHook = nullptr;
    }

    destroyPluginState();
}

std::string pluginAPIVersionImpl() {
    return HYPRLAND_API_VERSION;
}

} // namespace

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    return pluginInitImpl(handle);
}

APICALL EXPORT void PLUGIN_EXIT() {
    pluginExitImpl();
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return pluginAPIVersionImpl();
}
