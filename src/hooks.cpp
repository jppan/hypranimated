#include "hypranimated.hpp"

namespace hypranimated {
namespace {

using origRenderSnapshot = void (*)(void*, PHLWINDOW);
using origStartWindowAnimation = void (*)(void*, PHLWINDOW, CDesktopAnimationManager::eAnimationType, bool);
using origStartWorkspaceAnimation = void (*)(void*, PHLWORKSPACE, CDesktopAnimationManager::eAnimationType, bool, bool);

} // namespace

CFunctionHook* hook(void* target, const std::string& signature, void* handler) {
    Dl_info info = {};
    if (!dladdr(target, &info))
        throw std::runtime_error("symbol not available");

#ifdef __GLIBCXX__
    // verify the mangled target so a hyprland signature drift fails loudly.
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

    if (auto it = g_closing.find(reinterpret_cast<uintptr_t>(window.get())); it != g_closing.end())
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

    // ask hyprland to settle the workspace immediately; this plugin renders the visible transition.
    callOriginalStartWorkspaceAnimation(thisptr, workspace, type, left, true);

    if (type == CDesktopAnimationManager::ANIMATION_TYPE_OUT) {
        g_pendingWorkspaceSwitchFrom[monitor->m_id] = PHLWORKSPACEREF{workspace};
        workspace->m_renderOffset->setValueAndWarp(Vector2D{0.F, 0.F});
        workspace->m_alpha->setValueAndWarp(0.F);
        g_pHyprRenderer->damageMonitor(monitor);
        return;
    }

    PHLWORKSPACE fromWorkspace = nullptr;
    if (auto it = g_pendingWorkspaceSwitchFrom.find(monitor->m_id); it != g_pendingWorkspaceSwitchFrom.end()) {
        fromWorkspace = it->second.lock();
        g_pendingWorkspaceSwitchFrom.erase(it);
    }

    if (!fromWorkspace)
        fromWorkspace = rememberedActiveWorkspace(monitor);

    startWorkspaceSwitchAnimation(workspace, fromWorkspace);
    workspace->m_renderOffset->setValueAndWarp(Vector2D{0.F, 0.F});
    workspace->m_alpha->setValueAndWarp(1.F);
    g_pHyprRenderer->damageMonitor(monitor);
}

} // namespace hypranimated
