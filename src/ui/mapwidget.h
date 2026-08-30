#pragma once

#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>
#include "../core/location.h"
#include "../core/locationsection.h"
#include "../uilib/image.h"

class Pack;

namespace Ui {

class MarkerIconResource;

class MapWidget final : public Image {
public:
    typedef ::Location::MapLocation::Shape Shape;

    MapWidget(int x, int y, int w, int h, const char* filename);
    MapWidget(int x, int y, int w, int h, const void* data, size_t len);
    MapWidget(int x, int y, int w, int h, std::unique_ptr<ImageFuture>&& future);

    struct Point {
        int x = 0;
        int y = 0;
        int size = 0;
        int borderThickness = 0;
        Shape shape = Shape::UNSPECIFIED;
        int state = 1;
        Highlight highlight = Highlight::NONE;
    };

    struct Location {
        std::vector<Point> pos;
    };

    struct MarkerAppearance {
        enum class Type {
            DIAMOND,
            ICON,
        };

        Type type = Type::DIAMOND;
        Color color = {0xff, 0xff, 0xff, 0xff};
        std::string iconPath;
        int iconSize = 16;
    };

    struct Marker {
        float x = 0.0f;
        float y = 0.0f;
        MarkerAppearance appearance;
        std::string label;
        std::shared_ptr<MarkerIconResource> icon;
        size_t order = 0;
        bool iconRendered = false;
    };

    struct MarkerHover {
        std::string id;
        std::string label;
    };

    // TODO: enum location state
    void addLocation(const std::string& name, Point&& point);
    void setLocationState(const std::string& name, int state, size_t n);
    void setLocationHighlight(const std::string& name, Highlight highlight, size_t n);

    void setMarker(const std::string& id, float x, float y);
    void setMarker(const std::string& id, float x, float y, MarkerAppearance appearance, std::string label = {});
    void clearMarker(const std::string& id);
    void clearMarkers();
    void setMarkerPack(const Pack* pack) { _markerPack = pack; }
    bool consumeMarkerHoverInvalidation();

    // FIXME: this does not work if name is not unique
    Signal<const std::string&,int,int> onLocationHover; // FIXME: we should provide absolute AND relative mouse position through the Event stack
    Signal<const std::vector<MarkerHover>&,int,int> onMarkerHover;

    void render(Renderer renderer, int offX, int offY) override;
    int getAbsLeft() const { return _absX; } // FIXME: this is not really a good solution
    int getAbsTop() const { return _absY; }

    void setHideClearedLocations(bool hide) { _hideClearedLocations = hide; }
    void setHideUnreachableLocations(bool hide) { _hideUnreachableLocations = hide; }

    float getZoom() const { return _zoom; }
    void setZoom(float zoom);
    std::tuple<float, float> getPan() const { return {_panX, _panY}; }
    void setPan(float x, float y);
    std::tuple<float, float> getPanCenter() const;
    void setPanCenter(float x, float y);

    static Color StateColors[17];
    static std::map<Highlight, Color> HighlightColors;
    static bool SplitRects;

protected:
    int _absX=0;
    int _absY=0;
    std::map<std::string, Location> _locations;
    std::map<std::string, Marker> _markers;
    std::map<std::string, std::weak_ptr<MarkerIconResource>> _markerIcons;
    std::optional<std::string> _locationHover; // TODO: store iterator instead of string?
    std::vector<std::string> _markerHover;
    bool _markerHoverInvalidated = false;
    size_t _nextMarkerOrder = 0;

    bool _hideClearedLocations = false;
    bool _hideUnreachableLocations = false;

    /// Zoom state
    float _zoom = 1.0f;           ///< Zoom level (>= 1.0, no maximum)
    float _panX = 0.0f;           ///< Pan offset X in image coordinates
    float _panY = 0.0f;           ///< Pan offset Y in image coordinates

    /// Drag state
    bool _dragging = false;       ///< Is currently dragging
    int _dragStartX = 0;          ///< Mouse position at drag start
    int _dragStartY = 0;          ///< Mouse position at drag start
    float _dragStartPanX = 0.0f;  ///< Pan at drag start
    float _dragStartPanY = 0.0f;  ///< Pan at drag start

    /// Last known mouse position (for scroll zooming)
    int _lastMouseX = 0;          ///< Mouse X for scroll zooming
    int _lastMouseY = 0;          ///< Mouse Y for scroll zooming

private:
    const Pack* _markerPack = nullptr;
    void connectSignals();
    /// Calculate srcRect and dstRect from size, autoSize, zoom and pan.
    /// Calling while .width or .height of size or autoSize is <1 is undefined.
    void calculateSrcAndDst(int offX, int offY, bool clip, float& baseScale, SDL_Rect& srcRect,
        SDL_FRect& dstRect) const;
    static void calculateImagePointScreenPosition(float x, float y, const SDL_Rect& srcRect,
        const SDL_FRect& dstRect, float& screenX, float& screenY);
    static void calculateMarkerScreenRect(const Marker& marker, const SDL_Rect& srcRect, const SDL_FRect& dstRect,
        SDL_FRect& rect);
    static bool isMarkerHit(const Marker& marker, int x, int y, const SDL_Rect& srcRect, const SDL_FRect& dstRect);
    static void calculateLocationScreenRect(const Point& pos, const SDL_Rect& srcRect, const SDL_FRect& dstRect,
        float baseScale, int& innerX, int& innerY, int& innerW, int& innerH, int& borderSize);
    bool updateMarkerHover(int x, int y, int absX, int absY, const SDL_Rect& srcRect, const SDL_FRect& dstRect);
    void clearMarkerHover();
};

} // namespace Ui
