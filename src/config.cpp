#include "hypranimated.hpp"

namespace hypranimated {
namespace {

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

int configInt(const std::string& name, int fallback) {
    const auto* ptr = configPtr<Hyprlang::INT>(name);
    return ptr ? static_cast<int>(*ptr) : fallback;
}

std::string configString(const std::string& name, std::string fallback) {
    const auto* ptr = configStringPtr(name);
    if (!ptr)
        return fallback;

    auto value = trim(*ptr);
    return value.empty() ? fallback : value;
}

SEffectConfig readEffectConfig() {
    SEffectConfig out;

    const int configuredDuration = configInt("duration_ms", 350);
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

        // duration_ms <= 0 lets the effect config file provide the duration.
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
    const int configuredDuration = configInt("duration_ms", 350);
    return std::format("{}\n{}\n{}", shadersDir().string(), effectName(), configuredDuration);
}

} // namespace

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

void refreshConfigPtrs() {
    g_config.enabled         = configPtr<Hyprlang::INT>("enabled");
    g_config.effect          = configStringPtr("effect");
    g_config.shadersDir      = configStringPtr("shaders_dir");
    g_config.durationMs      = configPtr<Hyprlang::INT>("duration_ms");
    g_config.workspaceSwitch = configPtr<Hyprlang::INT>("workspace_switch");
    g_config.syncHyprland    = configPtr<Hyprlang::INT>("sync_hyprland");
}

bool enabled() {
    const int cfgEnabled = configInt("enabled", 1);
    return !g_unloading && cfgEnabled && g_pHyprRenderer && g_pHyprOpenGL;
}

bool workspaceSwitchEnabled() {
    return enabled() && configInt("workspace_switch", 0);
}

bool syncHyprlandEnabled() {
    return enabled() && configInt("sync_hyprland", 1);
}

std::string effectName() {
    return configString("effect", "fade");
}

std::filesystem::path shadersDir() {
    return std::filesystem::path{configString("shaders_dir", DEFAULT_SHADERS)};
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

} // namespace hypranimated
