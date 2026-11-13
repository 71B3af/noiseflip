// fake_skate.cpp
//
// A stand-in for the game, for testing noiseflip.exe without skate. running.
// It builds the same object chain the real game has (same offsets, vtable values = base + RVA),
// including the skater's position and the board's velocity and spin, then plays a scripted session:
//
//   menu, roll, ollie (flat landing), a curb drop (quiet landing), a big air with a kickflip
//   (loud landing), a shuvit, a revert (plays the shuvit note), carving (must NOT trigger anything),
//   a grind, a bail whose ragdoll flickers (one bail note), and a ground/air flicker burst.
//
//   fake_skate.exe                      (leave it running)
//   noiseflip.exe --process fake_skate.exe --dry-run -v
//
// The gamepad cannot be faked this way, so grab, manual, push, footplant and fingerflip are not
// exercised here (the unit tests cover them).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "skate_chain.h"

using namespace skate;

template <class T>
static void put(std::uintptr_t address, T value) { std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T)); }

static std::uintptr_t object() {
    void* p = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return reinterpret_cast<std::uintptr_t>(p);
}

int main() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));

    // The game stores the player-manager offset at base + RVA, far past this small program's image.
    const std::uintptr_t slot = base + rva::context_player_manager_offset;
    void* page = VirtualAlloc(reinterpret_cast<void*>(slot & ~std::uintptr_t{0xFFF}), 0x2000,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!page) { std::printf("fake_skate: could not reserve memory at 0x%llx\n", static_cast<unsigned long long>(slot)); return 1; }
    const std::uintptr_t manager_offset = 0x1c0;
    put<std::uint32_t>(slot, static_cast<std::uint32_t>(manager_offset));

    const auto client = object(), context = object(), manager = object(), list = object(), player = object(),
               entity = object(), component = object(), core = object(), physics = object(), selector = object(),
               collection = object(), holder = object(), board = object(), parts = object();

    put<std::uintptr_t>(client, base + rva::client_vtable);
    put<std::uint32_t>(client + off::client_state, 5);                     // starts in the menu
    put<std::uintptr_t>(client + off::client_context, context);
    put<std::uintptr_t>(context + manager_offset, manager);
    put<std::uintptr_t>(manager, base + rva::local_player_manager_vtable);
    put<std::uintptr_t>(manager + off::manager_begin, list);
    put<std::uintptr_t>(manager + off::manager_end, list + 8);
    put<std::uintptr_t>(list, player);
    put<std::uintptr_t>(player, base + rva::local_player_vtable);
    put<std::uintptr_t>(player + off::player_context, context);
    put<std::uint8_t>(player + off::player_local, 1);
    put<std::uint8_t>(player + off::player_remote, 0);
    put<std::uintptr_t>(player + off::player_entity, entity);
    put<std::uintptr_t>(entity, base + rva::skater_entity_vtable);
    put<std::uintptr_t>(entity + off::entity_player, player);
    put<std::uintptr_t>(entity + off::entity_context, context);
    put<std::uint8_t>(entity + off::entity_teleport, 0);
    put<std::uintptr_t>(entity + off::entity_component, component);
    put<std::uintptr_t>(component, base + rva::skater_component_vtable);
    put<std::uintptr_t>(component + off::component_core, core);
    put<std::uintptr_t>(core, base + rva::bail_core_vtable);
    put<std::uintptr_t>(core + off::core_context, physics);
    put<std::uintptr_t>(core + off::core_selector, selector);
    put<std::uintptr_t>(selector + off::selector_context, physics);
    put<std::uint32_t>(physics + off::context_state, 100);

    // position: a transform collection that points back to the entity
    put<std::uintptr_t>(entity + off::entity_transform, collection);
    put<std::uintptr_t>(collection, entity);
    put<std::uint8_t>(collection + 8, 4);
    put<std::uint8_t>(collection + 9, 1);
    put<std::uint8_t>(collection + 10, 1);
    const std::uintptr_t matrix = collection + off::matrix_base + (1 + 2 * 1) * 0x20;

    // the board: core -> holder -> board physics -> bodies (9), body 0 is the root
    put<std::uintptr_t>(core + off::core_board_holder, holder);
    put<std::uintptr_t>(holder + off::holder_board, board);
    put<std::uintptr_t>(board, base + rva::board_physics_vtable);
    put<std::uintptr_t>(board + off::physics_bodies, parts);
    put<std::uint32_t>(parts, off::board_body_count);
    put<std::uintptr_t>(parts + off::body_owner, board);

    auto set_motion = [&](float y, float speed, float wx, float wy, float wz) {
        put<float>(matrix + off::matrix_position, 12.0f);
        put<float>(matrix + off::matrix_position + 4, y);
        put<float>(matrix + off::matrix_position + 8, -3.0f);
        put<float>(matrix + off::matrix_position + 12, 1e30f);      // the 4th lane is not a coordinate
        put<float>(parts + off::body_velocity, speed);
        put<float>(parts + off::body_velocity + 4, 0.0f);
        put<float>(parts + off::body_velocity + 8, 0.0f);
        put<float>(parts + off::body_spin, wx);
        put<float>(parts + off::body_spin + 4, wy);
        put<float>(parts + off::body_spin + 8, wz);
    };
    set_motion(0, 0, 0, 0, 0);

    std::printf("fake_skate: image base 0x%llx, client object at 0x%llx\n",
                static_cast<unsigned long long>(base), static_cast<unsigned long long>(client));
    std::printf("fake_skate: menu for 2 s, then the scripted session (repeats). Ctrl+C to stop.\n");
    std::fflush(stdout);

    // y goes from y0 to y1 over the step; spin is in radians per second (16 rad/s = 917 deg/s)
    struct Step { std::uint32_t state; int ms; float y0, y1, speed, wx, wy, wz; const char* what; };
    const Step script[] = {
        {100, 1200, 0, 0, 4, 0, 0, 0, "roll"},
        {103, 150, 0, 0, 4, 0, 0, 0, "crouch"},
        {200, 20, 0, 0.05f, 4, 0, 0, 0, "pop"},
        {201, 250, 0.05f, 0.5f, 4, 1.5f, 0, 0, "ollie up (slow pitch, 86 deg/s: no rotation note)"},
        {201, 250, 0.5f, 0, 4, 1.5f, 0, 0, "ollie down"},
        {100, 1000, 0, 0, 4, 0, 0, 0, "flat landing (drop 0.5 m)"},

        {201, 150, 0, -0.15f, 4, 0, 0, 0, "roll off a 15 cm curb"},
        {100, 1000, -0.15f, -0.15f, 4, 0, 0, 0, "curb landing (drop 0.15 m: quiet)"},

        {200, 20, -0.15f, -0.1f, 6, 0, 0, 0, "pop for a big air"},
        {201, 400, -0.1f, 1.6f, 6, 0, 0, 16.0f, "rising, kickflip (16 rad/s about z)"},
        {201, 350, 1.6f, 3.0f, 6, 0, 0, 0, "up to the apex (3.15 m above the landing)"},
        {201, 900, 3.0f, 0, 6, 0, 0, 0, "falling"},
        {100, 1200, 0, 0, 6, 0, 0, 0, "big landing (drop 3 m: loud)"},

        {201, 600, 0, 0.6f, 5, 0, 15.0f, 0, "shuvit (15 rad/s about y)"},
        {201, 300, 0.6f, 0, 5, 0, 0, 0, "down"},
        {100, 1000, 0, 0, 5, 0, 0, 0, "landing"},

        {100, 330, 0, 0, 4, 0, 10.5f, 0, "REVERT: 198 deg in 0.33 s on the ground (plays the shuvit note)"},
        {100, 600, 0, 0, 4, 0, 0, 0, "roll"},
        {100, 2500, 0, 0, 5, 0, 2.0f, 0, "carving, 115 deg/s for 2.5 s (must NOT trigger)"},
        {100, 1500, 0, 0, 5, 0, 4.36f, 0, "hard carve, 250 deg/s (under ground_min_rate: must NOT trigger)"},
        {100, 600, 0, 0, 4, 0, 0, 0, "roll"},

        {402, 700, 0, 0, 4, 0, 0, 0, "grind"},
        {201, 400, 0, 0.3f, 4, 0, 0, 0, "off the rail"},
        {100, 900, 0, 0, 4, 0, 0, 0, "roll"},

        {201, 400, 0, 0.8f, 5, 0, 0, 0, "jump, then a wipeout"},
        {300, 120, 0.8f, 0.7f, 5, 0, 0, 0, "wipeout"},
        {504, 1200, 0.7f, 0, 0, 0, 0, 0, "bail"},
        {100, 1500, 0, 0, 0, 0, 0, 0, "respawn on the board"},
        {201, 300, 0, 0.4f, 4, 0, 0, 0, "jump"},
        {504, 600, 0.4f, 0, 0, 0, 0, 0, "bail whose ragdoll flickers..."},
        {300, 80, 0, 0, 0, 0, 0, 0, "..."},
        {504, 400, 0, 0, 0, 0, 0, 0, "... in and out of 504 (still ONE bail note)"},
        {100, 150, 0, 0, 0, 0, 0, 0, "back on the board for only 150 ms"},
        {504, 300, 0, 0, 0, 0, 0, 0, ""},
        {100, 1500, 0, 0, 0, 0, 0, 0, "respawn"},

        {200, 15, 0, 0.1f, 4, 0, 0, 0, "flicker burst..."}, {100, 15, 0, 0, 4, 0, 0, 0, ""}, {200, 15, 0, 0.1f, 4, 0, 0, 0, ""},
        {100, 15, 0, 0, 4, 0, 0, 0, ""}, {201, 15, 0, 0.1f, 4, 0, 0, 0, ""}, {100, 1200, 0, 0, 4, 0, 0, 0, "... one jump, one land"},
    };

    for (;;) {
        put<std::uint32_t>(client + off::client_state, 5);
        set_motion(0, 0, 0, 0, 0);
        std::printf("fake_skate: menu\n"); std::fflush(stdout);
        Sleep(2000);
        put<std::uint32_t>(client + off::client_state, 13);
        std::printf("fake_skate: in a level\n"); std::fflush(stdout);
        for (const auto& s : script) {
            put<std::uint32_t>(physics + off::context_state, s.state);
            if (s.what[0]) { std::printf("fake_skate: state %u  %s\n", s.state, s.what); std::fflush(stdout); }
            const int steps = s.ms / 5 > 0 ? s.ms / 5 : 1;
            for (int i = 0; i < steps; ++i) {
                const float t = static_cast<float>(i + 1) / static_cast<float>(steps);
                set_motion(s.y0 + (s.y1 - s.y0) * t, s.speed, s.wx, s.wy, s.wz);
                Sleep(5);
            }
        }
    }
}
