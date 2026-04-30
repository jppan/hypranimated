#include "hypranimated.hpp"

namespace hypranimated {
namespace {

void backupAnimationConfig(const std::string& name, SP<Hyprutils::Animation::SAnimationPropertyConfig> node) {
    if (!node || g_animationBackups.contains(name))
        return;

    g_animationBackups.emplace(name, SAnimationConfigBackup{.node = node, .value = *node});
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

void forceWorkspaceInstant(PHLWORKSPACE workspace, bool visible);

void restoreWorkspaceSwitchState(const SP<SWorkspaceSwitchRenderState>& state) {
    if (!state || state->restored)
        return;

    if (const auto fromWorkspace = state->fromWorkspace.lock(); fromWorkspace && !fromWorkspace->inert()) {
        fromWorkspace->m_forceRendering = state->previousForceRendering;
        if (!fromWorkspace->isVisible())
            forceWorkspaceInstant(fromWorkspace, false);
    }

    state->restored = true;
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

    if (!window->m_isMapped || window->m_fadingOut || window->m_readyToDelete || window->m_pinned || window->isHidden())
        return false;

    return true;
}

bool shouldBlurWindow(PHLWINDOW window) {
    if (!window)
        return false;

    static auto PBLUR = CConfigValue<Hyprlang::INT>("decoration:blur:enabled");
    const bool  dontBlur =
        window->m_ruleApplicator->noBlur().valueOrDefault() || window->m_ruleApplicator->RGBX().valueOrDefault() || window->opaque();
    return *PBLUR && !dontBlur;
}

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

SP<SWorkspaceSwitchRenderState> workspaceSwitchFor(PHLMONITOR monitor, PHLWORKSPACE workspace, std::optional<EAnimationKind> kind = std::nullopt) {
    if (!monitor || !workspace)
        return {};

    for (auto const& state : g_workspaceSwitches) {
        const auto stateMonitor = state ? state->monitor.lock() : nullptr;
        const auto toWorkspace  = state ? state->toWorkspace.lock() : nullptr;
        if (state && !state->finished && (!kind || state->kind == *kind) && stateMonitor && stateMonitor->m_id == monitor->m_id && toWorkspace &&
            toWorkspace.get() == workspace.get())
            return state;
    }

    return {};
}

void removeAnimatedTransformersForWindow(PHLWINDOW window) {
    if (!window)
        return;

    auto& transformers = window->m_transformers;
    transformers.erase(std::remove_if(transformers.begin(), transformers.end(), [](const auto& transformer) {
                           if (!g_animatedTransformers.contains(transformer.get()))
                               return false;

                           g_animatedTransformers.erase(transformer.get());
                           return true;
                       }),
                       transformers.end());
}

bool attachWorkspaceTransformer(PHLWINDOW window, EAnimationKind kind, const SP<SWorkspaceSwitchRenderState>& state) {
    if (!window || !state || hasAnimatedTransformer(window))
        return false;

    auto transformer = makeUnique<CWindowShaderTransformer>(window, kind, state);
    g_animatedTransformers.insert(transformer.get());
    window->m_transformers.emplace_back(std::move(transformer));
    return true;
}

uintptr_t windowKey(PHLWINDOW window) {
    return reinterpret_cast<uintptr_t>(window.get());
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

void finishShaderClose(PHLWINDOW window) {
    if (!window)
        return;

    window->m_alpha->setValueAndWarp(0.F);

    g_pHyprRenderer->damageWindow(window, true);
}

} // namespace

void restoreAnimationConfigs() {
    for (auto& [_, backup] : g_animationBackups) {
        if (backup.node)
            *backup.node = backup.value;
    }

    g_animationBackups.clear();
}

void applyAnimationOverrides() {
    syncWindowsMoveConfig();
    disableWorkspaceSlideAnimations();
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

void rememberActiveWorkspacesForAllMonitors() {
    if (!g_pCompositor)
        return;

    for (auto const& monitor : g_pCompositor->m_monitors) {
        if (!monitor || monitor->isMirror() || !monitor->m_activeWorkspace)
            continue;

        monitorShaderState(monitor).activeWorkspace = monitor->m_activeWorkspace;
    }
}

PHLWORKSPACE rememberedActiveWorkspace(PHLMONITOR monitor) {
    if (!monitor)
        return nullptr;

    if (auto it = g_monitorShaderStates.find(monitor->m_id); it != g_monitorShaderStates.end() && it->second)
        return it->second->activeWorkspace.lock();

    return nullptr;
}

void startWorkspaceSwitchAnimation(PHLWORKSPACE workspace, PHLWORKSPACE fromWorkspaceOverride, bool forceSameWorkspace) {
    if (!workspaceSwitchEnabled() || !workspace || workspace->m_isSpecialWorkspace)
        return;

    sweepAnimations();

    const auto monitor = workspace->m_monitor.lock();
    if (!monitor || monitor->isMirror())
        return;

    auto& state = monitorShaderState(monitor);
    auto fromWorkspace     = fromWorkspaceOverride ? fromWorkspaceOverride : state.activeWorkspace.lock();
    state.activeWorkspace  = workspace;
    const bool sameWorkspace = fromWorkspace && fromWorkspace.get() == workspace.get();
    if (sameWorkspace && !forceSameWorkspace)
        return;
    if (sameWorkspace)
        fromWorkspace.reset();

    if (fromWorkspace && fromWorkspace->m_isSpecialWorkspace)
        fromWorkspace.reset();

    std::vector<PHLWINDOW> windows;
    if (g_pCompositor) {
        for (auto const& window : g_pCompositor->m_windows) {
            if (canAnimateWorkspaceWindow(window, workspace))
                windows.emplace_back(window);
        }
    }

    auto           kind = EAnimationKind::OPEN;
    PHLWORKSPACE   captureWorkspace = workspace;
    const bool     destinationEmpty = windows.empty();
    if (destinationEmpty) {
        if (!fromWorkspace || !shaderFileAvailable(EAnimationKind::CLOSE))
            return;

        kind             = EAnimationKind::CLOSE;
        captureWorkspace = fromWorkspace;
        if (g_pCompositor) {
            for (auto const& window : g_pCompositor->m_windows) {
                if (canAnimateWorkspaceWindow(window, captureWorkspace))
                    windows.emplace_back(window);
            }
        }
    } else if (!shaderFileAvailable(EAnimationKind::OPEN)) {
        return;
    }

    if (windows.empty() || workspaceSwitchFor(monitor, workspace, kind))
        return;

    // Workspace switches queue each captured window for post-window shader rendering.
    const bool previousForceRendering = fromWorkspace ? fromWorkspace->m_forceRendering : false;
    cancelWorkspaceSwitchesForMonitor(monitor);
    forceWorkspaceInstant(workspace, true);
    if (kind == EAnimationKind::CLOSE && captureWorkspace) {
        forceWorkspaceInstant(captureWorkspace, true);
        captureWorkspace->m_forceRendering = true;
    }

    const auto fromWorkspaceId = fromWorkspace ? fromWorkspace->m_id : WORKSPACE_INVALID;
    const auto seedKey = (static_cast<uintptr_t>(monitor->m_id) << 32U) ^ static_cast<uintptr_t>(fromWorkspaceId) ^
        (static_cast<uintptr_t>(workspace->m_id) << 1U);
    auto switchState = makeShared<SWorkspaceSwitchRenderState>();
    switchState->monitor                = PHLMONITORREF{monitor};
    switchState->fromWorkspace          = fromWorkspace ? PHLWORKSPACEREF{fromWorkspace} : PHLWORKSPACEREF{};
    switchState->toWorkspace            = PHLWORKSPACEREF{workspace};
    switchState->startedAt              = std::chrono::steady_clock::now();
    switchState->cfg                    = effectConfig();
    switchState->kind                   = kind;
    switchState->seed                   = randomSeed(seedKey, kind);
    switchState->previousForceRendering = previousForceRendering;

    size_t attached = 0;
    for (auto const& window : windows) {
        if (attachWorkspaceTransformer(window, kind, switchState))
            ++attached;
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
    if (!g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.pMonitor) {
        return;
    }

    const auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!monitor || monitor->isMirror()) {
        return;
    }

    for (auto& state : g_workspaceSwitches) {
        const auto stateMonitor = state ? state->monitor.lock() : nullptr;
        if (!state || state->finished || !stateMonitor || stateMonitor->m_id != monitor->m_id)
            continue;

        if (!state->sourceCaptured || state->renderItems.empty())
            continue;

        auto* shader = shaderFor(state->kind);
        if (!shader) {
            state->finished = true;
            restoreWorkspaceSwitchState(state);
            continue;
        }

        const float rawProgress = elapsedProgress(state->startedAt, state->cfg);
        if (rawProgress >= 1.F) {
            state->finished = true;
            restoreWorkspaceSwitchState(state);
            continue;
        }

        const auto  monitorSize = monitor->m_transformedSize;
        const CBox  geometryPx  = {0, 0, monitorSize.x, monitorSize.y};
        const auto  damage      = animationDamageForGeometry(geometryPx, monitorSize);
        const float progress    = ease(rawProgress, state->cfg.curve);
        bool        rendered    = false;

        if (!damage.empty()) {
            for (auto const& item : state->renderItems) {
                if (!item.renderTarget || !item.renderTarget->sourceFramebuffer.getTexture())
                    continue;

                if (item.blurAlpha > 0.001F && !item.damage.empty())
                    g_pHyprRenderer->m_renderPass.add(
                        makeAnimatedBlurPassElement(item.geometryPx, monitorSize, item.blurAlpha, item.blurRound, item.blurRoundingPower, item.damage));

                g_pHyprRenderer->m_renderPass.add(makeAnimatedShaderPassElement(item.renderTarget->sourceFramebuffer.getTexture(), shader, geometryPx, geometryPx,
                                                                                monitorSize, progress, state->seed, 1.F, damage));
                rendered = true;
            }
        }

        if (rendered)
            damageAnimationGeometry(monitor, geometryPx);

        state->sourceCaptured = false;
        state->renderItems.clear();
    }

    rememberActiveWorkspaceForCurrentMonitor();
}

void renderQueuedClosingAnimationsForCurrentMonitor() {
    if (!g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.pMonitor)
        return;

    const auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!monitor || monitor->isMirror())
        return;

    auto* shader = shaderFor(EAnimationKind::CLOSE);
    for (auto it = g_queuedClosingRenders.begin(); it != g_queuedClosingRenders.end();) {
        const auto itemMonitor = it->monitor.lock();
        if (!itemMonitor) {
            it = g_queuedClosingRenders.erase(it);
            continue;
        }

        if (itemMonitor->m_id != monitor->m_id) {
            ++it;
            continue;
        }

        if (shader && it->texture && !it->damage.empty()) {
            if (it->dimAlpha > 0.F) {
                CRectPassElement::SRectData data;
                data.box   = {0, 0, monitor->m_pixelSize.x, monitor->m_pixelSize.y};
                data.color = CHyprColor(0, 0, 0, it->dimAlpha);
                g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(std::move(data)));
            }

            if (it->blurAlpha > 0.001F)
                g_pHyprRenderer->m_renderPass.add(
                    makeAnimatedBlurPassElement(it->geometryPx, monitor->m_transformedSize, it->blurAlpha, it->blurRound, it->blurRoundingPower, it->damage));

            g_pHyprRenderer->m_renderPass.add(makeAnimatedShaderPassElement(it->texture, shader, it->geometryPx, it->sourceGeometryPx, monitor->m_transformedSize,
                                                                            it->progress, it->seed, 1.F, it->damage));
            damageAnimationGeometry(monitor, it->geometryPx);
        }

        it = g_queuedClosingRenders.erase(it);
    }
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

void onWindowMoveToWorkspace(PHLWINDOW window, PHLWORKSPACE workspace) {
    if (!window)
        return;

    g_closing.erase(windowKey(window));

    if (!workspaceSwitchEnabled() || !workspace || workspace->m_isSpecialWorkspace)
        return;

    const auto monitor = workspace->m_monitor.lock();
    if (!monitor || monitor->isMirror() || !monitor->m_activeWorkspace || monitor->m_activeWorkspace.get() != workspace.get())
        return;

    auto state = workspaceSwitchFor(monitor, workspace, EAnimationKind::OPEN);
    if (!state) {
        if (canAnimateWorkspaceWindow(window, workspace))
            removeAnimatedTransformersForWindow(window);
        startWorkspaceSwitchAnimation(workspace, nullptr, true);
        return;
    }

    if (!canAnimateWorkspaceWindow(window, workspace))
        return;

    removeAnimatedTransformersForWindow(window);
    if (attachWorkspaceTransformer(window, EAnimationKind::OPEN, state))
        g_pHyprRenderer->damageWindow(window, true);
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
    const float dimAlpha = *PDIMAROUND && window->m_ruleApplicator->dimAround().valueOrDefault() ? *PDIMAROUND * (1.F - progress) : 0.F;

    const CBox logicalGeometry = {window->m_realPosition->value().x, window->m_realPosition->value().y, window->m_realSize->value().x, window->m_realSize->value().y};
    const CBox geometryPx      = expandedScaledGeometry(logicalGeometry, monitor, window);
    const CBox sourceGeometryPx = expandedWindowGeometry(CBox{window->m_originalClosedPos.x, window->m_originalClosedPos.y, window->m_originalClosedSize.x,
                                                              window->m_originalClosedSize.y},
                                                         window)
                                      .scale(monitor->m_scale);
    const CRegion damage = animationDamageForGeometry(geometryPx, monitor->m_transformedSize);
    if (damage.empty()) {
        finishShaderClose(window);
        g_closing.erase(it);
        return;
    }

    const float blurAlpha = std::clamp(1.F - progress, 0.F, 1.F);
    const bool  blur      = shouldBlurWindow(window) && blurAlpha > 0.001F;
    const int   round     = blur ? std::max(0, static_cast<int>(std::round(window->rounding() * monitor->m_scale))) : 0;
    g_queuedClosingRenders.emplace_back(SQueuedClosingRender{
        .monitor           = PHLMONITORREF{monitor},
        .texture           = fbData->getTexture(),
        .geometryPx        = geometryPx,
        .sourceGeometryPx  = sourceGeometryPx,
        .damage            = damage,
        .progress          = progress,
        .seed              = it->second.seed,
        .dimAlpha          = dimAlpha,
        .blurAlpha         = blur ? blurAlpha : 0.F,
        .blurRound         = round,
        .blurRoundingPower = window->roundingPower(),
    });
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
    g_queuedClosingRenders.clear();
    g_monitorShaderStates.clear();
    g_pendingWorkspaceSwitchFrom.clear();
    g_workspaceSwitches.clear();
    g_listeners.clear();

    clearShaderCache();
    g_reloadShaders = false;
    g_effectConfig.reset();
    g_effectConfigKey.clear();
}

} // namespace hypranimated
