#include "hypranimated.hpp"

// hyprland 0.54.3 does not export this key function, but the plugin needs the
// interface typeinfo for a derived transformer. keep this equivalent to hyprland's no-op.
void IWindowTransformer::preWindowRender(CSurfacePassElement::SRenderData*) {
}

namespace hypranimated {

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

} // namespace hypranimated

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    return hypranimated::pluginInitImpl(handle);
}

APICALL EXPORT void PLUGIN_EXIT() {
    hypranimated::pluginExitImpl();
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return hypranimated::pluginAPIVersionImpl();
}
