#include "hypranimated.hpp"

namespace hypranimated {
namespace {

std::unordered_map<SShaderKey, UP<CAnimationShader>, SShaderKeyHash> g_shaders;

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

void retireShadersInGLContext() {
    if (!g_reloadShaders)
        return;

    if (g_pHyprRenderer)
        g_pHyprRenderer->makeEGLCurrent();

    g_shaders.clear();
    g_reloadShaders = false;
}

} // namespace

CAnimationShader::CAnimationShader(EAnimationKind kind, std::filesystem::path path, std::string userSource) : m_kind(kind), m_path(std::move(path)) {
    create(std::move(userSource));
}

CAnimationShader::~CAnimationShader() {
    destroy();
}

bool CAnimationShader::valid() const {
    return m_program != 0 && m_vao != 0 && m_vbo != 0;
}

void CAnimationShader::render(SP<CTexture> texture, const CBox& geometryPx, const CBox& sourceGeometryPx, const Vector2D& monitorSize, float progress, float seed,
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

void CAnimationShader::create(std::string userSource) {
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

    // the wrapper preserves niri shader inputs while mapping them to hyprland geometry.
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

void CAnimationShader::destroy() {
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

void clearShaderCache() {
    g_shaders.clear();
}

} // namespace hypranimated
