#pragma once
#include <cstdint>
#include <string>

// What the agent is about, drawn over the game world itself (the player, 2026-11-10: «poder activar y desactivar
// las overlays del agente en el juego, no tan solo en el mapa»): its intent and reason over the hero, the way it
// goes, its target, the enemies' aggro reach (Manolito, block N: 11.2 tiles), the doors and ways in it heads for.
// Each layer on or off in the ModKit menu («Overlays del agente», with the radar and the map's marks), saved to
// [AgentOverlay]. The agent fills a frame on the render thread; the HUD draws the last one.
//
// World to screen: the game's isometric view (Manolito, block O: X = 0.894427 (x - y), Y = 0.447214 (x + y) in
// world units), anchored on the hero's on-screen centre, scaled by the pixels per unit measured on any creature on
// screen (the zoom).
namespace AgentOverlay
{
    void install();

    // The layers, for the agent to skip work for those off.
    enum class Layer
    {
        Intent,
        Route,
        Target,
        Aggro,
        Doors,
    };
    bool shown(Layer layer);

    // A frame: the hero's world position and its on-screen centre.
    void begin(int32_t heroX, int32_t heroY, float heroScreenX, float heroScreenY);
    // Another object seen on screen: its world position and on-screen centre (the scale from it).
    void calibrate(int32_t x, int32_t y, float screenX, float screenY);
    // Pixels per isometric world unit, as last measured (0: not yet), for AgentCursor's world-to-screen.
    float scale();
    // A circle on the ground (radius in tiles).
    void ring(int32_t x, int32_t y, float tiles, uint32_t color, float thickness = 1.5f);
    // A line on the ground from the hero to (x, y), with a mark at its end and an optional label.
    void way(int32_t x, int32_t y, uint32_t color, const std::string& label = {});
    // A ring around an object on screen (its pick centre).
    void around(float screenX, float screenY, float radius, uint32_t color);
    // The text over the hero (intent and reason).
    void heroText(const std::string& text, uint32_t color);
    void end(uint32_t now);
}
