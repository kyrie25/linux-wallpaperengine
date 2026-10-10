// This code is a modification of the original projects that can be found at
// https://github.com/if1live/cef-gl-example
// https://github.com/andmcgregor/cefgui
#include "CWeb.h"
#include "WallpaperEngine/WebBrowser/CEF/WPSchemeHandlerFactory.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Wallpapers;

using namespace WallpaperEngine::WebBrowser;
using namespace WallpaperEngine::WebBrowser::CEF;

CWeb::CWeb (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext, WebBrowserContext& browserContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode), m_browserContext (browserContext) {
    // setup framebuffers
    this->setupFramebuffers ();

    this->m_renderHandler = new WebBrowser::CEF::RenderHandler (this);
    this->m_client = new WebBrowser::CEF::BrowserClient (m_renderHandler);
    this->m_volume = context.getApp ().getContext ().settings.audio.volume;
}

void CWeb::createBrowser () {
    // Page scripts must see the configured output dimensions on their first frame.
    CefWindowInfo window_info;
    window_info.SetAsWindowless (0);
    window_info.bounds = CefRect (0, 0, this->getWidth (), this->getHeight ());

    CefBrowserSettings browserSettings;
    // documentaion says that 60 fps is maximum value
    browserSettings.windowless_frame_rate = std::clamp (
	this->getContext ().getApp ().getContext ().settings.render.maximumFPS, 1, 60);
    // use the custom scheme for the wallpaper's files
    const std::string htmlURL = WPSchemeHandlerFactory::generateSchemeName (this->getWeb ().project.workshopId)
	+ "://root/" + this->getWeb ().filename;
    this->m_browser
	= CefBrowserHost::CreateBrowserSync (window_info, this->m_client, htmlURL, browserSettings, nullptr, nullptr);
    if (!this->m_browser) throw std::runtime_error ("Cannot create web wallpaper browser");
    this->setMuted (!getContext ().getApp ().getContext ().settings.audio.enabled
                   || getContext ().getApp ().getContext ().settings.audio.volume == 0);
}

void CWeb::setFrameRate (int fps) {
    if (m_browser) m_browser->GetHost ()->SetWindowlessFrameRate (std::clamp (fps, 1, 60));
}

void CWeb::setMuted (bool muted) {
    if (m_browser) m_browser->GetHost ()->SetAudioMuted (muted);
}

void CWeb::setVolume (int volume) {
    m_volume = std::clamp (volume, 0, 128);
    m_nextVolumeQuery = {};
    setMuted (m_volume == 0);
}

void CWeb::applyAudioVolume (pa_context* context, const pa_sink_input_info* info, int end, void* data) {
    if (end != 0 || !info || !info->proplist) return;
    auto* web = static_cast<CWeb*> (data);
    const char* process = pa_proplist_gets (info->proplist, PA_PROP_APPLICATION_PROCESS_ID);
    if (!process) return;
    char* suffix = nullptr;
    long pid = std::strtol (process, &suffix, 10);
    if (*suffix != '\0' || pid <= 0) return;
    // Chromium audio may run in another process group. Follow ancestry rather
    // than matching a shared Chromium name or mutating the user's other apps.
    for (int depth = 0; pid != getpid () && pid > 1 && depth < 64; ++depth) {
        std::ifstream stat ("/proc/" + std::to_string (pid) + "/stat");
        std::string line;
        std::getline (stat, line);
        const auto start = line.rfind (") ");
        if (start == std::string::npos) return;
        std::istringstream fields (line.substr (start + 2));
        char state;
        if (!(fields >> state >> pid)) return;
    }
    if (pid != getpid ()) return;
    pa_cvolume volume;
    pa_cvolume_set (&volume, info->volume.channels,
        pa_sw_volume_from_linear (web->m_volume / 128.0));
    if (pa_cvolume_equal (&volume, &info->volume)) return;
    if (auto* operation = pa_context_set_sink_input_volume (context, info->index, &volume, nullptr, nullptr))
        pa_operation_unref (operation);
}

void CWeb::updateAudioVolume () {
    const auto now = std::chrono::steady_clock::now ();
    if (!m_volumeLoop) {
        m_volumeLoop = pa_mainloop_new ();
        m_volumeContext = pa_context_new (pa_mainloop_get_api (m_volumeLoop), "wallpaper-web-volume");
        pa_context_connect (m_volumeContext, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);
    }
    // Never block the Wayland renderer waiting for the sound server.
    for (int i = 0; i < 32 && pa_mainloop_iterate (m_volumeLoop, 0, nullptr) > 0; ++i) { }
    if (m_volumeQuery && pa_operation_get_state (m_volumeQuery) != PA_OPERATION_RUNNING) {
        pa_operation_unref (m_volumeQuery);
        m_volumeQuery = nullptr;
    }
    const auto state = pa_context_get_state (m_volumeContext);
    if ((state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) && now >= m_nextVolumeQuery) {
        if (m_volumeQuery) {
            pa_operation_cancel (m_volumeQuery);
            pa_operation_unref (m_volumeQuery);
            m_volumeQuery = nullptr;
        }
        pa_context_unref (m_volumeContext);
        m_volumeContext = pa_context_new (pa_mainloop_get_api (m_volumeLoop), "wallpaper-web-volume");
        pa_context_connect (m_volumeContext, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);
        m_nextVolumeQuery = now + std::chrono::seconds (1);
    } else if (state == PA_CONTEXT_READY && !m_volumeQuery && now >= m_nextVolumeQuery) {
        m_volumeQuery = pa_context_get_sink_input_info_list (m_volumeContext, applyAudioVolume, this);
        m_nextVolumeQuery = now + std::chrono::milliseconds (250);
    }
}

void CWeb::setSize (const int width, const int height) {
    this->m_width = width > 0 ? width : this->m_width;
    this->m_height = height > 0 ? height : this->m_height;

    // do not refresh the texture if any of the sizes are invalid
    if (this->m_width <= 0 || this->m_height <= 0) {
	return;
    }

    // reconfigure the texture
    glBindTexture (GL_TEXTURE_2D, this->getWallpaperTexture ());
    glTexImage2D (
	GL_TEXTURE_2D, 0, GL_RGBA8, this->getWidth (), this->getHeight (), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr
    );

    // Notify cef that it was resized(maybe it's not even needed)
    if (this->m_browser) this->m_browser->GetHost ()->WasResized ();
}

void CWeb::renderFrame (const glm::ivec4& viewport) {
    if (viewport.z <= 0 || viewport.w <= 0) return;
    // ensure the viewport matches the window size, and resize if needed
    if (viewport.z != this->getWidth () || viewport.w != this->getHeight ()) {
	this->setSize (viewport.z, viewport.w);
    }
    if (!this->m_browser) this->createBrowser ();

    // ensure the virtual mouse position is up to date
    this->updateMouse (viewport);
    // use the scene's framebuffer by default
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    // ensure we render over the whole framebuffer
    glViewport (0, 0, this->getWidth (), this->getHeight ());

    // Cef processes all messages, including OnPaint, which renders frame
    // If there is no OnPaint in message loop, we will not update(render) frame
    //  This means some frames will not have OnPaint call in cef messageLoop
    //  Because of that glClear will result in flickering on higher fps
    //  Do not use glClear until some method to control rendering with cef is supported
    // We might actually try to use cef to execute javascript, and not using off-screen rendering at all
    // But for now let it be like this
    //  glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    CefDoMessageLoopWork ();
    this->updateAudioVolume ();
}

void CWeb::updateMouse (const glm::ivec4& viewport) {
    // update virtual mouse position first
    auto& input = this->getContext ().getInputContext ().getMouseInput ();

    const glm::dvec2 position = input.position ();
    const auto leftClick = input.leftClick ();
    const auto rightClick = input.rightClick ();

    CefMouseEvent evt;
    // Set mouse current position. Maybe clamps are not needed
    evt.x = std::clamp (static_cast<int> (position.x - viewport.x), 0, viewport.z);
    // Convert from OpenGL coordinates (Y=0 at bottom) to CEF coordinates (Y=0 at top)
    evt.y = viewport.w - std::clamp (static_cast<int> (position.y - viewport.y), 0, viewport.w);
    // Send mouse position to cef
    this->m_browser->GetHost ()->SendMouseMoveEvent (evt, false);

    // TODO: ANY OTHER MOUSE EVENTS TO SEND?
    if (leftClick != this->m_leftClick) {
	this->m_browser->GetHost ()->SendMouseClickEvent (
	    evt, CefBrowserHost::MouseButtonType::MBT_LEFT,
	    leftClick == WallpaperEngine::Input::MouseClickStatus::Released, 1
	);
    }

    if (rightClick != this->m_rightClick) {
	this->m_browser->GetHost ()->SendMouseClickEvent (
	    evt, CefBrowserHost::MouseButtonType::MBT_RIGHT,
	    rightClick == WallpaperEngine::Input::MouseClickStatus::Released, 1
	);
    }

    this->m_leftClick = leftClick;
    this->m_rightClick = rightClick;
}

CWeb::~CWeb () {
    if (m_volumeQuery) {
        pa_operation_cancel (m_volumeQuery);
        pa_operation_unref (m_volumeQuery);
    }
    if (m_volumeContext) {
        pa_context_disconnect (m_volumeContext);
        pa_context_unref (m_volumeContext);
    }
    if (m_volumeLoop) pa_mainloop_free (m_volumeLoop);
    // Closing can leave queued CEF callbacks while the browser releases its
    // client. Keep the ref-counted handler alive, without a destroyed owner.
    this->m_renderHandler->detach ();
    if (this->m_browser) this->m_browser->GetHost ()->CloseBrowser (true);
}
