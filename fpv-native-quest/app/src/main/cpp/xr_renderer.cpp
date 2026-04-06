/**
 * xr_renderer.cpp — OpenXR stereo renderer (TASK-005).
 *
 * Full implementation: OpenXR 1.0 session lifecycle + OpenGL ES 3.2 swapchain +
 * samplerExternalOES video texture.  Replaces webxr-renderer.js with a native
 * equivalent that reads real IPD/FOV from the headset and skips the browser
 * compositor (saves ~5–10 ms per frame vs. the WebXR path).
 *
 * Architecture:
 *   XrInstance → XrSession → per-eye XrSwapchain
 *       ↓
 *   xrWaitFrame() → xrBeginFrame()
 *       ↓
 *   for each XrView (left, right):
 *       glBindFramebuffer(swapchain image FBO)
 *       glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_videoTexId)   // TASK-004 zero-copy
 *       glDrawElements(quad)                                    // samplerExternalOES shader
 *   xrEndFrame()
 *
 * EGL / texture sharing:
 *   XrRenderThread.kt creates an EglBase that shares with the WebRTC decoder context
 *   (eglBase.eglBaseContext).  Both contexts belong to the same EGL share group, so
 *   the OES texture allocated by WebRTC's SurfaceTextureHelper is accessible here.
 *   video_decoder.cpp calls glFlush() after each frame update to ensure GPU-visibility
 *   across the shared contexts (Adreno/Quest 2 requirement).
 *
 * Required setup before building:
 *   Run ./setup-openxr.sh  (downloads Khronos headers + Meta loader via Maven Prefab)
 */

// XR_USE_PLATFORM_ANDROID and XR_USE_GRAPHICS_API_OPENGL_ES are defined
// via target_compile_definitions() in CMakeLists.txt.

#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>   // GL_TEXTURE_EXTERNAL_OES

// OpenXR headers — installed by setup-openxr.sh into third_party/openxr/include
// or provided by the Meta Maven Prefab AAR.
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "video_state.h"    // videostate_getTexId(), videostate_getMatrix()

#include <atomic>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

// ── Logging ───────────────────────────────────────────────────────────────────

#define LOG_TAG "xr_renderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── XR error helpers ─────────────────────────────────────────────────────────

static const char* xrResultStr(XrResult r) {
    switch (r) {
        case XR_SUCCESS:                         return "XR_SUCCESS";
        case XR_ERROR_INITIALIZATION_FAILED:     return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_RUNTIME_FAILURE:           return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_INSTANCE_LOST:             return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_SESSION_LOST:              return "XR_ERROR_SESSION_LOST";
        case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
        case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING:
            return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
        default:                                 return "(unknown XrResult)";
    }
}

#define XR_CHECK(expr)                                                      \
    do {                                                                     \
        XrResult _r = (expr);                                                \
        if (XR_FAILED(_r)) {                                                 \
            LOGE("XR FAILED: %s → %s (%d)", #expr, xrResultStr(_r), _r);   \
            return false;                                                     \
        }                                                                     \
    } while (0)

#define XR_CHECK_VOID(expr)                                                  \
    do {                                                                     \
        XrResult _r = (expr);                                                \
        if (XR_FAILED(_r)) {                                                 \
            LOGE("XR FAILED: %s → %s (%d)", #expr, xrResultStr(_r), _r);   \
            return;                                                           \
        }                                                                     \
    } while (0)

// ── Column-major 4×4 matrix math ─────────────────────────────────────────────

// m[col*4 + row]  (OpenGL column-major convention)
struct Mat4 {
    float m[16] = {};

    static Mat4 Identity() {
        Mat4 r; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f; return r;
    }

    /** Build view matrix (world → eye) from OpenXR eye pose (eye → world). */
    static Mat4 ViewFromPose(const XrPosef& pose) {
        const float qx = pose.orientation.x, qy = pose.orientation.y;
        const float qz = pose.orientation.z, qw = pose.orientation.w;

        // Rotation matrix rows from quaternion
        float r00 = 1.f-2.f*(qy*qy+qz*qz), r01 = 2.f*(qx*qy-qw*qz), r02 = 2.f*(qx*qz+qw*qy);
        float r10 = 2.f*(qx*qy+qw*qz),   r11 = 1.f-2.f*(qx*qx+qz*qz), r12 = 2.f*(qy*qz-qw*qx);
        float r20 = 2.f*(qx*qz-qw*qy),   r21 = 2.f*(qy*qz+qw*qx),   r22 = 1.f-2.f*(qx*qx+qy*qy);

        const float px = pose.position.x, py = pose.position.y, pz = pose.position.z;

        // Translation: -(R^T * pos); R^T[row i] = R[col i]
        float tx = -(r00*px + r10*py + r20*pz);
        float ty = -(r01*px + r11*py + r21*pz);
        float tz = -(r02*px + r12*py + r22*pz);

        // Column-major: each column is one column of R^T
        Mat4 v;
        v.m[ 0]=r00; v.m[ 1]=r01; v.m[ 2]=r02; v.m[ 3]=0;  // col 0
        v.m[ 4]=r10; v.m[ 5]=r11; v.m[ 6]=r12; v.m[ 7]=0;  // col 1
        v.m[ 8]=r20; v.m[ 9]=r21; v.m[10]=r22; v.m[11]=0;  // col 2
        v.m[12]=tx;  v.m[13]=ty;  v.m[14]=tz;  v.m[15]=1;  // col 3
        return v;
    }

    /**
     * Asymmetric frustum projection from OpenXR tangent-angle FOV.
     * Maps nearZ → NDC z = -1, farZ → NDC z = +1 (standard OpenGL convention).
     */
    static Mat4 ProjFromFov(const XrFovf& fov, float nearZ, float farZ) {
        float tL = tanf(fov.angleLeft),  tR = tanf(fov.angleRight);
        float tU = tanf(fov.angleUp),    tD = tanf(fov.angleDown);
        float w = tR - tL, h = tU - tD;
        Mat4 p;
        p.m[ 0] = 2.f / w;
        p.m[ 5] = 2.f / h;
        p.m[ 8] = (tR + tL) / w;
        p.m[ 9] = (tU + tD) / h;
        p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
        p.m[11] = -1.f;
        p.m[14] = -(2.f * farZ * nearZ) / (farZ - nearZ);
        return p;
    }

    static Mat4 Mul(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += a.m[k*4+row] * b.m[col*4+k];
                r.m[col*4+row] = s;
            }
        return r;
    }
};

// ── Shader sources ────────────────────────────────────────────────────────────

// World-space video quad shader (mirrors webxr-renderer.js geometry):
//   a_pos is in [-1, 1]; scaled by half-width/height uniforms
//   a_uv  is [0,1]×[0,1]; corrected by SurfaceTexture affine transform (u_stMat)
//
// uniforms:
//   u_viewProj  mat4   — projection × view for this eye
//   u_halfW     float  — half-width of virtual screen in metres (default 1.0)
//   u_halfH     float  — half-height (default 0.5625 for 16:9)
//   u_dist      float  — distance from origin (default 1.5 m)
//   u_yOff      float  — vertical offset (default 0.0)
//   u_video     sampler — OES video texture
//   u_stMat     mat3   — SurfaceTexture UV transform (row→col transposed in C++)

static const char* VERT_SRC = R"glsl(
#version 300 es
in vec2 a_pos;
in vec2 a_uv;
uniform mat4 u_viewProj;
uniform float u_halfW;
uniform float u_halfH;
uniform float u_dist;
uniform float u_yOff;
out vec2 v_uv;
void main() {
    vec4 wp = vec4(a_pos.x * u_halfW, a_pos.y * u_halfH + u_yOff, -u_dist, 1.0);
    gl_Position = u_viewProj * wp;
    v_uv = a_uv;
}
)glsl";

static const char* FRAG_SRC = R"glsl(
#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES u_video;
uniform mat3 u_stMat;
in vec2 v_uv;
out vec4 outColor;
void main() {
    vec2 uv = (u_stMat * vec3(v_uv, 1.0)).xy;
    outColor = texture(u_video, uv);
}
)glsl";

// ── GL helpers ────────────────────────────────────────────────────────────────

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        LOGE("Shader compile error: %s", buf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint linkProgram(const char* vertSrc, const char* fragSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER,   vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        LOGE("Program link error: %s", buf);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void checkGlError(const char* tag) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) LOGE("GL error after %s: 0x%x", tag, e);
}

// ── XrApp state ───────────────────────────────────────────────────────────────

struct SwapchainData {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;
    uint32_t width  = 0;
    uint32_t height = 0;
};

struct XrApp {
    // OpenXR handles
    XrInstance      instance     = XR_NULL_HANDLE;
    XrSession       session      = XR_NULL_HANDLE;
    XrSpace         appSpace     = XR_NULL_HANDLE;
    XrSystemId      systemId     = XR_NULL_SYSTEM_ID;

    // Per-eye swapchains
    SwapchainData   swapchains[2];

    // Session state machine
    XrSessionState  sessionState = XR_SESSION_STATE_UNKNOWN;
    bool            sessionRunning = false;
    bool            exitRequested  = false;

    // Android / EGL context
    JavaVM*         jvm          = nullptr;
    jobject         activity     = nullptr; // global ref

    // OpenGL video shader
    GLuint          videoProgram = 0;
    GLint           u_viewProj   = -1;
    GLint           u_halfW      = -1;
    GLint           u_halfH      = -1;
    GLint           u_dist       = -1;
    GLint           u_yOff       = -1;
    GLint           u_video      = -1;
    GLint           u_stMat      = -1;

    // Quad geometry
    GLuint          quadVAO      = 0;
    GLuint          quadVBO      = 0;
    GLuint          quadEBO      = 0;

    // Virtual screen parameters (matching webxr-renderer.js defaults)
    float screenHalfW = 1.0f;    // half-width  = 1 m  → screen 2 m wide
    float screenHalfH = 0.5625f; // half-height: 16:9 → 1.125 m tall
    float screenDist  = 1.5f;    // 1.5 m away from eyes
    float screenYOff  = 0.0f;    // no vertical offset
};

static XrApp g_xr;

// ── Swapchain helpers ─────────────────────────────────────────────────────────

static bool createSwapchain(XrSession session, uint32_t width, uint32_t height,
                             SwapchainData& out) {
    // Enumerate supported formats; prefer sRGB8 for correct gamma, fall back to RGBA8.
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, fmts.data());

    int64_t chosenFmt = GL_RGBA8;
    for (int64_t f : fmts) {
        if (f == GL_SRGB8_ALPHA8) { chosenFmt = f; break; }
    }
    LOGI("Swapchain format: 0x%llx (%s)", (long long)chosenFmt,
         chosenFmt == GL_SRGB8_ALPHA8 ? "sRGB8_A8" : "RGBA8");

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format      = chosenFmt;
    sci.sampleCount = 1;
    sci.width       = width;
    sci.height      = height;
    sci.faceCount   = 1;
    sci.arraySize   = 1;
    sci.mipCount    = 1;

    XR_CHECK(xrCreateSwapchain(session, &sci, &out.handle));
    out.width  = width;
    out.height = height;

    // Enumerate swapchain images
    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(out.handle, 0, &imgCount, nullptr);
    out.images.resize(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(out.handle, imgCount, &imgCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(out.images.data()));

    // Create one FBO per swapchain image (avoids re-attaching each frame)
    out.fbos.resize(imgCount, 0);
    glGenFramebuffers((GLsizei)imgCount, out.fbos.data());
    for (uint32_t i = 0; i < imgCount; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, out.fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, out.images[i].image, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            LOGE("FBO %u incomplete: 0x%x", i, status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    LOGI("Swapchain created: %u×%u, %u images", width, height, imgCount);
    return true;
}

static void destroySwapchain(SwapchainData& sc) {
    if (!sc.fbos.empty()) {
        glDeleteFramebuffers((GLsizei)sc.fbos.size(), sc.fbos.data());
        sc.fbos.clear();
    }
    if (sc.handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(sc.handle);
        sc.handle = XR_NULL_HANDLE;
    }
    sc.images.clear();
}

// ── Quad geometry ─────────────────────────────────────────────────────────────

// Interleaved: x, y, u, v  (positions in [-1,1], UVs in [0,1])
static const float QUAD_VERTS[] = {
    -1.f, -1.f,  0.f, 0.f,   // bottom-left
     1.f, -1.f,  1.f, 0.f,   // bottom-right
     1.f,  1.f,  1.f, 1.f,   // top-right
    -1.f,  1.f,  0.f, 1.f,   // top-left
};
static const uint16_t QUAD_IDX[] = {0, 1, 2,  0, 2, 3};

static bool createQuadGeometry(GLuint& vao, GLuint& vbo, GLuint& ebo) {
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTS), QUAD_VERTS, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(QUAD_IDX), QUAD_IDX, GL_STATIC_DRAW);

    GLsizei stride = 4 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);                 // a_pos
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(2*sizeof(float))); // a_uv

    glBindVertexArray(0);
    checkGlError("createQuadGeometry");
    return true;
}

// ── XrApp init ────────────────────────────────────────────────────────────────

static bool xrApp_init(JNIEnv* env, jobject activity) {
    env->GetJavaVM(&g_xr.jvm);
    g_xr.activity = env->NewGlobalRef(activity);

    // ── 1. Initialize OpenXR loader (Android mandatory first step) ────────────
    {
        PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
        xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            (PFN_xrVoidFunction*)&xrInitializeLoaderKHR);
        if (!xrInitializeLoaderKHR) {
            LOGE("xrInitializeLoaderKHR not found — check libopenxr_loader.so");
            return false;
        }
        XrLoaderInitInfoAndroidKHR loaderInfo = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInfo.applicationVM       = g_xr.jvm;
        loaderInfo.applicationContext  = g_xr.activity;
        XR_CHECK(xrInitializeLoaderKHR(
            reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)));
        LOGI("OpenXR loader initialized");
    }

    // ── 2. Create XrInstance ──────────────────────────────────────────────────
    {
        XrInstanceCreateInfoAndroidKHR androidInfo = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        androidInfo.applicationVM       = g_xr.jvm;
        androidInfo.applicationActivity = g_xr.activity;

        const char* exts[] = {
            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        };

        XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
        ici.next                               = &androidInfo;
        ici.enabledExtensionCount              = 2;
        ici.enabledExtensionNames              = exts;
        ici.applicationInfo.apiVersion         = XR_API_VERSION_1_0;
        snprintf(ici.applicationInfo.applicationName,
                 XR_MAX_APPLICATION_NAME_SIZE, "FPV Quest");
        snprintf(ici.applicationInfo.engineName,
                 XR_MAX_ENGINE_NAME_SIZE, "custom");

        XR_CHECK(xrCreateInstance(&ici, &g_xr.instance));
        LOGI("XrInstance created");
    }

    // ── 3. Get system ─────────────────────────────────────────────────────────
    {
        XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
        sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XR_CHECK(xrGetSystem(g_xr.instance, &sysInfo, &g_xr.systemId));
        LOGI("XrSystem acquired (id=%llu)", (unsigned long long)g_xr.systemId);
    }

    // ── 4. Satisfy graphics requirements (mandatory before xrCreateSession) ───
    {
        PFN_xrGetOpenGLESGraphicsRequirementsKHR getReqs = nullptr;
        xrGetInstanceProcAddr(g_xr.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
            (PFN_xrVoidFunction*)&getReqs);
        if (getReqs) {
            XrGraphicsRequirementsOpenGLESKHR reqs = {
                XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
            getReqs(g_xr.instance, g_xr.systemId, &reqs);
            LOGI("OpenGL ES min version: %u.%u",
                 XR_VERSION_MAJOR(reqs.minApiVersionSupported),
                 XR_VERSION_MINOR(reqs.minApiVersionSupported));
        }
    }

    // ── 5. Query view configuration for per-eye resolution ───────────────────
    uint32_t recWidth = 1440, recHeight = 1584;  // Quest 2 defaults if query fails
    {
        uint32_t vcCount = 0;
        xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vcCount, nullptr);
        if (vcCount >= 2) {
            std::vector<XrViewConfigurationView> vcv(vcCount,
                {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.systemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                vcCount, &vcCount, vcv.data());
            recWidth  = vcv[0].recommendedImageRectWidth;
            recHeight = vcv[0].recommendedImageRectHeight;
        }
        LOGI("Recommended swapchain size: %u×%u", recWidth, recHeight);
    }

    // ── 6. Create XrSession with current EGL context ──────────────────────────
    {
        EGLDisplay display = eglGetCurrentDisplay();
        EGLContext context = eglGetCurrentContext();

        // Recover EGLConfig from current context
        EGLint configId = 0;
        eglQueryContext(display, context, EGL_CONFIG_ID, &configId);
        EGLConfig config = nullptr;
        EGLint nCfg = 0;
        EGLint cfgAttribs[] = {EGL_CONFIG_ID, configId, EGL_NONE};
        eglChooseConfig(display, cfgAttribs, &config, 1, &nCfg);

        XrGraphicsBindingOpenGLESAndroidKHR gfxBinding = {
            XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        gfxBinding.display = display;
        gfxBinding.config  = config;
        gfxBinding.context = context;

        XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
        sci.next     = &gfxBinding;
        sci.systemId = g_xr.systemId;
        XR_CHECK(xrCreateSession(g_xr.instance, &sci, &g_xr.session));
        LOGI("XrSession created (EGL context=%p)", (void*)context);
    }

    // ── 7. Create reference space ─────────────────────────────────────────────
    {
        XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        rsci.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        if (XR_FAILED(xrCreateReferenceSpace(g_xr.session, &rsci, &g_xr.appSpace))) {
            // Fall back to LOCAL space (no guaranteed floor origin)
            rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            XR_CHECK(xrCreateReferenceSpace(g_xr.session, &rsci, &g_xr.appSpace));
            LOGW("STAGE space unavailable, using LOCAL");
        } else {
            LOGI("Reference space: STAGE");
        }
    }

    // ── 8. Create per-eye swapchains ──────────────────────────────────────────
    for (int eye = 0; eye < 2; ++eye) {
        if (!createSwapchain(g_xr.session, recWidth, recHeight, g_xr.swapchains[eye]))
            return false;
    }

    // ── 9. Compile shaders and build quad geometry ────────────────────────────
    g_xr.videoProgram = linkProgram(VERT_SRC, FRAG_SRC);
    if (!g_xr.videoProgram) return false;

    // Bind attribute locations
    glBindAttribLocation(g_xr.videoProgram, 0, "a_pos");
    glBindAttribLocation(g_xr.videoProgram, 1, "a_uv");

    // Cache uniform locations
    g_xr.u_viewProj = glGetUniformLocation(g_xr.videoProgram, "u_viewProj");
    g_xr.u_halfW    = glGetUniformLocation(g_xr.videoProgram, "u_halfW");
    g_xr.u_halfH    = glGetUniformLocation(g_xr.videoProgram, "u_halfH");
    g_xr.u_dist     = glGetUniformLocation(g_xr.videoProgram, "u_dist");
    g_xr.u_yOff     = glGetUniformLocation(g_xr.videoProgram, "u_yOff");
    g_xr.u_video    = glGetUniformLocation(g_xr.videoProgram, "u_video");
    g_xr.u_stMat    = glGetUniformLocation(g_xr.videoProgram, "u_stMat");

    if (!createQuadGeometry(g_xr.quadVAO, g_xr.quadVBO, g_xr.quadEBO))
        return false;

    // Need to re-link after BindAttribLocation
    glLinkProgram(g_xr.videoProgram);

    LOGI("xr_renderer: init complete — session created, shaders compiled");
    return true;
}

// ── Event polling ─────────────────────────────────────────────────────────────

/**
 * Drain the OpenXR event queue.
 * Returns false when the render loop should stop (session lost / exit requested).
 */
static bool xrApp_pollEvents() {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_xr.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                g_xr.sessionState = e->state;
                LOGI("Session state → %d", (int)g_xr.sessionState);

                if (g_xr.sessionState == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo sbi = {XR_TYPE_SESSION_BEGIN_INFO};
                    sbi.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(g_xr.session, &sbi))) {
                        g_xr.sessionRunning = true;
                        LOGI("Session running");
                    }
                } else if (g_xr.sessionState == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(g_xr.session);
                    g_xr.sessionRunning = false;
                    LOGI("Session stopped");
                } else if (g_xr.sessionState == XR_SESSION_STATE_EXITING ||
                           g_xr.sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                    g_xr.exitRequested = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOGW("Instance loss pending");
                g_xr.exitRequested = true;
                break;
            default:
                break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !g_xr.exitRequested;
}

// ── Frame rendering ───────────────────────────────────────────────────────────

/**
 * Render one stereo frame.
 * Called in a tight loop from XrRenderThread; xrWaitFrame provides frame pacing.
 * Returns false when the render loop should stop.
 */
static bool xrApp_renderFrame() {
    if (!xrApp_pollEvents()) return false;

    if (!g_xr.sessionRunning) {
        // Session not yet running; yield briefly so we don't busy-loop
        struct timespec ts = {0, 2000000}; // 2 ms
        nanosleep(&ts, nullptr);
        return true;
    }

    // ── Wait for the predicted display time ───────────────────────────────────
    XrFrameWaitInfo fwi   = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState    fState = {XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(g_xr.session, &fwi, &fState));
    XR_CHECK(xrBeginFrame(g_xr.session, nullptr));

    // ── Locate eye views at predicted display time ────────────────────────────
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrViewLocateInfo vli    = {XR_TYPE_VIEW_LOCATE_INFO};
    XrViewState      vState = {XR_TYPE_VIEW_STATE};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime           = fState.predictedDisplayTime;
    vli.space                 = g_xr.appSpace;
    uint32_t viewCount = 0;
    xrLocateViews(g_xr.session, &vli, &vState, 2, &viewCount, views);

    // Projection layer for xrEndFrame
    XrCompositionLayerProjectionView projViews[2] = {};
    XrCompositionLayerProjection     layer        = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};

    // ── Get video texture (may be 0 if no frame yet) ──────────────────────────
    GLuint videoTexId = videostate_getTexId();
    float  stMat[9];
    videostate_getMatrix(stMat);

    // Convert row-major Android affine (float[9]) → column-major GLSL mat3.
    // We pass transpose=GL_TRUE to glUniformMatrix3fv so no manual transposition needed.

    const bool hasVideo = (videoTexId != 0) && fState.shouldRender;

    // ── Render each eye ───────────────────────────────────────────────────────
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    for (int eye = 0; eye < 2 && (int)viewCount > eye; ++eye) {
        SwapchainData& sc = g_xr.swapchains[eye];

        // Acquire swapchain image
        XrSwapchainImageAcquireInfo acqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imgIndex = 0;
        XR_CHECK(xrAcquireSwapchainImage(sc.handle, &acqInfo, &imgIndex));

        XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        XR_CHECK(xrWaitSwapchainImage(sc.handle, &waitInfo));

        // Bind FBO
        glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[imgIndex]);
        glViewport(0, 0, (GLsizei)sc.width, (GLsizei)sc.height);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (hasVideo) {
            // Build view-projection matrix for this eye
            Mat4 view = Mat4::ViewFromPose(views[eye].pose);
            Mat4 proj = Mat4::ProjFromFov(views[eye].fov, 0.05f, 100.f);
            Mat4 vp   = Mat4::Mul(proj, view);

            glUseProgram(g_xr.videoProgram);
            glUniformMatrix4fv(g_xr.u_viewProj, 1, GL_FALSE, vp.m);
            glUniform1f(g_xr.u_halfW,  g_xr.screenHalfW);
            glUniform1f(g_xr.u_halfH,  g_xr.screenHalfH);
            glUniform1f(g_xr.u_dist,   g_xr.screenDist);
            glUniform1f(g_xr.u_yOff,   g_xr.screenYOff);

            // SurfaceTexture affine — row-major in stMat; use transpose=GL_TRUE
            glUniformMatrix3fv(g_xr.u_stMat, 1, GL_TRUE, stMat);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, videoTexId);
            glUniform1i(g_xr.u_video, 0);

            glBindVertexArray(g_xr.quadVAO);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
            glBindVertexArray(0);

            checkGlError("draw video quad");
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XR_CHECK(xrReleaseSwapchainImage(sc.handle, &relInfo));

        // Fill projection view for this eye
        projViews[eye].type                                 = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projViews[eye].pose                                 = views[eye].pose;
        projViews[eye].fov                                  = views[eye].fov;
        projViews[eye].subImage.swapchain                   = sc.handle;
        projViews[eye].subImage.imageArrayIndex             = 0;
        projViews[eye].subImage.imageRect.offset            = {0, 0};
        projViews[eye].subImage.imageRect.extent.width      = (int32_t)sc.width;
        projViews[eye].subImage.imageRect.extent.height     = (int32_t)sc.height;
    }

    // ── Submit frame ──────────────────────────────────────────────────────────
    layer.space      = g_xr.appSpace;
    layer.viewCount  = 2;
    layer.views      = projViews;

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)
    };

    XrFrameEndInfo fei       = {XR_TYPE_FRAME_END_INFO};
    fei.displayTime          = fState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount           = fState.shouldRender ? 1u : 0u;
    fei.layers               = fState.shouldRender ? layers : nullptr;
    XR_CHECK(xrEndFrame(g_xr.session, &fei));

    return true;
}

// ── Cleanup ───────────────────────────────────────────────────────────────────

static void xrApp_destroy(JNIEnv* env) {
    // Delete GL resources
    if (g_xr.quadVAO)      { glDeleteVertexArrays(1, &g_xr.quadVAO); g_xr.quadVAO = 0; }
    if (g_xr.quadVBO)      { glDeleteBuffers(1, &g_xr.quadVBO);       g_xr.quadVBO = 0; }
    if (g_xr.quadEBO)      { glDeleteBuffers(1, &g_xr.quadEBO);       g_xr.quadEBO = 0; }
    if (g_xr.videoProgram) { glDeleteProgram(g_xr.videoProgram);      g_xr.videoProgram = 0; }

    for (int eye = 0; eye < 2; ++eye) destroySwapchain(g_xr.swapchains[eye]);

    if (g_xr.appSpace  != XR_NULL_HANDLE) { xrDestroySpace(g_xr.appSpace);    g_xr.appSpace  = XR_NULL_HANDLE; }
    if (g_xr.session   != XR_NULL_HANDLE) { xrDestroySession(g_xr.session);   g_xr.session   = XR_NULL_HANDLE; }
    if (g_xr.instance  != XR_NULL_HANDLE) { xrDestroyInstance(g_xr.instance); g_xr.instance  = XR_NULL_HANDLE; }

    if (g_xr.activity && env) { env->DeleteGlobalRef(g_xr.activity); g_xr.activity = nullptr; }

    g_xr.sessionRunning = false;
    g_xr.exitRequested  = false;
    g_xr.sessionState   = XR_SESSION_STATE_UNKNOWN;

    LOGI("xr_renderer: destroyed");
}

// ── JNI exports ───────────────────────────────────────────────────────────────

extern "C" {

/**
 * Initialize OpenXR instance + session.
 * Must be called from XrRenderThread with the shared EGL context current.
 *
 * @param activity  The MainActivity instance (used by XrInstanceCreateInfoAndroidKHR).
 * @return true on success.
 */
JNIEXPORT jboolean JNICALL
Java_com_fpv_quest_MainActivity_nativeInitXR(JNIEnv* env, jclass /*clazz*/, jobject activity) {
    LOGI("nativeInitXR()");
    return xrApp_init(env, activity) ? JNI_TRUE : JNI_FALSE;
}

/**
 * Render one stereo frame (and poll XR events).
 * Blocks inside xrWaitFrame for natural frame pacing (~13.9 ms at 72 Hz).
 *
 * @return true  — keep running.
 * @return false — session lost / exit requested; stop the render loop.
 */
JNIEXPORT jboolean JNICALL
Java_com_fpv_quest_MainActivity_nativeRenderFrame(JNIEnv* /*env*/, jclass /*clazz*/) {
    return xrApp_renderFrame() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Gracefully request the XR session to exit.
 * Unblocks a pending xrWaitFrame() call so the render thread can stop promptly.
 */
JNIEXPORT void JNICALL
Java_com_fpv_quest_MainActivity_nativeRequestExitSession(JNIEnv* /*env*/, jclass /*clazz*/) {
    if (g_xr.session != XR_NULL_HANDLE && g_xr.sessionRunning) {
        xrRequestExitSession(g_xr.session);
        LOGI("Exit session requested");
    }
    g_xr.exitRequested = true;
}

/**
 * Destroy the OpenXR session and release all resources.
 * Called from XrRenderThread after the render loop exits.
 */
JNIEXPORT void JNICALL
Java_com_fpv_quest_MainActivity_nativeDestroyXR(JNIEnv* env, jclass /*clazz*/) {
    LOGI("nativeDestroyXR()");
    xrApp_destroy(env);
}

} // extern "C"
