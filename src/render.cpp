#include "hypranimated.hpp"

namespace hypranimated {
namespace {

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

CBox monitorPixelBoxToLayoutBox(const CBox& geometryPx, PHLMONITOR monitor) {
    const double scale = std::max(0.01, monitor ? monitor->m_scale : 1.0);
    const auto   pos   = monitor ? monitor->m_position : Vector2D{};

    return CBox{geometryPx.x / scale + pos.x, geometryPx.y / scale + pos.y, geometryPx.w / scale, geometryPx.h / scale};
}

bool coversMonitor(const CBox& geometryPx, const Vector2D& monitorSize) {
    return geometryPx.x <= 0.5 && geometryPx.y <= 0.5 && geometryPx.x + geometryPx.w >= monitorSize.x - 0.5 &&
        geometryPx.y + geometryPx.h >= monitorSize.y - 0.5;
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
            // live captures are copied first so the shader samples a stable texture.
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
    SP<CTexture>      m_texture;
    CFramebuffer*     m_liveSourceFramebuffer = nullptr;
    CFramebuffer      m_stableSourceFramebuffer;
    CAnimationShader* m_shader = nullptr;
    CBox              m_geometryPx;
    CBox              m_sourceGeometryPx;
    Vector2D          m_monitorSize;
    float             m_progress = 0.F;
    float             m_seed     = 0.F;
    CRegion           m_damage;
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
        // subsequent window surface passes render into this private capture framebuffer.
        // they must not occlude main pass damage behind the animated window.
        return true;
    }

    const char* passName() override {
        return "CBindOffMainPassElement";
    }

  private:
    SP<SWindowRenderTarget> m_renderTarget;
};

} // namespace

CBox scaledGeometry(const CBox& logicalBox, PHLMONITOR monitor) {
    return logicalBox.copy().translate(-monitor->m_position).scale(monitor->m_scale);
}

CBox expandedWindowGeometry(const CBox& logicalBox, PHLWINDOW window) {
    if (!window || window->isFullscreen())
        return logicalBox;

    const double borderSize = std::max(window->getRealBorderSize(), 0);
    if (borderSize <= 0.0)
        return logicalBox;

    return logicalBox.copy().expand(borderSize);
}

CBox expandedScaledGeometry(const CBox& logicalBox, PHLMONITOR monitor, PHLWINDOW window) {
    return scaledGeometry(expandedWindowGeometry(logicalBox, window), monitor);
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

CRegion animationDamageForGeometry(const CBox& geometryPx, const Vector2D& monitorSize) {
    const auto box = clippedAnimationBox(geometryPx, monitorSize);
    if (box.w <= 0 || box.h <= 0)
        return {};

    return CRegion{box};
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

UP<IPassElement> makeAnimatedShaderPassElement(SP<CTexture> texture, CAnimationShader* shader, CBox geometryPx, CBox sourceGeometryPx, Vector2D monitorSize, float progress,
                                               float seed, CRegion damage) {
    return makeUnique<CAnimatedShaderPassElement>(std::move(texture), shader, std::move(geometryPx), std::move(sourceGeometryPx), std::move(monitorSize), progress, seed,
                                                  std::move(damage));
}

UP<IPassElement> makeAnimatedShaderPassElement(CFramebuffer* liveSourceFramebuffer, CAnimationShader* shader, CBox geometryPx, CBox sourceGeometryPx, Vector2D monitorSize,
                                               float progress, float seed, CRegion damage) {
    return makeUnique<CAnimatedShaderPassElement>(liveSourceFramebuffer, shader, std::move(geometryPx), std::move(sourceGeometryPx), std::move(monitorSize), progress, seed,
                                                  std::move(damage));
}

UP<IPassElement> makeBindOffMainPassElement(SP<SWindowRenderTarget> renderTarget) {
    return makeUnique<CBindOffMainPassElement>(std::move(renderTarget));
}

} // namespace hypranimated
