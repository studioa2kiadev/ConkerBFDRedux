// From CBFD-Recompiled (Copyright (c) 2026 Sean Ciaschi, MIT License; see
// recomp/THIRD_PARTY_LICENSE), used unchanged except for this note.

// Frame interpolation help, called from hooks in conker.toml.
//
// RT64 draws frames between the game's by pairing each transform with one from the
// previous frame. Without help it guesses, from draw calls that look alike, and the
// game batches a character's triangles differently from frame to frame, so some of
// Conker's parts were paired with others and jumped about. func_1502CCFC draws a game
// object (from gObjects) into a display list: its commands are wrapped in an RT64
// matrix group whose ID names the object, with linear ordering, so the n-th matrix
// of an object is always paired with the n-th of the same object last frame. An
// object whose count of matrices changed isn't paired at all for that frame (RT64).
//
// A character's shadow (func_15186794) is the ground under it, clipped anew every
// frame and drawn with the shadow's texture projected onto it: its own group asks RT64
// to interpolate its texture coordinates from last frame's projection (rt64.patch),
// so the shadow slides with the character instead of stepping at the game's rate.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "recomp.h"

namespace {
    // RT64's extended GBI (tools/rt64/include/rt64_extended_gbi.h) for F3DEX2,
    // whose no-op (0xE0) carries RT64's hooks.
    constexpr uint32_t rt64_hook_opcode = 0xE0;
    constexpr uint32_t rt64_hook_magic = 0x525464;
    constexpr uint32_t rt64_hook_op_enable = 0x1;
    constexpr uint32_t rt64_extended_opcode = 0x64;
    constexpr uint32_t g_ex_matrixgroup_v1 = 0x00000C;
    constexpr uint32_t g_ex_popmatrixgroup_v1 = 0x00000D;

    constexpr uint32_t g_ex_push = 1;
    constexpr uint32_t g_ex_interpolate_simple = 0;
    constexpr uint32_t g_ex_interpolate_decompose = 1;
    constexpr uint32_t g_ex_component_skip = 0;
    constexpr uint32_t g_ex_component_interpolate = 1;
    constexpr uint32_t g_ex_component_auto = 2;
    constexpr uint32_t g_ex_order_linear = 0;

    // gEXMatrixGroup's parameters for the model matrix, matched in the order drawn:
    // mode (bit 2), the matrices' position, rotation, scale, skew and perspective,
    // vertices, tile, ordering (bit 17), texture coordinates (bit 22) and look-at.
    constexpr uint32_t group_params(uint32_t mode, uint32_t matrices, uint32_t vertices, uint32_t texcoords) {
        return (g_ex_push << 0) | (mode << 2) |
            (matrices << 3) | (matrices << 5) | (matrices << 7) |
            (matrices << 9) | (matrices << 11) | (vertices << 13) |
            (g_ex_component_auto << 15) | (g_ex_order_linear << 17) | (texcoords << 22) |
            (g_ex_component_auto << 24);
    }

    // An object: every component interpolated automatically, vertices too (the game
    // bends joints on the CPU), texture coordinates as they are.
    constexpr uint32_t object_group_params = group_params(g_ex_interpolate_decompose, g_ex_component_auto, g_ex_component_auto, g_ex_component_skip);
    // A shadow: the ground it's drawn on stays where it is (its matrices and vertices as they
    // are), and only its texture coordinates are interpolated.
    constexpr uint32_t shadow_group_params = group_params(g_ex_interpolate_simple, g_ex_component_skip, g_ex_component_skip, g_ex_component_interpolate);

    void put_command(uint8_t* rdram, gpr& dl, uint32_t w0, uint32_t w1) {
        MEM_W(0, dl) = (int32_t)w0;
        MEM_W(4, dl) = (int32_t)w1;
        dl += 8;
    }

    // Opens a matrix group at dl, with RT64's extended GBI enabled (RT64 turns it off
    // at the start of every display list).
    void put_group(uint8_t* rdram, gpr& dl, uint32_t id, uint32_t params) {
        put_command(rdram, dl, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_matrixgroup_v1, id);
        put_command(rdram, dl, params, 0);
    }

    void put_pop(uint8_t* rdram, gpr& dl) {
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_popmatrixgroup_v1, 1);
    }
}

// func_1502CCFC, after its first instruction (the stack frame): $a0 is where it
// writes its first command, $a1 the object's index, and its 7th argument (0x168 on
// the new stack) tells its callers apart (the object, its shadow...). The group's
// commands go first, and the function writes after them. IDs are neither 0
// (G_EX_ID_IGNORE) nor 0xFFFFFFFF (G_EX_ID_AUTO).
extern "C" void conker_object_matrix_group_begin(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t object = (uint32_t)ctx->r5 & 0xFFFF;
    const uint32_t kind = (uint32_t)MEM_W(0x168, ctx->r29) & 0xFF;
    gpr dl = ctx->r4;
    put_group(rdram, dl, 0x43000000u | (kind << 16) | object, object_group_params);
    ctx->r4 = dl;
}

// At its return: $v0 is the end of what it wrote. The group is popped after it.
extern "C" void conker_object_matrix_group_end(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r2;
    put_pop(rdram, dl);
    ctx->r2 = dl;
}

namespace {
    // Testing (CONKER_SHADOW_PROBE): the commands a shadow writes, to see where its texture comes from.
    gpr shadow_dl_start = 0;
    uint32_t shadow_index = 0;

    void probe_shadow_commands(uint8_t* rdram, gpr start, gpr end) {
        static const bool probe = std::getenv("CONKER_SHADOW_PROBE") != nullptr;
        static int calls = 0;
        if (!probe || (calls++ % 90) != 0) {
            return;
        }
        std::printf("[shadow %u] %u commands\n", shadow_index, (uint32_t)((end - start) / 8));
        for (gpr dl = start; dl < end; dl += 8) {
            const uint32_t w0 = (uint32_t)MEM_W(0, dl), w1 = (uint32_t)MEM_W(4, dl);
            const uint32_t op = w0 >> 24;
            if (op == 0xFD || op == 0xFF || op == 0xDE || op == 0xE0 || op == 0x64) {
                std::printf("  %08X %08X\n", w0, w1);
            }
        }
        std::fflush(stdout);
    }
}

// func_15186794, after its first instruction: $a0 is where it writes its first
// command, $a1 the shadow's index.
extern "C" void conker_shadow_matrix_group_begin(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t shadow = (uint32_t)ctx->r5 & 0xFFFF;
    gpr dl = ctx->r4;
    put_group(rdram, dl, 0x53480000u | shadow, shadow_group_params);
    ctx->r4 = dl;
    shadow_dl_start = dl;
    shadow_index = shadow;
}

namespace {
    // A silhouette: the character drawn from the light's point of view into the shadow's own small
    // image, which the ground is then textured with. Interpolated like the character itself (its
    // matrices and the vertices the game bends on the CPU), so the shadow's shape changes at the
    // display's rate instead of stepping at the game's. The game draws it through func_1502CCFC, whose
    // own object group leaves its components automatic, which RT64 appears to take from the group around
    // it: inside the shadow's (matrices and vertices skipped), the silhouette was never interpolated.
    constexpr uint32_t silhouette_group_params = object_group_params;
    // A set-color-image this narrow is a shadow's image, not the screen (the game's are 64 wide).
    constexpr uint32_t silhouette_max_width = 128;

    // Rewrites the commands a shadow wrote (start to end) with each silhouette wrapped in its own
    // group, nested in the shadow's: opened after each set-color-image to a shadow's image, closed
    // before the next set-color-image (back to the screen). Returns the new end.
    gpr wrap_silhouettes(uint8_t* rdram, gpr start, gpr end) {
        thread_local std::vector<uint32_t> in, out;
        in.clear();
        out.clear();
        for (gpr dl = start; dl < end; dl += 4) {
            in.push_back((uint32_t)MEM_W(0, dl));
        }
        // Moving these commands is only safe if nothing points into them (vertices, matrices or display
        // lists the game might have put among them): if anything does, the shadow is left as it is.
        const uint32_t lo = (uint32_t)start & 0x1FFFFFFF, hi = (uint32_t)end & 0x1FFFFFFF;
        for (size_t i = 0; i + 1 < in.size(); i += 2) {
            const uint32_t address = in[i + 1] & 0x1FFFFFFF;
            if ((address >= lo) && (address < hi)) {
                static const bool probe = std::getenv("CONKER_SHADOW_PROBE") != nullptr;
                if (probe) {
                    std::printf("[shadow %u] not wrapped: %08X %08X points inside its own commands\n", shadow_index, in[i], in[i + 1]);
                }
                return end;
            }
        }
        auto put = [](uint32_t w0, uint32_t w1) { out.push_back(w0); out.push_back(w1); };
        uint32_t segment = 0;
        bool open = false;
        for (size_t i = 0; i + 1 < in.size(); i += 2) {
            const uint32_t w0 = in[i], w1 = in[i + 1];
            const bool set_color_image = (w0 >> 24) == 0xFF;
            if (set_color_image && open) {
                put((rt64_extended_opcode << 24) | g_ex_popmatrixgroup_v1, 1);
                open = false;
            }
            put(w0, w1);
            if (set_color_image && (w0 & 0xFFF) + 1 <= silhouette_max_width) {
                const uint32_t id = 0x53590000u | ((shadow_index & 0xFF) << 8) | (segment++ & 0xFF);
                put((rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
                put((rt64_extended_opcode << 24) | g_ex_matrixgroup_v1, id);
                put(silhouette_group_params, 0);
                open = true;
            }
        }
        if (open) {
            put((rt64_extended_opcode << 24) | g_ex_popmatrixgroup_v1, 1);
        }
        gpr dl = start;
        for (uint32_t word : out) {
            MEM_W(0, dl) = (int32_t)word;
            dl += 4;
        }
        return dl;
    }
}

// At its return: $v0 is the end of what it wrote.
extern "C" void conker_shadow_matrix_group_end(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r2;
    dl = wrap_silhouettes(rdram, shadow_dl_start, dl);
    probe_shadow_commands(rdram, shadow_dl_start, dl);
    put_pop(rdram, dl);
    ctx->r2 = dl;
}
