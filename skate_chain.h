// skate_chain.h
//
// Reads the local skater's physics state from skate.'s memory, read only.
//
// Every constant below is copied from ReSkate's source (github.com/Dingo-Shenanigans/ReSkate,
// commit c733816) and is valid for ONE game build only: Steam build 25414733.
//   RVAs      : Engine/Game/Build/20260929/engine.h, no_bail.h, skater_state.h
//   The chain : Extension/Skater/no_bail.cpp, resolve()
//
// The code never writes to the game and never injects anything. It only needs a Memory object
// that can read bytes from the game process (the real one uses ReadProcessMemory; the tests use
// a fake).
#pragma once
#include <cstddef>
#include <cstdint>

namespace skate {

// RVAs: add the module base of Skate.exe to get the address.
namespace rva {
constexpr std::uintptr_t client_vtable                 = 0x60842c0;
constexpr std::uintptr_t skater_entity_vtable          = 0x6087908;
constexpr std::uintptr_t skater_component_vtable       = 0x6170410;
constexpr std::uintptr_t local_player_vtable           = 0x63e2d30;
constexpr std::uintptr_t local_player_manager_vtable   = 0x63e30a0;
constexpr std::uintptr_t bail_core_vtable              = 0x6176d30; // the skater's physics core
constexpr std::uintptr_t board_physics_vtable          = 0x65e67a0; // the board's physics (client_source_spawn.h)
constexpr std::uintptr_t context_player_manager_offset = 0x7161720; // holds a uint32 offset
}  // namespace rva

// Field offsets inside the game's objects.
namespace off {
constexpr std::uintptr_t client_state      = 0xc4;   // uint32, 13 or 21 while playing a level
constexpr std::uintptr_t client_context   = 0x8;    // pointer
constexpr std::uintptr_t manager_begin    = 0x4c8;  // pointer to the local player list
constexpr std::uintptr_t manager_end      = 0x4d0;  // must be begin + 8 (exactly one local player)
constexpr std::uintptr_t player_remote    = 0x44;   // uint8, must be 0
constexpr std::uintptr_t player_local     = 0x45;   // uint8, must be 1
constexpr std::uintptr_t player_context   = 0x78;   // pointer, must equal the client context
constexpr std::uintptr_t player_entity    = 0xb8;   // pointer to the skater entity
constexpr std::uintptr_t entity_context   = 0x20;   // pointer, must equal the client context
constexpr std::uintptr_t entity_player    = 0xf8;   // pointer, must equal the player
constexpr std::uintptr_t entity_component = 0x628; // pointer to the skater component
constexpr std::uintptr_t entity_teleport  = 0x7e0;  // uint8, nonzero while teleporting
constexpr std::uintptr_t component_core   = 0x70;   // pointer to the physics core
constexpr std::uintptr_t core_context     = 0x3c0;  // pointer to the physics context
constexpr std::uintptr_t core_selector    = 0x440;  // pointer; selector + 8 == physics context
constexpr std::uintptr_t selector_context = 0x8;
constexpr std::uintptr_t context_state    = 0x1414; // uint32: THE PHYSICS STATE

// position of the skater (Extension/Skater/client_debug.cpp, debug_skater)
constexpr std::uintptr_t entity_transform = 0x70;   // pointer to the transform collection (its first field points back to the entity)
constexpr std::uintptr_t matrix_base      = 0x10;   // matrix = collection + 0x10 + (first + 2 * extra) * 0x20
constexpr std::uintptr_t matrix_position  = 0x30;   // float[4] row: x, y, z, (not a coordinate)

// the board's physics bodies (Engine/Game/Build/20260929/skater_state.h)
constexpr std::uintptr_t core_board_holder   = 0x430;  // core -> board holder
constexpr std::uintptr_t holder_board        = 0x18;   // holder -> board physics
constexpr std::uintptr_t physics_bodies      = 0x20;   // board physics -> bodies; uint32 count (9) at their start
constexpr std::uintptr_t body_owner          = 0x10;   // body 0 is the board's root; owner must be the board physics
constexpr std::uintptr_t body_velocity       = 0x70;   // float[3], metres per second
constexpr std::uintptr_t body_spin           = 0x90;   // float[3], radians per second
constexpr std::uint32_t  board_body_count    = 9;
}  // namespace off

// Anything that can read bytes from the game process.
struct Memory {
    virtual ~Memory() = default;
    virtual bool read(std::uintptr_t address, void* out, std::size_t size) = 0;
};

constexpr std::uintptr_t highest_user_address = 0x00007fffffffffffULL;

template <class T>
bool rd(Memory& m, std::uintptr_t address, T& value) {
    if (address < 0x10000 || address > highest_user_address - sizeof(T)) return false;
    return m.read(address, &value, sizeof(T));
}
inline bool rd_ptr(Memory& m, std::uintptr_t address, std::uintptr_t& value) {
    return rd(m, address, value);
}

enum class Result {
    Ok,          // state is valid
    NoClient,    // the address is not the game's client object
    NotInLevel,  // the client exists, but no level is being played (menu, loading)
    ChainBroken, // a link did not check out (respawning, loading, wrong build)
    Teleporting, // the skater is being teleported; state is not trusted this sample
};

struct Resolved {
    Result        result = Result::NoClient;
    const char*   step   = "";   // which check failed, for messages
    std::uint32_t state  = 0;    // the physics state, when result == Ok
    std::uintptr_t physics_context = 0;
    std::uintptr_t entity = 0;   // the skater entity
    std::uintptr_t core = 0;     // the skater's physics core
};

// Follows the same links and checks as ReSkate's own resolver, so a stale pointer after a
// respawn or level change is refused instead of being read.
inline Resolved resolve(Memory& m, std::uintptr_t base, std::uintptr_t client) {
    Resolved r;
    auto fail = [&](Result why, const char* step) { r.result = why; r.step = step; return r; };

    std::uintptr_t p = 0;
    if (!rd_ptr(m, client, p) || p != base + rva::client_vtable)
        return fail(Result::NoClient, "client vtable");

    std::uint32_t client_state = 0;
    if (!rd(m, client + off::client_state, client_state))
        return fail(Result::ChainBroken, "client state unreadable");
    if (client_state != 13 && client_state != 21)
        return fail(Result::NotInLevel, "client is not in a level");

    std::uintptr_t context = 0;
    if (!rd_ptr(m, client + off::client_context, context) || !context)
        return fail(Result::ChainBroken, "client context");

    std::uint32_t manager_offset = 0;
    if (!rd(m, base + rva::context_player_manager_offset, manager_offset) || manager_offset > 0x1000000)
        return fail(Result::ChainBroken, "player manager offset");

    std::uintptr_t manager = 0;
    if (!rd_ptr(m, context + manager_offset, manager) || !manager) return fail(Result::ChainBroken, "player manager");
    if (!rd_ptr(m, manager, p) || p != base + rva::local_player_manager_vtable)
        return fail(Result::ChainBroken, "player manager vtable");

    std::uintptr_t begin = 0, end = 0;
    if (!rd_ptr(m, manager + off::manager_begin, begin) || !rd_ptr(m, manager + off::manager_end, end) ||
        !begin || end != begin + 8)
        return fail(Result::ChainBroken, "expected exactly one local player");

    std::uintptr_t player = 0;
    if (!rd_ptr(m, begin, player) || !player) return fail(Result::ChainBroken, "player");
    std::uintptr_t player_context = 0, entity = 0;
    std::uint8_t local = 0, remote = 1;
    if (!rd_ptr(m, player, p) || p != base + rva::local_player_vtable)
        return fail(Result::ChainBroken, "player vtable");
    if (!rd_ptr(m, player + off::player_context, player_context) || player_context != context)
        return fail(Result::ChainBroken, "player context");
    if (!rd(m, player + off::player_local, local) || local != 1 ||
        !rd(m, player + off::player_remote, remote) || remote != 0)
        return fail(Result::ChainBroken, "player is not the local human");
    if (!rd_ptr(m, player + off::player_entity, entity) || !entity)
        return fail(Result::ChainBroken, "skater entity");

    std::uintptr_t entity_player = 0, entity_context = 0, component = 0;
    if (!rd_ptr(m, entity, p) || p != base + rva::skater_entity_vtable)
        return fail(Result::ChainBroken, "entity vtable");
    if (!rd_ptr(m, entity + off::entity_player, entity_player) || entity_player != player)
        return fail(Result::ChainBroken, "entity player");
    if (!rd_ptr(m, entity + off::entity_context, entity_context) || entity_context != context)
        return fail(Result::ChainBroken, "entity context");
    std::uint8_t teleport = 1;
    if (!rd(m, entity + off::entity_teleport, teleport)) return fail(Result::ChainBroken, "teleport flag");
    if (teleport) return fail(Result::Teleporting, "teleporting");

    if (!rd_ptr(m, entity + off::entity_component, component) || !component)
        return fail(Result::ChainBroken, "skater component");
    if (!rd_ptr(m, component, p) || p != base + rva::skater_component_vtable)
        return fail(Result::ChainBroken, "component vtable");

    std::uintptr_t core = 0;
    if (!rd_ptr(m, component + off::component_core, core) || !core) return fail(Result::ChainBroken, "physics core");
    if (!rd_ptr(m, core, p) || p != base + rva::bail_core_vtable) return fail(Result::ChainBroken, "core vtable");

    std::uintptr_t physics = 0, selector = 0, selector_context = 0;
    if (!rd_ptr(m, core + off::core_context, physics) || !physics) return fail(Result::ChainBroken, "physics context");
    if (!rd_ptr(m, core + off::core_selector, selector) || !selector ||
        !rd_ptr(m, selector + off::selector_context, selector_context) || selector_context != physics)
        return fail(Result::ChainBroken, "selector");

    std::uint32_t state = 0;
    if (!rd(m, physics + off::context_state, state)) return fail(Result::ChainBroken, "physics state unreadable");

    r.result = Result::Ok;
    r.step = "";
    r.state = state;
    r.physics_context = physics;
    r.entity = entity;
    r.core = core;
    return r;
}

// How the skater moves: where it is (world position, y is up) and what the board's root body does.
struct RawMotion {
    float position[3] = {0, 0, 0};   // metres
    float velocity[3] = {0, 0, 0};   // metres per second
    float spin[3] = {0, 0, 0};       // radians per second, board root body
};

inline bool finite_f(float v, float limit) { return v == v && v >= -limit && v <= limit; }

// Reads the skater's position and the board's velocity and spin, with the same checks ReSkate uses.
// `why` names the failing check.
inline bool read_motion(Memory& m, std::uintptr_t base, const Resolved& r, RawMotion& out, const char*& why) {
    why = "";
    // position
    std::uintptr_t collection = 0, owner = 0;
    if (!rd_ptr(m, r.entity + off::entity_transform, collection) || !collection || !rd_ptr(m, collection, owner) || owner != r.entity) {
        why = "transform collection";
        return false;
    }
    std::uint8_t count = 0, first = 0, extra = 0;
    if (!rd(m, collection + 8, count) || !rd(m, collection + 9, first) || !rd(m, collection + 10, extra) ||
        count > 128 || first > 128 || extra > 32) {
        why = "transform layout";
        return false;
    }
    const std::uintptr_t matrix = collection + off::matrix_base + (std::uintptr_t{first} + 2 * std::uintptr_t{extra}) * 0x20;
    float row[4] = {0, 0, 0, 0};
    if (!m.read(matrix + off::matrix_position, row, sizeof row)) { why = "position unreadable"; return false; }
    for (int i = 0; i < 3; ++i)
        if (!finite_f(row[i], 1000000.0f)) { why = "position out of bounds"; return false; }

    // board root body
    std::uintptr_t holder = 0, board = 0, parts = 0, vt = 0, body_owner = 0;
    if (!rd_ptr(m, r.core + off::core_board_holder, holder) || !holder ||
        !rd_ptr(m, holder + off::holder_board, board) || !board || !rd_ptr(m, board, vt) || vt != base + rva::board_physics_vtable) {
        why = "board physics";
        return false;
    }
    std::uint32_t body_count = 0;
    if (!rd_ptr(m, board + off::physics_bodies, parts) || !parts || !rd(m, parts, body_count) || body_count != off::board_body_count) {
        why = "board bodies";
        return false;
    }
    if (!rd_ptr(m, parts + off::body_owner, body_owner) || body_owner != board) { why = "board body owner"; return false; }
    float velocity[3] = {0, 0, 0}, spin[3] = {0, 0, 0};
    if (!m.read(parts + off::body_velocity, velocity, sizeof velocity) || !m.read(parts + off::body_spin, spin, sizeof spin)) {
        why = "board motion unreadable";
        return false;
    }
    for (int i = 0; i < 3; ++i)
        if (!finite_f(velocity[i], 100000.0f) || !finite_f(spin[i], 100000.0f)) { why = "board motion invalid"; return false; }

    for (int i = 0; i < 3; ++i) { out.position[i] = row[i]; out.velocity[i] = velocity[i]; out.spin[i] = spin[i]; }
    return true;
}

}  // namespace skate
