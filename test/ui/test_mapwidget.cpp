#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

#include "../../lib/luaglue/lua_include.h"
#include "../../src/ui/mapwidget.h"
#include "../../src/ui/trackerview.h"
#include "../uilib/font_helper.h"

namespace {

constexpr char IMAGE_PATH[] = "examples/zoom-pan-test/images/map.png";
constexpr char PACK_PATH[] = "examples/zoom-pan-test";
constexpr char ICON_PACK_PATH[] = "examples/template_pack";
constexpr int SURFACE_WIDTH = 240;
constexpr int SURFACE_HEIGHT = 120;

class TestTrackerView : public Ui::TrackerView {
public:
    explicit TestTrackerView(Tracker* tracker)
        : TrackerView(0, 0, SURFACE_WIDTH, SURFACE_HEIGHT, tracker, "tracker_default", &fontStore)
    {
    }

    Ui::MapWidget* map(const std::string& name, const size_t index = 0)
    {
        auto it = _maps.find(name);
        if (it == _maps.end() || index >= it->second.size())
            return nullptr;
        auto widget = it->second.begin();
        std::advance(widget, static_cast<long>(index));
        return *widget;
    }

    bool addDuplicateMapNode()
    {
        return addLayoutNode(this, LayoutNode::FromJSON({
            {"type", "map"},
            {"maps", {"map", "map"}},
        }));
    }
};

class SoftwareRenderer {
public:
    SoftwareRenderer()
    {
        surface = SDL_CreateRGBSurfaceWithFormat(0, SURFACE_WIDTH, SURFACE_HEIGHT, 32, SDL_PIXELFORMAT_RGBA32);
        if (!surface)
            throw std::runtime_error("failed to create surface");
        renderer = SDL_CreateSoftwareRenderer(surface);
        if (!renderer) {
            SDL_FreeSurface(surface);
            throw std::runtime_error("failed to create renderer");
        }
    }

    ~SoftwareRenderer()
    {
        SDL_DestroyRenderer(renderer);
        SDL_FreeSurface(surface);
    }

    std::string render(Ui::MapWidget& widget)
    {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        widget.render(renderer, 0, 0);
        SDL_RenderPresent(renderer);
        return std::string(static_cast<const char*>(surface->pixels), surface->h * surface->pitch);
    }

    SDL_Surface* surface = nullptr;
    SDL_Renderer* renderer = nullptr;
};

struct Bounds {
    int left = SURFACE_WIDTH;
    int top = SURFACE_HEIGHT;
    int right = -1;
    int bottom = -1;

    bool empty() const { return right < left || bottom < top; }
    int width() const { return empty() ? 0 : right - left + 1; }
    int height() const { return empty() ? 0 : bottom - top + 1; }
};

Bounds differenceBounds(const std::string& before, const std::string& after, const int pitch)
{
    Bounds bounds;
    for (int y = 0; y < SURFACE_HEIGHT; ++y) {
        for (int x = 0; x < SURFACE_WIDTH; ++x) {
            const size_t offset = static_cast<size_t>(y * pitch + x * 4);
            if (before.compare(offset, 4, after, offset, 4) != 0) {
                bounds.left = std::min(bounds.left, x);
                bounds.top = std::min(bounds.top, y);
                bounds.right = std::max(bounds.right, x);
                bounds.bottom = std::max(bounds.bottom, y);
            }
        }
    }
    return bounds;
}

std::string renderUntilDifferent(SoftwareRenderer& output, Ui::MapWidget& widget, const std::string& reference)
{
    for (int i = 0; i < 1000; ++i) {
        const std::string pixels = output.render(widget);
        if (pixels != reference)
            return pixels;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ADD_FAILURE() << "icon did not finish loading within 1 second";
    return reference;
}

TEST(MapWidgetMarker, SetUpdateAndClearState)
{
    SoftwareRenderer output;
    Ui::MapWidget widget(0, 0, 207, 100, IMAGE_PATH);
    const std::string baseline = output.render(widget);

    widget.setMarker("player", 100.25f, 50.5f);
    EXPECT_FALSE(output.render(widget) == baseline);
    widget.setMarker("player", 200.5f, 50.75f);
    const std::string moved = output.render(widget);
    Ui::MapWidget onlyMovedMarker(0, 0, 207, 100, IMAGE_PATH);
    onlyMovedMarker.setMarker("player", 200.5f, 50.75f);
    EXPECT_TRUE(moved == output.render(onlyMovedMarker));

    widget.setMarker("spectator", 300.0f, 100.0f);
    widget.clearMarker("player");
    Ui::MapWidget onlySpectator(0, 0, 207, 100, IMAGE_PATH);
    onlySpectator.setMarker("spectator", 300.0f, 100.0f);
    EXPECT_TRUE(output.render(widget) == output.render(onlySpectator));

    widget.clearMarkers();
    EXPECT_TRUE(output.render(widget) == baseline);
}

TEST(MapWidgetMarker, RenderFollowsZoomAndPanWithFixedSize)
{
    SoftwareRenderer output;
    Ui::MapWidget widget(10, 10, 207, 100, IMAGE_PATH);
    const std::string baseline = output.render(widget);

    widget.setMarker("player", 414.0f, 200.0f);
    const Bounds normal = differenceBounds(baseline, output.render(widget), output.surface->pitch);
    ASSERT_FALSE(normal.empty());

    widget.setZoom(2.0f);
    const std::string zoomBaseline = [&] {
        widget.clearMarkers();
        const std::string pixels = output.render(widget);
        widget.setMarker("player", 414.0f, 200.0f);
        return pixels;
    }();
    const Bounds zoomed = differenceBounds(zoomBaseline, output.render(widget), output.surface->pitch);
    EXPECT_LE(std::abs(zoomed.width() - normal.width()), 1);
    EXPECT_LE(std::abs(zoomed.height() - normal.height()), 1);
    EXPECT_LE(std::abs((zoomed.left + zoomed.right) - (normal.left + normal.right)), 1);
    EXPECT_LE(std::abs((zoomed.top + zoomed.bottom) - (normal.top + normal.bottom)), 1);

    widget.setPanCenter(300.0f, 200.0f);
    const std::string panBaseline = [&] {
        widget.clearMarkers();
        const std::string pixels = output.render(widget);
        widget.setMarker("player", 414.0f, 200.0f);
        return pixels;
    }();
    const Bounds panned = differenceBounds(panBaseline, output.render(widget), output.surface->pitch);
    EXPECT_GT(panned.left, zoomed.left);
    EXPECT_LE(std::abs(panned.width() - normal.width()), 1);
    EXPECT_LE(std::abs(panned.height() - normal.height()), 1);
}

TEST(MapWidgetMarker, WidgetClipContainsOffViewportMarker)
{
    SoftwareRenderer output;
    Ui::MapWidget widget(20, 20, 100, 80, IMAGE_PATH);
    const std::string baseline = output.render(widget);

    widget.setMarker("player", -10000.0f, -10000.0f);
    EXPECT_TRUE(output.render(widget) == baseline);
}

TEST(MapWidgetMarker, PackIconsRenderWithFixedAspectFallbackAndReuse)
{
    SoftwareRenderer output;
    Pack iconPack(ICON_PACK_PATH);
    iconPack.setVariant("standard");
    Ui::MapWidget widget(0, 0, 207, 100, IMAGE_PATH);
    widget.setMarkerPack(&iconPack);
    const std::string baseline = output.render(widget);

    widget.setMarker("marker", 414.0f, 200.0f);
    const std::string diamond = output.render(widget);
    EXPECT_NE(diamond, baseline);

    Ui::MapWidget::MarkerAppearance icon;
    icon.type = Ui::MapWidget::MarkerAppearance::Type::ICON;
    icon.iconPath = "images/items/toggle.png"; // 64 by 32 in the pack
    icon.iconSize = 16;
    widget.setMarker("marker", 414.0f, 200.0f, icon);
    const std::string iconPixels = renderUntilDifferent(output, widget, diamond);
    EXPECT_NE(iconPixels, diamond);
    const Bounds iconBounds = differenceBounds(baseline, iconPixels, output.surface->pitch);
    EXPECT_FALSE(iconBounds.empty());
    EXPECT_LE(iconBounds.width(), 16);
    EXPECT_LE(iconBounds.height(), 8);
    EXPECT_LE(std::abs((iconBounds.left + iconBounds.right) - 207), 3);
    EXPECT_LE(std::abs((iconBounds.top + iconBounds.bottom) - 100), 3);

    widget.setZoom(2.0f);
    Ui::MapWidget zoomReference(0, 0, 207, 100, IMAGE_PATH);
    zoomReference.setZoom(2.0f);
    const Bounds zoomedBounds = differenceBounds(output.render(zoomReference), output.render(widget), output.surface->pitch);
    EXPECT_FALSE(zoomedBounds.empty());
    EXPECT_LE(std::abs(zoomedBounds.width() - iconBounds.width()), 1);
    EXPECT_LE(std::abs(zoomedBounds.height() - iconBounds.height()), 1);

    widget.setPanCenter(300.0f, 200.0f);
    Ui::MapWidget panReference(0, 0, 207, 100, IMAGE_PATH);
    panReference.setZoom(2.0f);
    panReference.setPanCenter(300.0f, 200.0f);
    const std::string pannedBaseline = output.render(panReference);
    const Bounds pannedBounds = differenceBounds(pannedBaseline, output.render(widget), output.surface->pitch);
    EXPECT_FALSE(pannedBounds.empty());
    EXPECT_LE(std::abs(pannedBounds.width() - iconBounds.width()), 1);
    EXPECT_LE(std::abs(pannedBounds.height() - iconBounds.height()), 1);

    // A coordinate-only replacement of the same icon must keep the ready resource instead of showing its diamond.
    Ui::MapWidget movedReference(0, 0, 207, 100, IMAGE_PATH);
    movedReference.setZoom(2.0f);
    movedReference.setPanCenter(300.0f, 200.0f);
    movedReference.setMarker("marker", 200.0f, 200.0f);
    const std::string movedDiamond = output.render(movedReference);
    widget.setMarker("marker", 200.0f, 200.0f, icon);
    EXPECT_NE(output.render(widget), movedDiamond);

    // Missing assets retain the normal diamond fallback even after their asynchronous decode has completed.
    Ui::MapWidget::MarkerAppearance missing = icon;
    missing.iconPath = "images/items/missing.png";
    widget.setMarker("marker", 200.0f, 200.0f, missing);
    EXPECT_EQ(output.render(widget), movedDiamond);
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        EXPECT_EQ(output.render(widget), movedDiamond);
    }

    // Replacement, removal, and reset-style clearing release their visible resource and restore the map.
    widget.setMarker("marker", 200.0f, 200.0f, icon);
    EXPECT_NE(renderUntilDifferent(output, widget, movedDiamond), movedDiamond);
    widget.clearMarker("marker");
    EXPECT_EQ(output.render(widget), pannedBaseline);
    widget.setMarker("marker", 200.0f, 200.0f, icon);
    widget.clearMarkers();
    EXPECT_EQ(output.render(widget), pannedBaseline);
}

TEST(MapWidgetMarker, PackIconClippingContainsOffViewportIcon)
{
    SoftwareRenderer output;
    Pack pack(PACK_PATH);
    pack.setVariant("standard");
    Ui::MapWidget widget(20, 20, 100, 80, IMAGE_PATH);
    widget.setMarkerPack(&pack);
    const std::string baseline = output.render(widget);

    Ui::MapWidget::MarkerAppearance icon;
    icon.type = Ui::MapWidget::MarkerAppearance::Type::ICON;
    icon.iconPath = "images/a.png";
    widget.setMarker("marker", 414.0f, 200.0f);
    const std::string diamond = output.render(widget);
    widget.setMarker("marker", 414.0f, 200.0f, icon);
    EXPECT_NE(renderUntilDifferent(output, widget, diamond), diamond);
    widget.setMarker("marker", -10000.0f, -10000.0f, icon);
    EXPECT_EQ(output.render(widget), baseline);
}

TEST(TrackerViewMapMarkerHint, IconAppearanceLoadsPackRelativeAssetAndReplacesDiamond)
{
    (void)getDefaultFont();
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);

    {
        SoftwareRenderer output;
        Pack pack(PACK_PATH);
        pack.setVariant("standard");
        Tracker tracker(&pack, L);
        ASSERT_TRUE(tracker.AddMaps("maps/maps.json"));
        ASSERT_TRUE(tracker.AddLocations("locations/locations.json"));
        ASSERT_TRUE(tracker.AddItems("items/items.json"));
        ASSERT_TRUE(tracker.AddLayouts("layouts/standard.json"));

        TestTrackerView view(&tracker);
        view.relayout();
        Ui::MapWidget* map = view.map("map");
        ASSERT_NE(map, nullptr);
        map->setPosition({0, 0});
        map->setSize({207, 100});
        map->setImage(IMAGE_PATH);

        const std::string baseline = output.render(*map);
        tracker.UiHint("MapMarker map", R"({"id":"player","x":414,"y":200})");
        const std::string diamond = output.render(*map);
        EXPECT_NE(diamond, baseline);

        tracker.UiHint("MapMarker map", R"({"id":"player","x":414,"y":200,"appearance":{"type":"icon","path":"images/a.png","size":16}})");
        const std::string icon = renderUntilDifferent(output, *map, diamond);
        EXPECT_NE(icon, diamond);

        SDL_Surface* iconSurface = pack.getImage("images/a.png");
        ASSERT_NE(iconSurface, nullptr);
        Uint8 red, green, blue, alpha;
        SDL_GetRGBA(*static_cast<const Uint32*>(iconSurface->pixels), iconSurface->format,
            &red, &green, &blue, &alpha);
        EXPECT_EQ(alpha, 0);
        const size_t transparentCorner = static_cast<size_t>(42 * output.surface->pitch + 95 * 4);
        EXPECT_EQ(icon.compare(transparentCorner, 4, baseline, transparentCorner, 4), 0);

        // Omitted size defaults to 16, and this full replacement retains the already-ready path resource.
        tracker.UiHint("MapMarker map", R"({"id":"player","x":414,"y":200,"appearance":{"type":"icon","path":"images/a.png"}})");
        EXPECT_EQ(output.render(*map), icon);

        tracker.UiHint("MapMarker map", R"({"id":"player","remove":true})");
        EXPECT_EQ(output.render(*map), baseline);
    }

    lua_close(L);
}

TEST(TrackerViewMapMarkerHint, ParsesJsonRoutesRemovesAndResetsWithoutSaving)
{
    (void)getDefaultFont();
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);

    {
        Pack pack(PACK_PATH);
        pack.setVariant("standard");
        Tracker tracker(&pack, L);
        ASSERT_TRUE(tracker.AddMaps("maps/maps.json"));
        ASSERT_TRUE(tracker.AddLocations("locations/locations.json"));
        ASSERT_TRUE(tracker.AddItems("items/items.json"));
        ASSERT_TRUE(tracker.AddLayouts("layouts/standard.json"));

        TestTrackerView view(&tracker);
        view.relayout();
        Ui::MapWidget* map = view.map("map");
        ASSERT_NE(map, nullptr);
        map->setPosition({10, 10});
        map->setSize({207, 100});
        map->setImage(IMAGE_PATH);

        SoftwareRenderer output;
        const std::string baseline = output.render(*map);

        const std::string defaultMarker = R"({"id":"player","x":414.5,"y":200.25})";
        const std::string redMarker = R"({"id":"player","x":414.5,"y":200.25,"appearance":{"type":"diamond","color":"#ff0000"}})";
        const std::string alphaMarker = R"({"id":"player","x":414.5,"y":200.25,"appearance":{"type":"diamond","color":"#8000ff00"}})";

        tracker.UiHint("MapMarker map", defaultMarker);
        const std::string defaultMarked = output.render(*map);
        EXPECT_FALSE(defaultMarked == baseline);

        tracker.UiHint("MapMarker map", redMarker);
        const std::string marked = output.render(*map);
        EXPECT_FALSE(marked == defaultMarked);

        tracker.UiHint("MapMarker map", alphaMarker);
        const std::string alphaMarked = output.render(*map);
        EXPECT_FALSE(alphaMarked == marked);

        // Set operations completely replace the appearance rather than retaining it.
        tracker.UiHint("MapMarker map", defaultMarker);
        EXPECT_TRUE(output.render(*map) == defaultMarked);
        tracker.UiHint("MapMarker map", redMarker);
        EXPECT_TRUE(output.render(*map) == marked);

        tracker.UiHint("MapMarker map", R"({"id":"player","x":414.5,"y":200.25,"extra":true,"appearance":{"type":"diamond","color":"#ff0000","note":"ignored"}})");
        EXPECT_TRUE(output.render(*map) == marked);

        const auto savedHints = view.getHints();
        EXPECT_TRUE(std::none_of(savedHints.begin(), savedHints.end(), [](const auto& hint) {
            return hint.first.rfind("MapMarker ", 0) == 0;
        }));

        const std::vector<std::string> invalidValues = {
            "", "not JSON", "[]", "null", "{}",
            R"({"id":"","x":1,"y":2})", R"({"id":1,"x":1,"y":2})",
            R"({"id":"player","x":1})", R"({"id":"player","y":2})",
            R"({"id":"player","x":"1","y":2})", R"({"id":"player","x":true,"y":2})",
            R"({"id":"player","x":null,"y":2})", R"({"id":"player","x":[],"y":2})",
            R"({"id":"player","x":1,"y":"2"})", R"({"id":"player","x":1,"y":false})",
            R"({"id":"player","x":NaN,"y":2})", R"({"id":"player","x":Infinity,"y":2})",
            R"({"id":"player","x":-Infinity,"y":2})", R"({"id":"player","x":3.5e38,"y":2})",
            R"({"id":"player","x":1,"y":3.5e38})", R"({"id":"player","x":1e400,"y":2})",
            R"({"id":"player","remove":false})", R"({"id":"player","remove":"true"})",
            R"({"id":"player","remove":true,"x":1,"y":2})",
            R"({"id":"player","x":1,"y":2,"appearance":null})",
            R"({"id":"player","x":1,"y":2,"appearance":{}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":1}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"circle"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"diamond","color":1}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"diamond","color":"ff0000"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"diamond","color":"#f00"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"diamond","color":"#ff000"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"diamond","color":"#ff000g"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":""}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":1}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"/images/a.png"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"../images/a.png"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"images/../a.png"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"C:\\\\images\\a.png"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"images/a.png","size":0}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"images/a.png","size":-1}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"images/a.png","size":1.5}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"images/a.png","size":"16"}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"images/a.png","size":4097}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"images/a.png","size":2147483648}})",
            R"({"id":"player","x":1,"y":2,"appearance":{"type":"icon","path":"images/a.png","color":"#ff0000"}})",
        };
        for (const auto& value : invalidValues) {
            EXPECT_NO_THROW(tracker.UiHint("MapMarker map", value));
            EXPECT_TRUE(output.render(*map) == marked) << value;
        }

        tracker.UiHint("MapMarker other", R"({"id":"player","remove":true})");
        EXPECT_TRUE(output.render(*map) == marked);
        tracker.UiHint("MapMarker map[0]", R"({"id":"player","remove":true})");
        EXPECT_TRUE(output.render(*map) == baseline);

        // Coordinates remain valid when fractional or outside the map image.
        tracker.UiHint("MapMarker map[0]", R"({"id":"player","x":-4.5,"y":3.25})");
        EXPECT_FALSE(output.render(*map) == baseline);
        tracker.UiHint("reset", "reset");
        EXPECT_TRUE(output.render(*map) == baseline);
    }

    lua_close(L);
}

TEST(TrackerViewMapMarkerHint, UnnumberedTargetsAllAndNumberedTargetsOneInstance)
{
    (void)getDefaultFont();
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);

    {
        Pack pack(PACK_PATH);
        pack.setVariant("standard");
        Tracker tracker(&pack, L);
        ASSERT_TRUE(tracker.AddMaps("maps/maps.json"));

        TestTrackerView view(&tracker);
        ASSERT_TRUE(view.addDuplicateMapNode());
        Ui::MapWidget* first = view.map("map", 0);
        Ui::MapWidget* second = view.map("map", 1);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        for (Ui::MapWidget* map : {first, second}) {
            map->setPosition({0, 0});
            map->setSize({207, 100});
            map->setImage(IMAGE_PATH);
        }

        SoftwareRenderer output;
        const std::string firstBaseline = output.render(*first);
        const std::string secondBaseline = output.render(*second);

        tracker.UiHint("MapMarker map", R"({"id":"player","x":414,"y":200})");
        EXPECT_FALSE(output.render(*first) == firstBaseline);
        EXPECT_FALSE(output.render(*second) == secondBaseline);

        tracker.UiHint("MapMarker map", R"({"id":"player","remove":true})");
        tracker.UiHint("MapMarker map[0]", R"({"id":"player","x":414,"y":200})");
        EXPECT_FALSE(output.render(*first) == firstBaseline);
        EXPECT_TRUE(output.render(*second) == secondBaseline);
    }

    lua_close(L);
}

} // namespace
