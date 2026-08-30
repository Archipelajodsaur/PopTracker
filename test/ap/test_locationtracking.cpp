#include <gtest/gtest.h>
#include <lauxlib.h>
#include <lua.h>
#include <cassert>
#include <memory>
#include <vector>
#include "../../src/ap/locationtracking.h"
#include "../../src/ap/aptracker.h"
#include "../../src/core/pack.h"
#include "../../src/core/tracker.h"

namespace {

class LocationTrackingTest : public testing::Test {
protected:
    LocationTrackingTest()
        : pack("examples/rules_test"), L(luaL_newstate()), tracker(&pack, L), ap("PopTracker")
    {
        assert(L);
        luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
        lua_pop(L, 1);
        host = std::make_unique<LocationTracking>(L, &ap, &tracker);
        tracker.onUiHint += {this, [this](void*, const std::string& name, const std::string& value) {
            hints.emplace_back(name, value);
        }};
    }

    ~LocationTrackingTest() override
    {
        host.reset();
        lua_close(L);
    }

    void loadContract(const char* body)
    {
        ASSERT_EQ(luaL_dostring(L, body), LUA_OK) << lua_tostring(L, -1);
        ASSERT_TRUE(host->discover());
    }

    Pack pack;
    lua_State* L;
    Tracker tracker;
    APTracker ap;
    std::unique_ptr<LocationTracking> host;
    std::vector<std::pair<std::string, std::string>> hints;
};

} // namespace

TEST(LocationTracking, ResolvesRequiredPlaceholders)
{
    EXPECT_EQ(LocationTracking::resolveSettingKey("LivePosition_{team}_{player}", 0, 7), "LivePosition_0_7");
    EXPECT_EQ(LocationTracking::resolveSettingKey("{team}:{team}/{player}", 3, 11), "3:3/11");
    EXPECT_TRUE(LocationTracking::resolveSettingKey("LivePosition_{player}", 0, 7).empty());
}

TEST_F(LocationTrackingTest, OptInIsStrictAndAbsentContractIsInert)
{
    EXPECT_FALSE(host->discover());
    ASSERT_EQ(luaL_dostring(L, R"(LOCATION_TRACKING = { api_version = 2, location_setting_key = "x_{team}_{player}", location_markers = function() return {} end })"), LUA_OK);
    EXPECT_FALSE(host->discover());
    EXPECT_TRUE(hints.empty());
}

TEST_F(LocationTrackingTest, RendersMultipleMarkersAndRemovesMissingAndMovedMarkers)
{
    loadContract(R"(
        LOCATION_TRACKING = {
            api_version = 1,
            location_setting_key = "LivePosition_{team}_{player}",
            location_markers = function(value)
                if value.step == 1 then
                    return {
                        { id = "desk", map = "Crater", x = 400.5, y = 200.25, label = "Desktop", debug = { ignored = true } },
                        { id = "laptop", map = "Caves", x = 12, y = 20 }
                    }
                end
                return { { id = "desk", map = "Caves", x = 18, y = 22 } }
            end,
        }
    )");

    ASSERT_TRUE(host->update({{"step", 1}}));
    ASSERT_EQ(hints.size(), 2U);
    EXPECT_TRUE((hints[0].first == "MapMarker Caves" && hints[1].first == "MapMarker Crater") ||
            (hints[0].first == "MapMarker Crater" && hints[1].first == "MapMarker Caves"));
    const auto& deskHint = hints[0].first == "MapMarker Crater" ? hints[0].second : hints[1].second;
    EXPECT_NE(deskHint.find("location-tracking:-1:-1:desk"), std::string::npos);
    EXPECT_NE(deskHint.find("Desktop"), std::string::npos);

    ASSERT_TRUE(host->update({{"step", 2}}));
    ASSERT_EQ(hints.size(), 5U);
    EXPECT_TRUE((hints[2].first == "MapMarker Caves" && hints[3].first == "MapMarker Crater") ||
            (hints[2].first == "MapMarker Crater" && hints[3].first == "MapMarker Caves"));
    EXPECT_NE(hints[2].second.find("\"remove\":true"), std::string::npos);
    EXPECT_NE(hints[3].second.find("\"remove\":true"), std::string::npos);
    EXPECT_EQ(hints[4].first, "MapMarker Caves");
}

TEST_F(LocationTrackingTest, NilAndInvisibleResultsClearExistingMarkers)
{
    loadContract(R"(
        LOCATION_TRACKING = {
            api_version = 1,
            location_setting_key = "LivePosition_{team}_{player}",
            location_markers = function(value)
                if value.hidden then return { { id = "player", map = "Crater", x = 1, y = 2, visible = false } } end
                if value.none then return nil end
                return { { id = "player", map = "Crater", x = 1, y = 2 } }
            end,
        }
    )");

    ASSERT_TRUE(host->update({}));
    ASSERT_TRUE(host->update({{"hidden", true}}));
    ASSERT_TRUE(host->update({{"none", true}}));
    ASSERT_EQ(hints.size(), 2U);
    EXPECT_NE(hints[1].second.find("\"remove\":true"), std::string::npos);
}

TEST_F(LocationTrackingTest, RejectsMalformedAndDuplicateMarkersWithoutChangingCurrentMarkers)
{
    loadContract(R"(
        LOCATION_TRACKING = {
            api_version = 1,
            location_setting_key = "LivePosition_{team}_{player}",
            location_markers = function(value)
                if value.bad then return { { id = "x", map = "Map", x = 1, y = 2 }, { id = "x", map = "Map", x = 3, y = 4 } } end
                if value.mixed then return { { id = "x", map = "Map", x = 1, y = 2 }, extra = true } end
                if value.sparse then return { [1] = { id = "x", map = "Map", x = 1, y = 2 }, [3] = { id = "y", map = "Map", x = 3, y = 4 } } end
                if value.numeric_string then return { { id = "x", map = "Map", x = "1", y = 2 } } end
                if value.over_limit then
                    local markers = {}
                    for i = 1, 257 do markers[i] = { id = tostring(i), map = "Map", x = i, y = 2 } end
                    return markers
                end
                if value.oversized_id then
                    local id = ""
                    for i = 1, 129 do id = id .. "x" end
                    return { { id = id, map = "Map", x = 1, y = 2 } }
                end
                return { { id = "x", map = "Map", x = 1, y = 2 } }
            end,
        }
    )");

    ASSERT_TRUE(host->update({}));
    const auto initialHints = hints.size();
    EXPECT_FALSE(host->update({{"bad", true}}));
    EXPECT_FALSE(host->update({{"mixed", true}}));
    EXPECT_FALSE(host->update({{"sparse", true}}));
    EXPECT_FALSE(host->update({{"over_limit", true}}));
    EXPECT_EQ(hints.size(), initialHints);
    ASSERT_TRUE(host->update({{"numeric_string", true}}));
    ASSERT_EQ(hints.size(), initialHints + 1);
    EXPECT_NE(hints.back().second.find("\"remove\":true"), std::string::npos);

    ASSERT_TRUE(host->update({}));
    const auto restoredHints = hints.size();
    ASSERT_TRUE(host->update({{"oversized_id", true}}));
    ASSERT_EQ(hints.size(), restoredHints + 1);
    EXPECT_NE(hints.back().second.find("\"remove\":true"), std::string::npos);
}

TEST_F(LocationTrackingTest, DropsMalformedRowsButReconcilesTheirValidSiblings)
{
    loadContract(R"(
        LOCATION_TRACKING = {
            api_version = 1,
            location_setting_key = "LivePosition_{team}_{player}",
            location_markers = function(value)
                return {
                    { id = "valid", map = "Map", x = 1, y = 2 },
                    { id = "invalid", map = "Map", x = "not-a-number", y = 3 },
                }
            end,
        }
    )");

    ASSERT_TRUE(host->update({}));
    ASSERT_EQ(hints.size(), 1U);
    EXPECT_EQ(hints[0].first, "MapMarker Map");
    EXPECT_NE(hints[0].second.find("valid"), std::string::npos);
    EXPECT_EQ(hints[0].second.find("invalid"), std::string::npos);
}

TEST_F(LocationTrackingTest, ResetRendersCachedMarkersAndDisconnectClearsThem)
{
    loadContract(R"(
        LOCATION_TRACKING = {
            api_version = 1,
            location_setting_key = "LivePosition_{team}_{player}",
            location_markers = function(value) return { { id = "player", map = "Map", x = 1, y = 2 } } end,
        }
    )");

    ASSERT_TRUE(host->update({}));
    bool resetHintCompleted = false;
    tracker.onUiHint += {&resetHintCompleted, [&resetHintCompleted](void*, const std::string& name, const std::string&) {
        if (name == "reset")
            resetHintCompleted = true;
    }};
    tracker.onPostUiHintReset += {&resetHintCompleted, [&resetHintCompleted](void*) {
        EXPECT_TRUE(resetHintCompleted);
    }};
    tracker.UiHint("reset", "reset");
    ASSERT_EQ(hints.size(), 3U);
    EXPECT_EQ(hints[1].first, "reset");
    EXPECT_EQ(hints[2].first, "MapMarker Map");

    ap.onStateChanged.emit(&ap, APClient::State::DISCONNECTED);
    ASSERT_EQ(hints.size(), 4U);
    EXPECT_EQ(hints[3].first, "MapMarker Map");
    EXPECT_NE(hints[3].second.find("\"remove\":true"), std::string::npos);
}
