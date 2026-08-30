#include "locationtracking.h"

#include "aptracker.h"
#include "../core/tracker.h"
#include <luaglue/lua_json.h>
#include <cmath>
#include <cstdio>
#include <limits>
#include <list>
#include <unordered_set>

using nlohmann::json;

namespace {

bool replaceAll(std::string& value, const std::string& from, const std::string& to)
{
    bool replaced = false;
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.length(), to);
        pos += to.length();
        replaced = true;
    }
    return replaced;
}

bool getStringField(lua_State* L, int table, const char* name, std::string& output,
        bool required, bool nonEmptyWhenPresent = false,
        size_t maxLength = std::numeric_limits<size_t>::max())
{
    lua_getfield(L, table, name);
    const int type = lua_type(L, -1);
    if (type == LUA_TNIL && !required) {
        lua_pop(L, 1);
        return true;
    }
    if (type != LUA_TSTRING) {
        lua_pop(L, 1);
        return false;
    }
    size_t length = 0;
    const char* value = lua_tolstring(L, -1, &length);
    if (length > maxLength) {
        lua_pop(L, 1);
        return false;
    }
    output.assign(value, length);
    lua_pop(L, 1);
    return (required || nonEmptyWhenPresent) ? !output.empty() : true;
}

bool getCoordinateField(lua_State* L, int table, const char* name, float& output)
{
    lua_getfield(L, table, name);
    if (lua_type(L, -1) != LUA_TNUMBER) {
        lua_pop(L, 1);
        return false;
    }
    const lua_Number value = lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (!std::isfinite(value) || value < -std::numeric_limits<float>::max() ||
            value > std::numeric_limits<float>::max())
        return false;
    output = static_cast<float>(value);
    return true;
}

bool getVisibleField(lua_State* L, int table, bool& visible)
{
    lua_getfield(L, table, "visible");
    const int type = lua_type(L, -1);
    if (type == LUA_TNIL) {
        lua_pop(L, 1);
        return true;
    }
    if (type != LUA_TBOOLEAN) {
        lua_pop(L, 1);
        return false;
    }
    visible = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return true;
}

bool isSequence(lua_State* L, int table, size_t length)
{
    for (size_t i = 1; i <= length; ++i) {
        lua_rawgeti(L, table, static_cast<lua_Integer>(i));
        const bool present = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (!present)
            return false;
    }
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        const bool valid = lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1 &&
                static_cast<size_t>(lua_tointeger(L, -2)) <= length;
        lua_pop(L, 1);
        if (!valid) {
            lua_pop(L, 1);
            return false;
        }
    }
    return true;
}

} // namespace

LocationTracking::LocationTracking(lua_State* L, APTracker* ap, Tracker* tracker)
    : _L(L), _ap(ap), _tracker(tracker)
{
}

LocationTracking::~LocationTracking()
{
    clearMarkers();
    if (_ap) {
        _ap->onClear -= this;
        _ap->onStateChanged -= this;
        _ap->onRetrieved -= this;
        _ap->onSetReply -= this;
    }
    if (_tracker)
        _tracker->onPostUiHintReset -= this;
    if (_L && _callback != LUA_NOREF)
        luaL_unref(_L, LUA_REGISTRYINDEX, _callback);
}

std::string LocationTracking::resolveSettingKey(const std::string& keyTemplate, int team, int player)
{
    std::string key = keyTemplate;
    const bool teamReplaced = replaceAll(key, "{team}", std::to_string(team));
    const bool playerReplaced = replaceAll(key, "{player}", std::to_string(player));
    return teamReplaced && playerReplaced ? key : "";
}

bool LocationTracking::discover()
{
    if (!_L || !_ap || !_tracker || _active)
        return false;

    lua_getglobal(_L, "LOCATION_TRACKING");
    if (lua_type(_L, -1) != LUA_TTABLE) {
        lua_pop(_L, 1);
        return false;
    }

    lua_getfield(_L, -1, "api_version");
    const bool supportedVersion = lua_isinteger(_L, -1) && lua_tointeger(_L, -1) == 1;
    lua_pop(_L, 1);
    if (!supportedVersion || !getStringField(_L, -1, "location_setting_key", _keyTemplate, true,
            false, MAX_MAP_LENGTH * 4)) {
        lua_pop(_L, 1);
        return false;
    }
    lua_getfield(_L, -1, "location_markers");
    if (lua_type(_L, -1) != LUA_TFUNCTION) {
        lua_pop(_L, 2);
        return false;
    }
    _callback = luaL_ref(_L, LUA_REGISTRYINDEX);
    lua_pop(_L, 1); // LOCATION_TRACKING
    _active = true;

    _ap->onClear += {this, [this](void*, const json&) { subscribe(); }};
    _ap->onStateChanged += {this, [this](void*, APClient::State state) {
        if (state == APClient::State::DISCONNECTED) {
            // APTracker deletes the old APClient before emitting this state, and
            // AP client callbacks are synchronous under APTracker's EventLock.
            // Therefore an old connection cannot deliver a later same-key value;
            // clearing the key also fences any subsequent signal dispatch.
            clearMarkers();
            _activeKey.clear();
            _team = -1;
            _player = -1;
        }
    }};
    _ap->onRetrieved += {this, [this](void*, const std::string& key, const json& value) {
        handleValue(key, value);
    }};
    _ap->onSetReply += {this, [this](void*, const std::string& key, const json& value, const json&) {
        handleValue(key, value);
    }};
    _tracker->onPostUiHintReset += {this, [this](void*) { renderMarkers(); }};

    // A pack can load after the AP slot is already connected (for example when
    // switching packs). onClear has already run in that case, so subscribe now.
    subscribe();
    return true;
}

void LocationTracking::subscribe()
{
    if (!_active || !_ap)
        return;
    clearMarkers();
    _activeKey.clear();
    _team = _ap->getTeamNumber();
    _player = _ap->getPlayerNumber();
    if (_team < 0 || _player < 0)
        return;
    _activeKey = resolveSettingKey(_keyTemplate, _team, _player);
    if (_activeKey.empty()) {
        fprintf(stderr, "LOCATION_TRACKING: location_setting_key must contain {team} and {player}.\n");
        return;
    }
    const std::list<std::string> keys = {_activeKey};
    _ap->SetNotify(keys);
    _ap->Get(keys);
}

void LocationTracking::handleValue(const std::string& key, const json& value)
{
    if (_active && !key.empty() && key == _activeKey)
        update(value);
}

std::string LocationTracking::markerId(const std::string& id) const
{
    // MapMarker has a string-only identity. This is serialization at the UI
    // boundary; lifecycle state remains the source id plus _team/_player.
    return "location-tracking:" + std::to_string(_team) + ":" + std::to_string(_player) + ":" + id;
}

void LocationTracking::clearMarkers()
{
    if (!_tracker)
        return;
    for (const auto& marker : _markers) {
        _tracker->UiHint("MapMarker " + marker.second.map,
                json({{"id", markerId(marker.first)}, {"remove", true}}).dump());
    }
    _markers.clear();
}

void LocationTracking::renderMarkers()
{
    if (!_tracker)
        return;
    for (const auto& marker : _markers) {
        const auto& placement = marker.second;
        json hint = {{"id", markerId(marker.first)}, {"x", placement.x}, {"y", placement.y}};
        if (!placement.label.empty())
            hint["label"] = placement.label;
        _tracker->UiHint("MapMarker " + placement.map, hint.dump());
    }
}

bool LocationTracking::update(const json& value)
{
    if (!_active || !_L || _callback == LUA_NOREF)
        return false;
    if (!lua_checkstack(_L, 4))
        return false;

    lua_pushcfunction(_L, Tracker::luaErrorHandler);
    lua_rawgeti(_L, LUA_REGISTRYINDEX, _callback);
    try {
        json_to_lua(_L, value);
    } catch (const std::exception& e) {
        fprintf(stderr, "LOCATION_TRACKING: could not pass DataStorage value to Lua: %s\n", e.what());
        lua_pop(_L, 2);
        return false;
    }
    lua_sethook(_L, Tracker::luaTimeoutHook, LUA_MASKCOUNT, Tracker::DEFAULT_EXEC_LIMIT);
    const int status = lua_pcall(_L, 1, 1, -3);
    lua_sethook(_L, nullptr, 0, 0);
    if (status != LUA_OK) {
        const char* error = lua_tostring(_L, -1);
        fprintf(stderr, "LOCATION_TRACKING: error in location_markers: %s\n", error ? error : "Unknown error");
        lua_pop(_L, 2);
        return false;
    }
    if (lua_isnil(_L, -1)) {
        lua_pop(_L, 2);
        clearMarkers();
        return true;
    }
    if (lua_type(_L, -1) != LUA_TTABLE) {
        fprintf(stderr, "LOCATION_TRACKING: location_markers must return a sequence or nil.\n");
        lua_pop(_L, 2);
        return false;
    }

    const int markersTable = lua_gettop(_L);
    const size_t length = lua_rawlen(_L, markersTable);
    if (length > MAX_MARKERS) {
        fprintf(stderr, "LOCATION_TRACKING: location_markers returned %zu markers; maximum is %zu.\n",
                length, MAX_MARKERS);
        lua_pop(_L, 2);
        return false;
    }
    if (!isSequence(_L, markersTable, length)) {
        fprintf(stderr, "LOCATION_TRACKING: location_markers must return a dense sequence.\n");
        lua_pop(_L, 2);
        return false;
    }

    std::unordered_map<std::string, Marker> next;
    std::unordered_set<std::string> seenIds;
    bool validCallback = true;
    size_t invalidRows = 0;
    for (size_t i = 1; validCallback && i <= length; ++i) {
        lua_rawgeti(_L, markersTable, static_cast<lua_Integer>(i));
        if (lua_type(_L, -1) != LUA_TTABLE) {
            ++invalidRows;
        } else {
            Marker marker;
            std::string id;
            bool visible = true;
            const bool hasId = getStringField(_L, -1, "id", id, true, false, MAX_ID_LENGTH);
            const bool hasVisible = hasId && getVisibleField(_L, -1, visible);
            if (!hasVisible) {
                ++invalidRows;
            } else if (!seenIds.emplace(id).second) {
                validCallback = false; // duplicate IDs make removal ambiguous
            } else if (!(getStringField(_L, -1, "map", marker.map, true, false, MAX_MAP_LENGTH) &&
                    getCoordinateField(_L, -1, "x", marker.x) &&
                    getCoordinateField(_L, -1, "y", marker.y) &&
                    getStringField(_L, -1, "label", marker.label, false, true, MAX_LABEL_LENGTH))) {
                ++invalidRows;
            } else if (visible) {
                next.emplace(id, std::move(marker));
            }
        }
        lua_pop(_L, 1);
    }
    lua_pop(_L, 2); // result + error handler
    if (!validCallback) {
        fprintf(stderr, "LOCATION_TRACKING: duplicate marker ID; retaining the previous placement snapshot.\n");
        return false;
    }
    if (invalidRows) {
        fprintf(stderr, "LOCATION_TRACKING: ignored %zu malformed marker row%s.\n",
                invalidRows, invalidRows == 1 ? "" : "s");
    }

    // Remove the old map placement before writing a moved marker. Markers that
    // disappear or become invisible must likewise be removed from their old map.
    for (const auto& oldMarker : _markers) {
        const auto nextMarker = next.find(oldMarker.first);
        if (nextMarker == next.end() || nextMarker->second.map != oldMarker.second.map) {
            _tracker->UiHint("MapMarker " + oldMarker.second.map,
                    json({{"id", markerId(oldMarker.first)}, {"remove", true}}).dump());
        }
    }
    _markers = std::move(next);
    renderMarkers();
    return true;
}
