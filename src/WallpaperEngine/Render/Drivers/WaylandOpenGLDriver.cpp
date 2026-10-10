#include "WaylandOpenGLDriver.h"
#include "VideoFactories.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"

#define class _class
#define namespace _namespace
#define static
extern "C" {
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"
#include <linux/input-event-codes.h>
}
#undef class
#undef namespace
#undef static

#include <algorithm>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>

using namespace WallpaperEngine::Render::Drivers;

static void handlePointerEnter (
    void* data, struct wl_pointer* wl_pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t surface_x,
    wl_fixed_t surface_y
) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);
    const auto viewport = driver->surfaceToViewport (surface);
    if (!viewport) return;
    driver->viewportInFocus = viewport;
    viewport->mousePos = {
	wl_fixed_to_double (surface_x) * viewport->scale,
	(viewport->size.y - wl_fixed_to_double (surface_y)) * viewport->scale,
    };
    wl_surface_set_buffer_scale (viewport->cursorSurface, viewport->scale);
    wl_surface_attach (viewport->cursorSurface, wl_cursor_image_get_buffer (viewport->pointer->images[0]), 0, 0);
    wl_pointer_set_cursor (
	wl_pointer, serial, viewport->cursorSurface, viewport->pointer->images[0]->hotspot_x,
	viewport->pointer->images[0]->hotspot_y
    );
    wl_surface_commit (viewport->cursorSurface);
}

static void
handlePointerLeave (void* data, struct wl_pointer* wl_pointer, uint32_t serial, struct wl_surface* surface) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);
    if (driver->viewportInFocus && driver->viewportInFocus->surface == surface) {
	driver->viewportInFocus->leftClick = WallpaperEngine::Input::MouseClickStatus::Released;
	driver->viewportInFocus->rightClick = WallpaperEngine::Input::MouseClickStatus::Released;
	driver->viewportInFocus = nullptr;
    }
}

static void handlePointerAxis (void* data, wl_pointer* wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) { }

static void handlePointerMotion (
    void* data, struct wl_pointer* wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y
) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);

    const auto x = wl_fixed_to_double (surface_x);
    auto y = wl_fixed_to_double (surface_y);

    if (!driver->viewportInFocus) {
	return;
    }

    // Convert from Wayland coordinate system (Y=0 at top) to OpenGL coordinate system (Y=0 at bottom)
    const double viewportHeight = static_cast<double> (driver->viewportInFocus->size.y);
    y = viewportHeight - y;

    driver->viewportInFocus->mousePos = { x * driver->viewportInFocus->scale, y * driver->viewportInFocus->scale };
}

static void handlePointerButton (
    void* data, struct wl_pointer* wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t button_state
) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);

    if (!driver->viewportInFocus) {
	return;
    }

    if (button == BTN_LEFT) {
	if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
	    driver->viewportInFocus->leftClick = WallpaperEngine::Input::MouseClickStatus::Clicked;
	} else if (button_state == WL_POINTER_BUTTON_STATE_RELEASED) {
	    driver->viewportInFocus->leftClick = WallpaperEngine::Input::MouseClickStatus::Released;
	}
    } else if (button == BTN_RIGHT) {
	if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
	    driver->viewportInFocus->rightClick = WallpaperEngine::Input::MouseClickStatus::Clicked;
	} else if (button_state == WL_POINTER_BUTTON_STATE_RELEASED) {
	    driver->viewportInFocus->rightClick = WallpaperEngine::Input::MouseClickStatus::Released;
	}
    }
}

constexpr struct wl_pointer_listener pointerListener = { .enter = handlePointerEnter,
							 .leave = handlePointerLeave,
							 .motion = handlePointerMotion,
							 .button = handlePointerButton,
							 .axis = handlePointerAxis };

static void handleCapabilities (void* data, wl_seat* wl_seat, uint32_t capabilities) {
    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
	wl_pointer_add_listener (wl_seat_get_pointer (wl_seat), &pointerListener, data);
    }
}

constexpr struct wl_seat_listener seatListener = { .capabilities = handleCapabilities };

static void
handleGlobal (void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);

    if (strcmp (interface, wl_compositor_interface.name) == 0) {
	driver->getWaylandContext ()->compositor
	    = static_cast<wl_compositor*> (wl_registry_bind (registry, name, &wl_compositor_interface, 4));
    } else if (strcmp (interface, wl_shm_interface.name) == 0) {
	driver->getWaylandContext ()->shm
	    = static_cast<wl_shm*> (wl_registry_bind (registry, name, &wl_shm_interface, 1));
    } else if (strcmp (interface, wl_output_interface.name) == 0) {
	driver->m_screens.emplace_back (
	    new WallpaperEngine::Render::Drivers::Output::WaylandOutputViewport (driver, name, registry)
	);
    } else if (strcmp (interface, zwlr_layer_shell_v1_interface.name) == 0) {
	driver->getWaylandContext ()->layerShell
	    = static_cast<zwlr_layer_shell_v1*> (wl_registry_bind (registry, name, &zwlr_layer_shell_v1_interface, 1));
    } else if (strcmp (interface, wl_seat_interface.name) == 0) {
	driver->getWaylandContext ()->seat
	    = static_cast<wl_seat*> (wl_registry_bind (registry, name, &wl_seat_interface, 1));
	wl_seat_add_listener (driver->getWaylandContext ()->seat, &seatListener, driver);
    } else if (strcmp (interface, zxdg_output_manager_v1_interface.name) == 0) {
	driver->getWaylandContext ()->xdgOutputManager = static_cast<zxdg_output_manager_v1*> (
	    wl_registry_bind (registry, name, &zxdg_output_manager_v1_interface, std::min (version, 3u))
	);
    }
}

static void handleGlobalRemoved (void* data, struct wl_registry* registry, uint32_t id) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);
    const auto found = std::ranges::find (driver->m_screens, id, &Output::WaylandOutputViewport::waylandName);
    if (found != driver->m_screens.end ()) driver->onLayerClose (*found);

}

constexpr struct wl_registry_listener registryListener = {
    .global = handleGlobal,
    .global_remove = handleGlobalRemoved,
};

void WaylandOpenGLDriver::initEGL () {
    const char* CLIENT_EXTENSIONS = eglQueryString (EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!CLIENT_EXTENSIONS) {
	sLog.exception ("Failed to query EGL Extensions");
    }

    const auto CLIENTEXTENSIONS = std::string (CLIENT_EXTENSIONS);

    if (CLIENTEXTENSIONS.find ("EGL_EXT_platform_base") == std::string::npos) {
	sLog.exception ("EGL_EXT_platform_base not supported by EGL!");
    }

    if (CLIENTEXTENSIONS.find ("EGL_EXT_platform_wayland") == std::string::npos) {
	sLog.exception ("EGL_EXT_platform_wayland not supported by EGL!");
    }

    const auto eglGetPlatformDisplayEXT
	= reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (eglGetProcAddress ("eglGetPlatformDisplayEXT"));
    m_eglContext.eglCreatePlatformWindowSurfaceEXT = reinterpret_cast<PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC> (
	eglGetProcAddress ("eglCreatePlatformWindowSurfaceEXT")
    );

    if (!eglGetPlatformDisplayEXT || !m_eglContext.eglCreatePlatformWindowSurfaceEXT) {
	sLog.exception ("EGL did not return EXT proc pointers!");
    }

    m_eglContext.display = eglGetPlatformDisplayEXT (EGL_PLATFORM_WAYLAND_EXT, m_waylandContext.display, nullptr);

    if (m_eglContext.display == EGL_NO_DISPLAY) {
	this->finishEGL ();
	sLog.exception ("eglGetPlatformDisplayEXT failed!");
    }

    if (!eglInitialize (m_eglContext.display, nullptr, nullptr)) {
	this->finishEGL ();
	sLog.exception ("eglInitialize failed!");
    }

    const auto CLIENTEXTENSIONSPOSTINIT = std::string (eglQueryString (m_eglContext.display, EGL_EXTENSIONS));

    if (CLIENTEXTENSIONSPOSTINIT.find ("EGL_KHR_create_context") == std::string::npos) {
	this->finishEGL ();
	sLog.exception ("EGL_KHR_create_context not supported!");
    }

    EGLint matchedConfigs = 0;
    const EGLint CONFIG_ATTRIBUTES[] = {
	EGL_SURFACE_TYPE,
	EGL_WINDOW_BIT,
	EGL_RED_SIZE,
	1,
	EGL_GREEN_SIZE,
	1,
	EGL_BLUE_SIZE,
	1,
	EGL_SAMPLES,
	this->m_context.settings.render.antiAliasing,
	EGL_RENDERABLE_TYPE,
	EGL_OPENGL_BIT,
	EGL_NONE,
    };

    if (!eglChooseConfig (m_eglContext.display, CONFIG_ATTRIBUTES, &m_eglContext.config, 1, &matchedConfigs)) {
	this->finishEGL ();
	sLog.exception ("eglChooseConfig failed!");
    }

    if (matchedConfigs == 0) {
	this->finishEGL ();
	sLog.exception ("eglChooseConfig failed! (matched 0 configs)");
    }

    if (!eglBindAPI (EGL_OPENGL_API)) {
	this->finishEGL ();
	sLog.exception ("eglBindAPI failed!");
    }

    const EGLint CONTEXT_ATTRIBUTES[] = {
	EGL_CONTEXT_MAJOR_VERSION_KHR,
	3,
	EGL_CONTEXT_MINOR_VERSION_KHR,
	3,
	EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
	EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
	EGL_NONE,
    };

    m_eglContext.context
	= eglCreateContext (m_eglContext.display, m_eglContext.config, EGL_NO_CONTEXT, CONTEXT_ATTRIBUTES);

    if (m_eglContext.context == EGL_NO_CONTEXT) {
	this->finishEGL ();
	sLog.error ("eglCreateContext error " + std::to_string (eglGetError ()));
	sLog.exception ("eglCreateContext failed!");
    }
}

void WaylandOpenGLDriver::finishEGL () const {
    eglMakeCurrent (EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_eglContext.display) {
	eglTerminate (m_eglContext.display);
    }
    eglReleaseThread ();
}

void WaylandOpenGLDriver::onLayerClose (Output::WaylandOutputViewport* viewport) {
    if (this->viewportInFocus == viewport) this->viewportInFocus = nullptr;
    if (viewport->frameCallback) wl_callback_destroy (viewport->frameCallback);
    if (viewport->eglSurface) {
        eglMakeCurrent (m_eglContext.display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext.context);
        eglDestroySurface (m_eglContext.display, viewport->eglSurface);
    }
    if (viewport->eglWindow) wl_egl_window_destroy (viewport->eglWindow);
    if (viewport->layerSurface) zwlr_layer_surface_v1_destroy (viewport->layerSurface);
    if (viewport->xdgOutput) zxdg_output_v1_destroy (viewport->xdgOutput);
    if (viewport->surface) wl_surface_destroy (viewport->surface);
    if (viewport->cursorSurface) wl_surface_destroy (viewport->cursorSurface);
    if (viewport->cursorTheme) wl_cursor_theme_destroy (viewport->cursorTheme);
    if (viewport->output) wl_output_release (viewport->output);
    std::erase (this->m_screens, viewport);
    this->getOutput ().reset ();
    delete viewport;
}

WaylandOpenGLDriver::WaylandOpenGLDriver (ApplicationContext& context, WallpaperApplication& app) :
    VideoDriver (app, m_mouseInput), m_output (context, *this), m_requestedExit (false), m_frameCounter (0),
    m_context (context), m_mouseInput (*this) {
    initWaylandRegistry ();
    initEGL ();
    setupOutputLayerSurfaces ();
    initGLEW ();
}

void WaylandOpenGLDriver::initWaylandRegistry () {
    m_waylandContext.display = wl_display_connect (nullptr);

    if (!m_waylandContext.display) {
	sLog.exception ("Failed to query wayland display");
    }

    m_waylandContext.registry = wl_display_get_registry (m_waylandContext.display);
    wl_registry_add_listener (m_waylandContext.registry, &registryListener, this);

    wl_display_dispatch (m_waylandContext.display);
    wl_display_roundtrip (m_waylandContext.display);

    if (!m_waylandContext.compositor || !m_waylandContext.shm || !m_waylandContext.layerShell
	|| this->m_screens.empty ()) {
	sLog.exception ("Failed to bind to required interfaces");
    }

    // If xdg-output-manager is available, use it to get logical output positions
    if (m_waylandContext.xdgOutputManager) {
	for (const auto& o : this->m_screens) {
	    o->setupXdgOutput (m_waylandContext.xdgOutputManager);
	}
	wl_display_roundtrip (m_waylandContext.display);
    } else if (!m_context.settings.general.spanGroups.empty ()) {
	sLog.error ("zxdg_output_manager_v1 is unavailable; screen-span positions will be incorrect.");
    }
}

void WaylandOpenGLDriver::setupOutputLayerSurfaces () {
    bool any = false;

    for (const auto& o : this->m_screens) {
	bool shouldSetup = m_context.settings.general.screenBackgrounds.contains (o->name);

	// also check if this screen is in any span group
	if (!shouldSetup) {
	    for (const auto& spanGroup : m_context.settings.general.spanGroups) {
		for (const auto& screen : spanGroup.screens) {
		    if (screen == o->name) {
			shouldSetup = true;
			break;
		    }
		}
		if (shouldSetup) {
		    break;
		}
	    }
	}

	if (!shouldSetup) {
	    continue;
	}

	o->setupLS ();
	any = true;
    }

    if (!any) {
	sLog.error ("No outputs could be initialized, please check the parameters and try again");
	sLog.error ("Detected outputs:");

	for (const auto& o : this->m_screens) {
	    sLog.error ("  ", o->name);
	}

	sLog.error ("Requested: ");

	for (const auto& o : m_context.settings.general.screenBackgrounds | std::views::keys) {
	    sLog.error ("  ", o);
	}

	for (const auto& spanGroup : m_context.settings.general.spanGroups) {
	    for (const auto& screen : spanGroup.screens) {
		sLog.error ("  ", screen, " (span group)");
	    }
	}

	sLog.exception ("Cannot continue...");
    }
}

void WaylandOpenGLDriver::initGLEW () {
    glewExperimental = GL_TRUE;
    if (const GLenum result = glewInit (); result != GLEW_OK) {
	if (result == GLEW_ERROR_NO_GLX_DISPLAY) {
	    sLog.out ("Failed to initialize GLEW, but continuing with EGL context: No GLX display");
	} else {
	    const char* error = reinterpret_cast<const char*> (glewGetErrorString (result));
	    sLog.error ("Failed to initialize GLEW: ", error ? error : "Unknown error");
	    sLog.exception ("Cannot continue...");
	}
    }
}

WaylandOpenGLDriver::~WaylandOpenGLDriver () {
    while (!this->m_screens.empty ()) this->onLayerClose (this->m_screens.back ());
    this->finishEGL ();
    if (this->m_waylandContext.display) wl_display_disconnect (this->m_waylandContext.display);
}

void WaylandOpenGLDriver::dispatchEventQueue () {
    for (const auto& viewport : this->m_screens) {
        if (!viewport->layerSurface && viewport->initialized
            && (this->m_context.settings.general.screenBackgrounds.contains (viewport->name)
                || std::ranges::any_of (m_context.settings.general.spanGroups, [&] (const auto& group) {
                    return std::ranges::find (group.screens, viewport->name) != group.screens.end ();
                }))) {
            if (!viewport->xdgOutput && m_waylandContext.xdgOutputManager)
                viewport->setupXdgOutput (m_waylandContext.xdgOutputManager);
            viewport->setupLS ();
        }
    }
    // Recover an initial or missed compositor callback. Policy pause uses SIGSTOP
    // in the supervisor, so a paused process does not enter this loop.
    const auto now = std::chrono::steady_clock::now ();
    for (const auto& viewport : this->m_screens) {
        if (viewport->layerSurface && now - viewport->lastSwap > std::chrono::seconds (1))
            this->getApp ().update (viewport);
    }
    auto* display = m_waylandContext.display;
    while (wl_display_prepare_read (display) != 0) {
        if (wl_display_dispatch_pending (display) == -1) { m_requestedExit = true; return; }
    }
    short events = POLLIN;
    if (wl_display_flush (display) == -1) {
        if (errno == EAGAIN) events |= POLLOUT;
        else { wl_display_cancel_read (display); m_requestedExit = true; return; }
    }
    pollfd fd {wl_display_get_fd (display), events, 0};
    const int ready = poll (&fd, 1, 100);
    if (ready > 0 && (fd.revents & POLLIN)) {
        if (wl_display_read_events (display) == -1) { m_requestedExit = true; return; }
    } else {
        wl_display_cancel_read (display);
    }
    if ((ready < 0 && errno != EINTR) || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        m_requestedExit = true; return;
    }
    if (fd.revents & POLLOUT) wl_display_flush (display);
    if (wl_display_dispatch_pending (display) == -1) { m_requestedExit = true; return; }
    this->m_framePacer.setFPS (this->m_context.settings.render.maximumFPS);
    const auto done = FramePacer::Clock::now ();
    if (const auto milliseconds = this->m_framePacer.sleepMilliseconds (done); milliseconds > 0)
        usleep (static_cast<useconds_t> (milliseconds * 1000));
    this->m_framePacer.woke (done, FramePacer::Clock::now ());
}

Output::Output& WaylandOpenGLDriver::getOutput () { return this->m_output; }

float WaylandOpenGLDriver::getRenderTime () const {
    return static_cast<float> (std::chrono::duration_cast<std::chrono::microseconds> (
				   std::chrono::high_resolution_clock::now () - renderStart
	   )
				   .count ())
	/ 1000000.0;
}

bool WaylandOpenGLDriver::closeRequested () { return this->m_requestedExit; }

void WaylandOpenGLDriver::resizeWindow (glm::ivec2 size) { }

void WaylandOpenGLDriver::resizeWindow (glm::ivec4 sizeandpos) { }

void WaylandOpenGLDriver::showWindow () { }

void WaylandOpenGLDriver::hideWindow () { }

glm::ivec2 WaylandOpenGLDriver::getFramebufferSize () const { return glm::ivec2 { 0, 0 }; }

uint32_t WaylandOpenGLDriver::getFrameCounter () const { return m_frameCounter; }

WaylandOpenGLDriver::SEGLContext* WaylandOpenGLDriver::getEGLContext () { return &this->m_eglContext; }

void* WaylandOpenGLDriver::getProcAddress (const char* name) const {
    return reinterpret_cast<void*> (eglGetProcAddress (name));
}

WaylandOpenGLDriver::WaylandContext* WaylandOpenGLDriver::getWaylandContext () { return &this->m_waylandContext; }

Output::WaylandOutputViewport* WaylandOpenGLDriver::surfaceToViewport (const wl_surface* surface) const {
    for (const auto& o : m_screens) {
	if (o->surface == surface) {
	    return o;
	}
    }

    return nullptr;
}

__attribute__ ((constructor)) void registerWaylandOpenGL () {
    sVideoFactories.registerDriver (
	ApplicationContext::DESKTOP_BACKGROUND, "wayland",
	[] (ApplicationContext& context, WallpaperApplication& application) -> std::unique_ptr<VideoDriver> {
	    return std::make_unique<WaylandOpenGLDriver> (context, application);
	}
    );
}
