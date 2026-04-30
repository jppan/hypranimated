#include "hypranimated.hpp"

namespace hypranimated {

CWindowShaderTransformer::CWindowShaderTransformer(PHLWINDOW window) :
    m_window(window),
    m_renderTarget(makeShared<SWindowRenderTarget>()),
    m_seed(randomSeed(reinterpret_cast<uintptr_t>(window.get()), EAnimationKind::OPEN)) {
}

CWindowShaderTransformer::CWindowShaderTransformer(PHLWINDOW window, EAnimationKind kind, SP<SWorkspaceSwitchRenderState> workspaceSwitch) :
    m_window(window),
    m_kind(kind),
    m_workspaceSwitch(std::move(workspaceSwitch)),
    m_renderTarget(makeShared<SWindowRenderTarget>()),
    m_seed(randomSeed(reinterpret_cast<uintptr_t>(window.get()), kind)) {
}

void CWindowShaderTransformer::preWindowRender(CSurfacePassElement::SRenderData* renderData) {
    if (!enabled() || !renderData || !renderData->pMonitor)
        return;

    const auto monitor = renderData->pMonitor.lock();
    if (!monitor)
        return;

    m_cfg      = m_workspaceSwitch ? m_workspaceSwitch->cfg : effectConfig();
    m_shader   = m_workspaceSwitch ? nullptr : shaderFor(m_kind);
    m_monitor  = renderData->pMonitor;
    m_geometry = expandedScaledGeometry(CBox{renderData->pos.x, renderData->pos.y, renderData->w, renderData->h}, monitor, m_window.lock());
    m_shouldBlur        = renderData->blur;
    m_blurRound         = std::max(0, static_cast<int>(std::round(renderData->rounding)));
    m_blurRoundingPower = renderData->roundingPower;
    m_outputAlpha       = std::clamp(renderData->alpha * renderData->fadeAlpha, 0.F, 1.F);

    if (m_workspaceSwitch && (m_workspaceSwitch->finished || (m_kind != EAnimationKind::OPEN && animationComplete()))) {
        m_done = true;
        m_workspaceSwitch->finished = true;
        return;
    }

    if (m_workspaceSwitch) {
        const auto stateMonitor = m_workspaceSwitch->monitor.lock();
        if (!m_renderTarget || !stateMonitor || stateMonitor.get() != monitor.get()) {
            m_done = true;
            return;
        }

        const auto damage = animationDamageForGeometry(m_geometry, monitor->m_transformedSize);
        if (damage.empty()) {
            m_done = true;
            return;
        }

        forceCurrentRenderDamage(monitor, damage);
        g_pHyprRenderer->m_renderPass.add(makeBindOffMainPassElement(m_renderTarget));
        m_workspaceSwitch->sourceCaptured = true;

        renderData->blur           = false;
        renderData->discardMode    = 0;
        renderData->discardOpacity = 0.F;
        return;
    }

    if (m_shader && m_renderTarget) {
        forceCurrentRenderDamage(monitor, animationDamageForGeometry(m_geometry, monitor->m_transformedSize));
        // capture the normal window draw so transform() can replace it with the shader output.
        g_pHyprRenderer->m_renderPass.add(makeBindOffMainPassElement(m_renderTarget));
        renderData->alpha          = 1.F;
        renderData->fadeAlpha      = 1.F;
        renderData->blur           = false;
        renderData->discardMode    = 0;
        renderData->discardOpacity = 0.F;
    } else {
        m_done = true;
    }
}

CFramebuffer* CWindowShaderTransformer::transform(CFramebuffer* in) {
    if (!enabled() || !in || !in->getTexture() || !m_monitor || !m_renderTarget || (!m_workspaceSwitch && !m_shader)) {
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
        return transparentHandoffFramebuffer(monitor, in);
    }

    const float rawProgress = rawAnimationProgress();
    if (m_workspaceSwitch && m_kind != EAnimationKind::OPEN && rawProgress >= 1.F) {
        m_workspaceSwitch->finished = true;
        m_done = true;
        return transparentHandoffFramebuffer(monitor, in);
    }

    if (m_workspaceSwitch) {
        auto* passthrough = clearPassthroughFramebuffer(monitor);
        if (!passthrough) {
            m_done = true;
            return in;
        }

        const float progress    = ease(rawProgress, m_cfg.curve);
        const auto  monitorSize = monitor->m_transformedSize;
        const auto  damage      = animationDamageForGeometry(m_geometry, monitorSize);
        const float blurAlpha   = std::clamp(m_kind == EAnimationKind::OPEN ? progress : 1.F - progress, 0.F, 1.F);
        if (!damage.empty()) {
            m_workspaceSwitch->renderItems.emplace_back(SWorkspaceSwitchRenderItem{
                .renderTarget       = m_renderTarget,
                .geometryPx         = m_geometry,
                .damage             = damage,
                .blurAlpha          = m_shouldBlur ? blurAlpha : 0.F,
                .blurRound          = m_blurRound,
                .blurRoundingPower  = m_blurRoundingPower,
            });
        }

        damageAnimationGeometry(monitor, m_geometry);
        g_pHyprOpenGL->blend(true);

        if (rawProgress >= 1.F)
            m_done = true;

        return passthrough;
    }

    const float progress    = ease(rawProgress, m_cfg.curve);
    const auto  geometry    = m_geometry;
    const auto  monitorSize = monitor->m_transformedSize;
    const auto  seed        = m_seed;
    const auto  damage      = animationDamageForGeometry(geometry, monitorSize);
    const float blurAlpha   = std::clamp(m_kind == EAnimationKind::OPEN ? progress : 1.F - progress, 0.F, 1.F);

    if (damage.empty()) {
        m_done = true;
        return in;
    }

    auto* passthrough = clearPassthroughFramebuffer(monitor);
    if (!passthrough) {
        m_done = true;
        return in;
    }

    if (m_shouldBlur && blurAlpha > 0.001F)
        g_pHyprRenderer->m_renderPass.add(makeAnimatedBlurPassElement(geometry, monitorSize, blurAlpha, m_blurRound, m_blurRoundingPower, damage));

    g_pHyprRenderer->m_renderPass.add(
        makeAnimatedShaderPassElement(&m_renderTarget->sourceFramebuffer, m_shader, geometry, geometry, monitorSize, progress, seed, m_outputAlpha, damage));

    damageAnimationGeometry(monitor, geometry);
    g_pHyprOpenGL->blend(true);

    if (rawProgress >= 1.F)
        m_done = true;

    return passthrough;
}

bool CWindowShaderTransformer::done() const {
    if (m_workspaceSwitch && m_kind == EAnimationKind::OPEN)
        return m_done || !enabled() || !m_window.lock() || m_workspaceSwitch->finished;

    if (m_workspaceSwitch)
        return m_done || !enabled() || !m_window.lock() || animationCompleteByClock();

    return m_done || !enabled() || !m_window.lock();
}

SP<SWorkspaceSwitchRenderState> CWindowShaderTransformer::workspaceSwitch() const {
    return m_workspaceSwitch;
}

CFramebuffer* CWindowShaderTransformer::clearPassthroughFramebuffer(PHLMONITOR monitor) {
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

CFramebuffer* CWindowShaderTransformer::transparentHandoffFramebuffer(PHLMONITOR monitor, CFramebuffer* fallback) {
    auto* passthrough = clearPassthroughFramebuffer(monitor);
    if (passthrough) {
        g_pHyprOpenGL->blend(true);
        return passthrough;
    }

    if (g_pHyprOpenGL)
        g_pHyprOpenGL->blend(true);

    return fallback;
}

float CWindowShaderTransformer::rawAnimationProgress() {
    if (m_workspaceSwitch)
        return elapsedProgress(m_workspaceSwitch->startedAt, m_workspaceSwitch->cfg);

    return elapsedProgress(m_startedAt, m_cfg);
}

bool CWindowShaderTransformer::animationComplete() {
    if (m_workspaceSwitch)
        return m_workspaceSwitch->finished || elapsedProgress(m_workspaceSwitch->startedAt, m_workspaceSwitch->cfg) >= 1.F;

    return m_startedAt && elapsedProgress(*m_startedAt, m_cfg) >= 1.F;
}

bool CWindowShaderTransformer::animationCompleteByClock() const {
    if (m_workspaceSwitch)
        return m_workspaceSwitch->finished || elapsedProgress(m_workspaceSwitch->startedAt, m_workspaceSwitch->cfg) >= 1.F;

    return m_startedAt && elapsedProgress(*m_startedAt, m_cfg) >= 1.F;
}

} // namespace hypranimated
