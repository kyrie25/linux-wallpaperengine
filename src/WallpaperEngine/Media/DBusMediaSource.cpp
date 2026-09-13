#include "DBusMediaSource.h"

#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cmath>

using namespace WallpaperEngine::Media;

namespace {
constexpr double kMicrosecondsPerSecond = 1000000.0;
}

DBusHandlerResult dbus_message_filter (DBusConnection* connection, DBusMessage* message, void* user_data) {
    const auto mediaSource = static_cast<DBusMediaSource*> (user_data);

    if (!dbus_message_is_signal (message, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char* interface = nullptr;
    DBusMessageIter iter;

    dbus_message_iter_init (message, &iter);
    dbus_message_iter_get_basic (&iter, &interface);

    std::string iface = interface ?: "";

    if (iface != "org.mpris.MediaPlayer2.Player") {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessageIter changed;

    dbus_message_iter_next (&iter);
    dbus_message_iter_recurse (&iter, &changed);

    while (dbus_message_iter_get_arg_type (&changed) == DBUS_TYPE_DICT_ENTRY) {
	DBusMessageIter entry;
	dbus_message_iter_recurse (&changed, &entry);

	const char* key = nullptr;
	dbus_message_iter_get_basic (&entry, &key);

	std::string keyStr = key ?: "";

	DBusMessageIter value;
	dbus_message_iter_next (&entry);
	dbus_message_iter_recurse (&entry, &value);

	if (keyStr == "Metadata") {
	    mediaSource->parseMetadata (value, dbus_message_get_sender (message));
	} else if (keyStr == "PlaybackStatus") {
	    mediaSource->parsePlaybackStatus (value, dbus_message_get_sender (message));
	}

	dbus_message_iter_next (&changed);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

DBusMediaSource::DBusMediaSource (std::chrono::milliseconds updateInterval) : MediaSource (updateInterval) {
    DBusError err;

    dbus_error_init (&err);

    this->m_connection = dbus_bus_get (DBUS_BUS_SESSION, &err);

    if (!this->m_connection) {
	sLog.exception ("Could not connect to DBus: ", err.message);
    }

    dbus_connection_add_filter (this->m_connection, dbus_message_filter, this, nullptr);

    dbus_bus_add_match (
	this->m_connection, "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
	nullptr
    );

    dbus_connection_flush (this->m_connection);

    this->detectPlayer ();
    this->initialStatusFetch ();
    this->m_pendingMetadataFetch = false;
}

DBusMediaSource::~DBusMediaSource () {
    dbus_connection_remove_filter (this->m_connection, dbus_message_filter, this);

    dbus_connection_unref (this->m_connection);
}

void DBusMediaSource::parseMetadata (DBusMessageIter& variant, const char* sender) {
    if (sender != nullptr && (!this->m_currentPlayer.has_value () || *this->m_currentPlayer != sender)) {
	return;
    }

    DBusMessageIter dict;
    dbus_message_iter_recurse (&variant, &dict);

    bool metadataUpdate = false;
    bool albumUpdate = false;

    while (dbus_message_iter_get_arg_type (&dict) == DBUS_TYPE_DICT_ENTRY) {
	DBusMessageIter entry;
	dbus_message_iter_recurse (&dict, &entry);

	const char* key = nullptr;
	dbus_message_iter_get_basic (&entry, &key);

	std::string keyStr = key ?: "";

	DBusMessageIter value;
	dbus_message_iter_next (&entry);
	dbus_message_iter_recurse (&entry, &value);

	if (keyStr == "xesam:title") {
	    const char* title = nullptr;
	    dbus_message_iter_get_basic (&value, &title);

	    std::string titleStr = title ?: "";

	    if (this->m_mediaInfo.title != titleStr) {
		this->m_mediaInfo.title = titleStr;
		metadataUpdate = true;
	    }
	} else if (keyStr == "xesam:artist") {
	    DBusMessageIter arr;

	    dbus_message_iter_recurse (&value, &arr);

	    if (dbus_message_iter_get_arg_type (&arr) == DBUS_TYPE_STRING) {
		const char* artist = nullptr;
		dbus_message_iter_get_basic (&arr, &artist);

		std::string artistStr = artist ?: "";

		if (this->m_mediaInfo.artist != artistStr) {
		    this->m_mediaInfo.artist = artistStr;
		    metadataUpdate = true;
		}
	    }
	} else if (keyStr == "xesam:album") {
	    const char* album = nullptr;
	    dbus_message_iter_get_basic (&value, &album);

	    std::string albumStr = album ?: "";

	    if (this->m_mediaInfo.album != albumStr) {
		this->m_mediaInfo.album = albumStr;
		albumUpdate = true;
	    }
	} else if (keyStr == "mpris:artUrl") {
	    const char* artUrl = nullptr;
	    dbus_message_iter_get_basic (&value, &artUrl);

	    std::string artUrlStr = artUrl ?: "";

	    if (artUrlStr.empty () && this->m_mediaInfo.url.has_value ()) {
		this->m_mediaInfo.url.reset ();
		albumUpdate = true;
	    } else if (this->m_mediaInfo.url.has_value () && artUrlStr != *this->m_mediaInfo.url) {
		this->m_mediaInfo.url = artUrlStr;
		albumUpdate = true;
	    } else if (!this->m_mediaInfo.url.has_value ()) {
		this->m_mediaInfo.url = artUrlStr;
		albumUpdate = true;
	    }
	} else if (keyStr == "mpris:length") {
	    int64_t length = 0;
	    dbus_message_iter_get_basic (&value, &length);
	    const double duration = std::floor (static_cast<double> (length) / kMicrosecondsPerSecond);

	    if (this->m_mediaInfo.duration != duration) {
		this->m_mediaInfo.duration = duration;
		metadataUpdate = true;
	    }
	}

	dbus_message_iter_next (&dict);
    }

    sLog.debug (
	"Player metadata received: title=", this->m_mediaInfo.title, ",artist=", this->m_mediaInfo.artist,
	",album=", this->m_mediaInfo.album, ",url=", this->m_mediaInfo.url.has_value () ? *this->m_mediaInfo.url : ""
    );

    if (metadataUpdate) {
	this->fireMetadataListeners ();
    }

    if (albumUpdate) {
	this->fireAlbumArtListeners ();
    }
}

void DBusMediaSource::parsePlaybackStatus (DBusMessageIter& variant, const char* sender) {
    if (sender == nullptr) {
	return;
    }

    const char* status = nullptr;
    dbus_message_iter_get_basic (&variant, &status);
    std::string statusStr = status ?: "";
    PlaybackState newState = PlaybackState::Stopped;

    if (statusStr == "Playing") {
	newState = PlaybackState::Playing;
    } else if (statusStr == "Paused") {
	newState = PlaybackState::Paused;
    }

    if (newState == PlaybackState::Playing && (!this->m_currentPlayer.has_value () || *this->m_currentPlayer != sender)) {
	this->selectPlayer (sender, newState);
	this->m_pendingMetadataFetch = true;
	return;
    }

    if (!this->m_currentPlayer.has_value () || *this->m_currentPlayer != sender) {
	return;
    }

    if (newState != this->m_mediaInfo.playbackState) {
	this->m_mediaInfo.playbackState = newState;
	this->fireMetadataListeners ();
    }
}

void DBusMediaSource::parsePosition (DBusMessageIter& variant) {
    int64_t position = 0;
    dbus_message_iter_get_basic (&variant, &position);

    const double positionSeconds = std::floor (static_cast<double> (position) / kMicrosecondsPerSecond);

    if (this->m_mediaInfo.position != positionSeconds) {
	this->m_mediaInfo.position = positionSeconds;
	this->fireMetadataListeners ();
    }
}

void DBusMediaSource::update () {
    // drain any dbus events
    dbus_connection_read_write (this->m_connection, 0);

    while (dbus_connection_dispatch (this->m_connection) == DBUS_DISPATCH_DATA_REMAINS)
	;

    if (this->m_pendingMetadataFetch) {
	this->initialStatusFetch ();
	this->m_pendingMetadataFetch = false;
    }

    this->MediaSource::update ();
}

DBusMessage* DBusMediaSource::dbusMessage (
    const char* bus_name, const char* path, const char* interface, const char* method, const char* iface,
    const char* prop
) {
    DBusError err;
    dbus_error_init (&err);

    DBusMessage* msg = dbus_message_new_method_call (bus_name, path, interface, method);
    Data::Utils::ScopeGuard guard ([msg] { dbus_message_unref (msg); });

    if (iface != nullptr && prop != nullptr) {
	dbus_message_append_args (msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    }

    DBusMessage* reply = dbus_connection_send_with_reply_and_block (m_connection, msg, -1, &err);

    if (reply == nullptr) {
	sLog.error ("DBus error: ", err.message, " (", err.name, ")");
	return nullptr;
    }

    return reply;
}

std::optional<DBusMediaSource::PlaybackState> DBusMediaSource::playbackState (const std::string& player) {
    DBusMessage* reply = this->dbusMessage (
	player.c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	"org.mpris.MediaPlayer2.Player", "PlaybackStatus"
    );
    if (reply == nullptr) {
	return std::nullopt;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    DBusMessageIter outer;
    dbus_message_iter_init (reply, &outer);
    DBusMessageIter variant;
    dbus_message_iter_recurse (&outer, &variant);

    const char* status = nullptr;
    dbus_message_iter_get_basic (&variant, &status);
    const std::string statusStr = status ?: "";
    if (statusStr == "Playing") {
	return PlaybackState::Playing;
    }
    if (statusStr == "Paused") {
	return PlaybackState::Paused;
    }
    return PlaybackState::Stopped;
}

std::optional<std::string> DBusMediaSource::playerOwner (const std::string& player) {
    DBusMessage* message = dbus_message_new_method_call (
	"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner"
    );
    if (message == nullptr) {
	return std::nullopt;
    }
    Data::Utils::ScopeGuard messageGuard ([message] { dbus_message_unref (message); });

    const char* playerName = player.c_str ();
    dbus_message_append_args (message, DBUS_TYPE_STRING, &playerName, DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init (&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block (this->m_connection, message, -1, &err);
    if (reply == nullptr) {
	if (dbus_error_is_set (&err)) {
	    dbus_error_free (&err);
	}
	return std::nullopt;
    }
    Data::Utils::ScopeGuard replyGuard ([reply] { dbus_message_unref (reply); });

    const char* owner = nullptr;
    if (!dbus_message_get_args (reply, &err, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID)) {
	if (dbus_error_is_set (&err)) {
	    dbus_error_free (&err);
	}
	return std::nullopt;
    }
    return std::string (owner ?: "");
}

void DBusMediaSource::selectPlayer (const std::string& player, PlaybackState state) {
    const bool playerChanged = !this->m_currentPlayer.has_value () || *this->m_currentPlayer != player;
    if (!playerChanged) {
	if (!this->m_mediaInfo.available || this->m_mediaInfo.playbackState != state) {
	    this->m_mediaInfo.available = true;
	    this->m_mediaInfo.playbackState = state;
	    this->fireMetadataListeners ();
	}
	return;
    }

    const bool hadAlbumArt = this->m_mediaInfo.url.has_value ();
    this->m_currentPlayer = player;
    this->m_mediaInfo = {
	.playbackState = state,
	.title = "",
	.artist = "",
	.album = "",
	.url = std::nullopt,
	.duration = 0.0,
	.position = 0.0,
	.available = true,
    };
    this->fireMetadataListeners ();
    if (hadAlbumArt) {
	this->fireAlbumArtListeners ();
    }
}

void DBusMediaSource::clearPlayer () {
    if (!this->m_currentPlayer.has_value () && !this->m_mediaInfo.available) {
	return;
    }

    const bool hadAlbumArt = this->m_mediaInfo.url.has_value ();
    this->m_currentPlayer.reset ();
    this->m_mediaInfo = {
	.playbackState = PlaybackState::Stopped,
	.title = "",
	.artist = "",
	.album = "",
	.url = std::nullopt,
	.duration = 0.0,
	.position = 0.0,
	.available = false,
    };
    this->fireMetadataListeners ();
    if (hadAlbumArt) {
	this->fireAlbumArtListeners ();
    }
}

void DBusMediaSource::detectPlayer () {
    DBusMessage* reply
	= this->dbusMessage ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames");
    if (reply == nullptr) {
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    std::vector<std::string> players;
    DBusMessageIter iter;
    dbus_message_iter_init (reply, &iter);
    if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) {
	return;
    }

    DBusMessageIter array;
    dbus_message_iter_recurse (&iter, &array);
    while (dbus_message_iter_get_arg_type (&array) != DBUS_TYPE_INVALID) {
	char* name = nullptr;
	dbus_message_iter_get_basic (&array, &name);
	const std::string service = name ?: "";
	if (service.starts_with ("org.mpris.MediaPlayer2.")) {
	    players.push_back (service);
	}
	dbus_message_iter_next (&array);
    }

    std::sort (players.begin (), players.end ());

    struct Candidate {
	std::string owner;
	PlaybackState state;
    };
    std::vector<Candidate> candidates;
    for (const auto& player : players) {
	const auto state = this->playbackState (player);
	const auto owner = this->playerOwner (player);
	if (state.has_value () && owner.has_value ()) {
	    candidates.push_back ({ *owner, *state });
	}
    }

    if (candidates.empty ()) {
	this->clearPlayer ();
	return;
    }

    const auto current = std::find_if (candidates.begin (), candidates.end (), [this] (const Candidate& candidate) {
	return this->m_currentPlayer.has_value () && candidate.owner == *this->m_currentPlayer;
    });
    const auto playing = std::find_if (candidates.begin (), candidates.end (), [] (const Candidate& candidate) {
	return candidate.state == PlaybackState::Playing;
    });
    const auto paused = std::find_if (candidates.begin (), candidates.end (), [] (const Candidate& candidate) {
	return candidate.state == PlaybackState::Paused;
    });

    auto selected = candidates.begin ();
    if (current != candidates.end () && current->state == PlaybackState::Playing) {
	selected = current;
    } else if (playing != candidates.end ()) {
	selected = playing;
    } else if (current != candidates.end ()) {
	selected = current;
    } else if (paused != candidates.end ()) {
	selected = paused;
    }

    const bool playerChanged = !this->m_currentPlayer.has_value () || *this->m_currentPlayer != selected->owner;
    this->selectPlayer (selected->owner, selected->state);
    this->m_pendingMetadataFetch = this->m_pendingMetadataFetch || playerChanged;
}

void DBusMediaSource::initialStatusFetch () {
    if (this->m_currentPlayer.has_value () == false) {
	return;
    }

    DBusMessage* reply = this->dbusMessage (
	this->m_currentPlayer.value ().c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	"org.mpris.MediaPlayer2.Player", "Metadata"
    );

    if (reply == nullptr) {
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    DBusMessageIter outer;
    dbus_message_iter_init (reply, &outer);

    DBusMessageIter variant;
    dbus_message_iter_recurse (&outer, &variant);

    this->parseMetadata (variant);
}

void DBusMediaSource::performUpdate () {
    this->detectPlayer ();

    // nothing to do if no player is detected
    if (!this->m_currentPlayer.has_value ()) {
	return;
    }

    this->initialStatusFetch ();
    this->m_pendingMetadataFetch = false;

    DBusMessage* reply = this->dbusMessage (
	this->m_currentPlayer.value ().c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	"org.mpris.MediaPlayer2.Player", "Position"
    );

    if (reply == nullptr) {
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    DBusMessageIter outer;
    dbus_message_iter_init (reply, &outer);

    DBusMessageIter variant;
    dbus_message_iter_recurse (&outer, &variant);

    dbus_int64_t position = 0;
    dbus_message_iter_get_basic (&variant, &position);

    const double positionSeconds = std::floor (static_cast<double> (position) / kMicrosecondsPerSecond);

    if (this->m_mediaInfo.position != positionSeconds) {
	this->m_mediaInfo.position = positionSeconds;
	this->fireMetadataListeners ();
    }
}
