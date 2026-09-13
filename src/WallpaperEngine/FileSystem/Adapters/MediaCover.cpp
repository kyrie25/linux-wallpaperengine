#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <curl/curl.h>

#include "MediaCover.h"

#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Media/MediaSource.h"

using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::FileSystem::Adapters;

namespace {
size_t appendResponse (char* data, size_t size, size_t count, void* userData) {
    const size_t bytes = size * count;
    static_cast<std::string*> (userData)->append (data, bytes);
    return bytes;
}

ReadStreamSharedPtr downloadCover (const std::string& url) {
    static const bool curlInitialized = curl_global_init (CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curlInitialized) {
	throw std::filesystem::filesystem_error ("Could not initialize libcurl", url, std::error_code ());
    }

    CURL* curl = curl_easy_init ();
    if (curl == nullptr) {
	throw std::filesystem::filesystem_error ("Could not create libcurl request", url, std::error_code ());
    }

    std::string contents;
    curl_easy_setopt (curl, CURLOPT_URL, url.c_str ());
    curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt (curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt (curl, CURLOPT_WRITEDATA, &contents);

    const CURLcode result = curl_easy_perform (curl);
    long status = 0;
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup (curl);

    if (result != CURLE_OK || status < 200 || status >= 300 || contents.empty ()) {
	throw std::filesystem::filesystem_error ("Could not download media cover", url, std::error_code ());
    }

    return std::make_shared<std::istringstream> (std::move (contents), std::ios::in | std::ios::binary);
}
} // namespace

ReadStreamSharedPtr MediaCoverAdapter::open (const std::filesystem::path& path) const {
    if (path != "$mediaThumbnail") {
	throw std::filesystem::filesystem_error (
	    "MediaCoverAdapter only supports $mediaThumbnail", path, std::error_code ()
	);
    }

    if (!source.getMediaInfo ().url.has_value ()) {
	throw std::filesystem::filesystem_error ("Media source does not have a valid URL", path, std::error_code ());
    }

    std::string album = *source.getMediaInfo ().url;

    if (album.starts_with ("file://")) {
	album = album.substr (7);
    } else if (album.starts_with ("http://") || album.starts_with ("https://")) {
	return downloadCover (album);
    } else {
	throw std::filesystem::filesystem_error (
	    "Unsupported URL for media cover", album, std::error_code ()
	);
    }

    std::filesystem::path file = std::filesystem::absolute (album);

    if (std::filesystem::exists (file) == false) {
	throw std::filesystem::filesystem_error ("Media file does not exist", file, std::error_code ());
    }

    if (std::filesystem::is_regular_file (file) == false) {
	throw std::filesystem::filesystem_error ("Media file is not a regular file", file, std::error_code ());
    }

    return std::make_shared<std::ifstream> (file);
}

bool MediaCoverAdapter::exists (const std::filesystem::path& path) const { return path == ""; }

std::filesystem::path MediaCoverAdapter::physicalPath (const std::filesystem::path& path) const {
    sLog.exception ("MediaCoverAdapter does not support realpath");
}

bool MediaCoverFactory::handlesMountpoint (const std::filesystem::path& path) const {
    return path == "$mediaThumbnail";
}

AdapterSharedPtr MediaCoverFactory::create (const std::filesystem::path& path) const {
    if (path != "$mediaThumbnail") {
	sLog.exception ("MediaCoveradapter only supports $mediaThumbnail");
    }

    return std::make_unique<MediaCoverAdapter> (source);
}
