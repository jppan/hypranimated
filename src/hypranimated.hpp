#pragma once

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
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprlang.hpp>

#include <GLES3/gl32.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hypranimated {

inline constexpr auto CONFIG_NS       = "plugin:hypranimated:";
inline constexpr auto DEFAULT_SHADERS = "/home/jppan/.local/bin/jpOSh/shaders";
inline constexpr DRMFormat ANIMATION_FB_FORMAT = DRM_FORMAT_ARGB8888;

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
    PHLWINDOWREF                                             window;
    std::optional<std::chrono::steady_clock::time_point>     startedAt;
    float                                                    seed = 0.F;
    bool                                                     suppressedHyprland = false;
};

struct SMonitorShaderState {
    PHLMONITORREF   monitor;
    PHLWORKSPACEREF activeWorkspace;
};

struct SWindowRenderTarget {
    CFramebuffer sourceFramebuffer;
    CFramebuffer passthroughFramebuffer;
};

struct SWorkspaceSwitchRenderItem {
    SP<SWindowRenderTarget> renderTarget;
    CBox                    geometryPx;
    CRegion                 damage;
    float                   blurAlpha         = 0.F;
    int                     blurRound         = 0;
    float                   blurRoundingPower = 2.F;
};

struct SWorkspaceSwitchRenderState {
    PHLMONITORREF                         monitor;
    PHLWORKSPACEREF                       fromWorkspace;
    PHLWORKSPACEREF                       toWorkspace;
    std::chrono::steady_clock::time_point startedAt;
    SEffectConfig                         cfg;
    std::vector<SWorkspaceSwitchRenderItem> renderItems;
    EAnimationKind                         kind                  = EAnimationKind::OPEN;
    float                                 seed                  = 0.F;
    bool                                  previousForceRendering = false;
    bool                                  sourceCaptured         = false;
    bool                                  finished              = false;
    bool                                  restored              = false;
};

struct SQueuedClosingRender {
    PHLMONITORREF monitor;
    SP<CTexture>  texture;
    CBox          geometryPx;
    CBox          sourceGeometryPx;
    CRegion       damage;
    float         progress          = 0.F;
    float         seed              = 0.F;
    float         dimAlpha          = 0.F;
    float         blurAlpha         = 0.F;
    int           blurRound         = 0;
    float         blurRoundingPower = 2.F;
};

struct SAnimationConfigBackup {
    SP<Hyprutils::Animation::SAnimationPropertyConfig> node;
    Hyprutils::Animation::SAnimationPropertyConfig     value;
};

extern HANDLE PHANDLE;
extern CFunctionHook* g_pRenderSnapshotHook;
extern CFunctionHook* g_pStartWindowAnimationHook;
extern CFunctionHook* g_pStartWorkspaceAnimationHook;
extern bool g_unloading;

extern SConfig g_config;
extern std::vector<Hyprutils::Signal::CHyprSignalListener> g_listeners;
extern std::unordered_map<uintptr_t, SClosingAnimation> g_closing;
extern std::vector<SQueuedClosingRender> g_queuedClosingRenders;
extern std::unordered_map<MONITORID, UP<SMonitorShaderState>> g_monitorShaderStates;
extern std::unordered_map<MONITORID, PHLWORKSPACEREF> g_pendingWorkspaceSwitchFrom;
extern std::unordered_map<MONITORID, bool> g_pendingWorkspaceForceRendering;
extern std::vector<SP<SWorkspaceSwitchRenderState>> g_workspaceSwitches;
extern std::unordered_set<IWindowTransformer*> g_animatedTransformers;
extern std::unordered_map<std::string, SAnimationConfigBackup> g_animationBackups;
extern bool g_reloadShaders;
extern std::optional<SEffectConfig> g_effectConfig;
extern std::string g_effectConfigKey;

std::string trim(std::string value);
std::string stripQuotes(std::string value);
std::string lower(std::string value);
void refreshConfigPtrs();
bool enabled();
bool workspaceSwitchEnabled();
bool syncHyprlandEnabled();
std::string effectName();
std::filesystem::path shadersDir();
std::string kindName(EAnimationKind kind);
std::string shaderFunction(EAnimationKind kind);
std::filesystem::path shaderPath(EAnimationKind kind);
bool shaderFileAvailable(EAnimationKind kind);
std::optional<std::string> readFile(const std::filesystem::path& path);
const SEffectConfig& effectConfig();
float ease(float progress, std::string curve);
float elapsedProgress(const std::chrono::steady_clock::time_point& startedAt, const SEffectConfig& cfg);
float elapsedProgress(std::optional<std::chrono::steady_clock::time_point>& startedAt, const SEffectConfig& cfg);
float randomSeed(uintptr_t value, EAnimationKind kind);

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

CFunctionHook* hook(void* target, const std::string& signature, void* handler);
void callOriginalRenderSnapshot(void* thisptr, PHLWINDOW window);
void callOriginalStartWindowAnimation(void* thisptr, PHLWINDOW window, CDesktopAnimationManager::eAnimationType type, bool force);
void callOriginalStartWorkspaceAnimation(void* thisptr, PHLWORKSPACE workspace, CDesktopAnimationManager::eAnimationType type, bool left, bool instant);

class CAnimationShader {
  public:
    CAnimationShader(EAnimationKind kind, std::filesystem::path path, std::string userSource);
    ~CAnimationShader();

    bool valid() const;
    void render(SP<CTexture> texture, const CBox& geometryPx, const CBox& sourceGeometryPx, const Vector2D& monitorSize, float progress, float seed,
                float outputAlpha, const CRegion& damage);

  private:
    EAnimationKind        m_kind;
    std::filesystem::path m_path;
    GLuint                m_program = 0;
    GLuint                m_vao     = 0;
    GLuint                m_vbo     = 0;

    GLint m_uniformProj            = -1;
    GLint m_uniformTex             = -1;
    GLint m_uniformGeoToTex        = -1;
    GLint m_uniformProgress        = -1;
    GLint m_uniformClampedProgress = -1;
    GLint m_uniformSeed            = -1;
    GLint m_uniformMonitorSize     = -1;
    GLint m_uniformGeometry        = -1;
    GLint m_uniformOutputAlpha     = -1;

    void create(std::string userSource);
    void destroy();
};

CAnimationShader* shaderFor(EAnimationKind kind);
void clearShaderCache();

CBox scaledGeometry(const CBox& logicalBox, PHLMONITOR monitor);
CBox expandedWindowGeometry(const CBox& logicalBox, PHLWINDOW window);
CBox expandedScaledGeometry(const CBox& logicalBox, PHLMONITOR monitor, PHLWINDOW window);
bool ensureFramebuffer(CFramebuffer& fb, const Vector2D& size, DRMFormat format);
CRegion animationDamageForGeometry(const CBox& geometryPx, const Vector2D& monitorSize);
void damageAnimationGeometry(PHLMONITOR monitor, const CBox& geometryPx);
void forceCurrentRenderDamage(PHLMONITOR monitor, const CRegion& damage);
UP<IPassElement> makeAnimatedShaderPassElement(SP<CTexture> texture, CAnimationShader* shader, CBox geometryPx, CBox sourceGeometryPx, Vector2D monitorSize,
                                               float progress, float seed, float outputAlpha, CRegion damage);
UP<IPassElement> makeAnimatedShaderPassElement(CFramebuffer* liveSourceFramebuffer, CAnimationShader* shader, CBox geometryPx, CBox sourceGeometryPx,
                                               Vector2D monitorSize, float progress, float seed, float outputAlpha, CRegion damage);
UP<IPassElement> makeAnimatedBlurPassElement(CBox geometryPx, Vector2D monitorSize, float alpha, int round, float roundingPower, CRegion damage);
UP<IPassElement> makeBindOffMainPassElement(SP<SWindowRenderTarget> renderTarget = {}, bool clear = true);

class CWindowShaderTransformer : public IWindowTransformer {
  public:
    explicit CWindowShaderTransformer(PHLWINDOW window);
    CWindowShaderTransformer(PHLWINDOW window, EAnimationKind kind, SP<SWorkspaceSwitchRenderState> workspaceSwitch);

    void preWindowRender(CSurfacePassElement::SRenderData* renderData) override;
    CFramebuffer* transform(CFramebuffer* in) override;
    bool done() const;
    SP<SWorkspaceSwitchRenderState> workspaceSwitch() const;

  private:
    PHLWINDOWREF                                             m_window;
    PHLMONITORREF                                           m_monitor;
    std::optional<std::chrono::steady_clock::time_point>    m_startedAt;
    EAnimationKind                                           m_kind = EAnimationKind::OPEN;
    SP<SWorkspaceSwitchRenderState>                          m_workspaceSwitch;
    SP<SWindowRenderTarget>                                  m_renderTarget;
    float                                                    m_seed = 0.F;
    SEffectConfig                                            m_cfg;
    CAnimationShader*                                        m_shader = nullptr;
    CBox                                                     m_geometry;
    bool                                                     m_done = false;
    bool                                                     m_shouldBlur = false;
    int                                                      m_blurRound = 0;
    float                                                    m_blurRoundingPower = 2.F;
    float                                                    m_outputAlpha = 1.F;

    CFramebuffer* clearPassthroughFramebuffer(PHLMONITOR monitor);
    CFramebuffer* transparentHandoffFramebuffer(PHLMONITOR monitor, CFramebuffer* fallback);
    float rawAnimationProgress();
    bool animationComplete();
    bool animationCompleteByClock() const;
};

void applyAnimationOverrides();
void restoreAnimationConfigs();
void finishAllWorkspaceSwitches();
void rememberActiveWorkspacesForAllMonitors();
PHLWORKSPACE rememberedActiveWorkspace(PHLMONITOR monitor);
void startWorkspaceSwitchAnimation(PHLWORKSPACE workspace, PHLWORKSPACE fromWorkspaceOverride = nullptr, bool forceSameWorkspace = false);
void sweepWorkspaceSwitches();
void renderWorkspaceSwitchForCurrentMonitor();
void renderQueuedClosingAnimationsForCurrentMonitor();
void detectWorkspaceSwitchForCurrentMonitor();
bool hasAnimatedTransformer(PHLWINDOW window);
void onWindowOpen(PHLWINDOW window);
void onWindowClose(PHLWINDOW window);
void onWindowMoveToWorkspace(PHLWINDOW window, PHLWORKSPACE workspace);
void sweepAnimations();
void renderAnimatedSnapshot(void* thisptr, PHLWINDOW window);
bool shouldOwnHyprlandWindowAnimation(PHLWINDOW window, CDesktopAnimationManager::eAnimationType type);
void keepWindowStaticForShaderOpen(PHLWINDOW window);
void keepWindowStaticForShaderClose(PHLWINDOW window);
void removeAnimatedTransformers();
void destroyPluginState();

void hkRenderSnapshot(void* thisptr, PHLWINDOW window);
void hkStartWindowAnimation(void* thisptr, PHLWINDOW window, CDesktopAnimationManager::eAnimationType type, bool force);
void hkStartWorkspaceAnimation(void* thisptr, PHLWORKSPACE workspace, CDesktopAnimationManager::eAnimationType type, bool left, bool instant);

} // namespace hypranimated
