#ifndef _AP_LOCATIONTRACKING_H
#define _AP_LOCATIONTRACKING_H

#include <nlohmann/json.hpp>
#include <luaglue/luaref.h>
#include <cstddef>
#include <string>
#include <unordered_map>

class APTracker;
class Tracker;

/// Native host for a pack's optional LOCATION_TRACKING Lua contract.
///
/// A compatible pack supplies LOCATION_TRACKING = {
///   api_version = 1,
///   location_setting_key = "LivePosition_{team}_{player}",
///   location_markers = function(value) return { ... } end,
/// }.
/// The host deliberately owns DataStorage subscription and marker lifecycle; the
/// pack only interprets the game-specific value and returns map placements.
class LocationTracking final {
public:
    static constexpr size_t MAX_MARKERS = 256;
    static constexpr size_t MAX_ID_LENGTH = 128;
    static constexpr size_t MAX_MAP_LENGTH = 128;
    static constexpr size_t MAX_LABEL_LENGTH = 512;

    LocationTracking(lua_State* L, APTracker* ap, Tracker* tracker);
    ~LocationTracking();

    /// Detect and validate LOCATION_TRACKING after the pack init script has run.
    /// Returns false when the pack does not opt into a supported contract.
    bool discover();

    /// Apply one decoded DataStorage value. Public primarily so this bounded Lua
    /// bridge can be tested without an Archipelago server.
    bool update(const nlohmann::json& value);

    bool active() const { return _active; }

    static std::string resolveSettingKey(const std::string& keyTemplate, int team, int player);

private:
    struct Marker {
        std::string map;
        float x;
        float y;
        std::string label;
    };

    lua_State* _L;
    APTracker* _ap;
    Tracker* _tracker;
    int _callback = LUA_NOREF;
    bool _active = false;
    std::string _keyTemplate;
    std::string _activeKey;
    int _team = -1;
    int _player = -1;
    std::unordered_map<std::string, Marker> _markers;

    void subscribe();
    void clearMarkers();
    void renderMarkers();
    void handleValue(const std::string& key, const nlohmann::json& value);
    std::string markerId(const std::string& id) const;
};

#endif // _AP_LOCATIONTRACKING_H
