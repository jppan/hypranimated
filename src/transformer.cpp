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
    m_renderTarget(makeShared<SWindowRenderTarget>()) {
}

void CWindowShaderTransformer::preWindowRender(CSurfacePassElement::SRenderData* renderData) {
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
        // capture the normal window draw so transform() can replace it with the shader output.
        g_pHyprRenderer->m_renderPass.add(makeBindOffMainPassElement(m_renderTarget));
        renderData->fadeAlpha = 1.F;
    } else {
        m_done = true;
    }
}

CFramebuffer* CWindowShaderTransformer::transform(CFramebuffer* in) {
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
    const CBox  monbox      = {0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y};
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

    g_pHyprRenderer->m_renderPass.add(makeAnimatedShaderPassElement(&m_renderTarget->sourceFramebuffer, m_shader, geometry, geometry, monitorSize, progress, seed, damage));

    damageAnimationGeometry(monitor, geometry);
    g_pHyprOpenGL->blend(true);
    return passthrough;
}

bool CWindowShaderTransformer::done() const {
    return m_done || !enabled() || !m_window.lock() || animationCompleteByClock();
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
