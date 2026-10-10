#include "WaylandMouseInput.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Render/Drivers/WaylandOpenGLDriver.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <glm/common.hpp>
#include <regex>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

using namespace WallpaperEngine::Input::Drivers;

WaylandMouseInput::WaylandMouseInput (const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& driver) :
    m_waylandDriver (driver) { }

void WaylandMouseInput::update () {
    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	this->m_globalCursorPosition.reset ();
	this->m_forwardedLeftDown = false;
	return;
    }

    const auto now = std::chrono::steady_clock::now ();
    if (now - this->m_lastHyprlandQuery < std::chrono::milliseconds (16)) {
	return;
    }
    this->m_lastHyprlandQuery = now;

    this->m_globalCursorPosition = this->queryHyprlandCursorPosition ();
    this->m_forwardedLeftDown = false;
    const auto& path = this->m_waylandDriver.getApp ().getContext ().settings.mouse.inputFile;
    if (!path.empty ()) {
	try {
	    // The shell refreshes a held button; expiry releases it if the shell disappears.
	    if (std::filesystem::file_time_type::clock::now () - std::filesystem::last_write_time (path)
		< std::chrono::seconds (2)) {
		std::ifstream input (path);
		const auto state = WallpaperEngine::Data::JSON::JSON::parse (input);
		this->m_forwardedLeftDown = state.value ("leftDown", false);
	    }
	} catch (const std::exception&) {
	    // A missing or partially replaced input file represents a released button.
	}
    }
}

glm::dvec2 WaylandMouseInput::position () const {
    const auto* viewport = this->getActiveOutputViewport ();

    if (!viewport) {
	return { 0, 0 };
    }

    if (this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	if (viewport == m_waylandDriver.viewportInFocus) {
	    return viewport->mousePos;
	}
	if (this->m_globalCursorPosition.has_value () && viewport->logicalSize.x > 0 && viewport->logicalSize.y > 0) {
	    // Hyprland reports logical desktop coordinates; convert for the output currently rendering.
	    const glm::dvec2 local = *this->m_globalCursorPosition - glm::dvec2 (viewport->globalPosition);
	    return {
		glm::clamp (local.x / viewport->logicalSize.x, 0.0, 1.0) * viewport->viewport.z,
		(1.0 - glm::clamp (local.y / viewport->logicalSize.y, 0.0, 1.0)) * viewport->viewport.w,
	    };
	}
    }

    return {
	static_cast<double> (viewport->viewport.z) / 2.0,
	static_cast<double> (viewport->viewport.w) / 2.0,
    };
}

WallpaperEngine::Input::MouseClickStatus WaylandMouseInput::leftClick () const {
    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	return MouseClickStatus::Released;
    }
    if (this->m_forwardedLeftDown) {
	return MouseClickStatus::Clicked;
    }
    const auto* viewport = this->getActiveOutputViewport ();
    if (viewport) {
	return viewport->leftClick;
    }

    return MouseClickStatus::Released;
}

const WallpaperEngine::Render::Drivers::Output::WaylandOutputViewport*
WaylandMouseInput::getActiveOutputViewport () const {
    if (this->m_waylandDriver.viewportInFocus && this->m_waylandDriver.viewportInFocus->rendering) {
	return this->m_waylandDriver.viewportInFocus;
    }

    for (const auto* viewport : this->m_waylandDriver.m_screens) {
	if (viewport && viewport->rendering) {
	    return viewport;
	}
    }

    return nullptr;
}

std::optional<glm::dvec2> WaylandMouseInput::queryHyprlandCursorPosition () const {
    const char* signature = std::getenv ("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = std::getenv ("XDG_RUNTIME_DIR");
    if (!signature || !runtime) {
	return std::nullopt;
    }

    const std::string socketPath = std::string (runtime) + "/hypr/" + signature + "/.socket.sock";

    int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
	return std::nullopt;
    }

    timeval timeout {};
    timeout.tv_usec = 50000;
    if (setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout)) != 0
	|| setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof (timeout)) != 0) {
	close (fd);
	return std::nullopt;
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (socketPath.size () >= sizeof (addr.sun_path)) {
	close (fd);
	return std::nullopt;
    }
    std::strncpy (addr.sun_path, socketPath.c_str (), sizeof (addr.sun_path) - 1);

    if (connect (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) != 0) {
	close (fd);
	return std::nullopt;
    }

    constexpr const char* request = "j/cursorpos";
    if (send (fd, request, std::strlen (request), MSG_NOSIGNAL) < 0) {
	close (fd);
	return std::nullopt;
    }
    shutdown (fd, SHUT_WR);

    std::string response;
    char buffer[256];
    ssize_t readBytes = 0;
    while ((readBytes = recv (fd, buffer, sizeof (buffer), 0)) > 0) {
	response.append (buffer, static_cast<std::size_t> (readBytes));
    }
    close (fd);

    static const std::regex xRegex (R"("x"\s*:\s*(-?\d+(?:\.\d+)?))");
    static const std::regex yRegex (R"("y"\s*:\s*(-?\d+(?:\.\d+)?))");
    std::smatch xMatch;
    std::smatch yMatch;
    if (!std::regex_search (response, xMatch, xRegex) || !std::regex_search (response, yMatch, yRegex)) {
	return std::nullopt;
    }

    try {
	return glm::dvec2 { std::stod (xMatch[1].str ()), std::stod (yMatch[1].str ()) };
    } catch (const std::exception&) {
	return std::nullopt;
    }
}

WallpaperEngine::Input::MouseClickStatus WaylandMouseInput::rightClick () const {
    const auto* viewport = this->getActiveOutputViewport ();

    if (viewport) {
	return viewport->rightClick;
    }

    return MouseClickStatus::Released;
}
