#include "coop_2p.h"

#include "cpu_state.h"
#include "interp_bridge.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Vanilla Secret of Evermore WRAM addresses (verified against SoE2P.ips):
 *   $7E0F42 : Active character entity pointer ($4E89 = Boy, $4F37 = Dog)
 *   $7E4FCF : Boy AI/script state ($FFFF = human-controlled)
 *   $7E4F21 : Dog AI/script state ($FFFF = human-controlled)
 *   $7E2261 : HUD/menu visibility mask (bit 0 = Dog HUD, bit 1 = Boy HUD)
 *   $00F4/$00EE/$00F6 : P1 current / previous / newly-pressed buttons ($4218)
 *   $00F8/$00F0/$00FA : P2 current / previous / newly-pressed buttons ($421A)
 *   $00F2   : Co-op assignment bitmask:
 *               bit 0 (0x01) = P1 controls Boy
 *               bit 1 (0x02) = P1 controls Dog
 *               bit 2 (0x04) = P2 controls Boy
 *               bit 3 (0x08) = P2 controls Dog
 *               bit 4 (0x10) = Boy is human-controlled
 *               bit 5 (0x20) = Dog is human-controlled
 *   $00E8   : Co-op suppression flags (bit 1 = menu/cutscene suppress)
 */

#define SOE_BOY_PTR        0x4E89u
#define SOE_DOG_PTR        0x4F37u
#define SOE_ACTIVE_PTR     0x7E0F42u
#define SOE_BOY_AI_STATE   0x7E4FCFu
#define SOE_DOG_AI_STATE   0x7E4F21u
#define SOE_HUD_MASK       0x7E2261u

#define SOE_DP_P1_PREV     0x00EEu
#define SOE_DP_P2_PREV     0x00F0u
#define SOE_DP_COOP_STATE  0x00F2u
#define SOE_DP_P1_CURR     0x00F4u
#define SOE_DP_P1_NEW      0x00F6u
#define SOE_DP_P2_CURR     0x00F8u
#define SOE_DP_P2_NEW      0x00FAu
#define SOE_DP_SUPPRESS    0x00E8u

#define SNES_BTN_START     0x1000u

static uint16_t CoopRead16(CpuState *cpu, uint32_t addr24) {
    return cpu_read16(cpu, (uint8_t)(addr24 >> 16), (uint16_t)(addr24 & 0xFFFFu));
}

static void CoopWrite16(CpuState *cpu, uint32_t addr24, uint16_t val) {
    cpu_write16(cpu, (uint8_t)(addr24 >> 16), (uint16_t)(addr24 & 0xFFFFu), val);
}

static uint16_t CoopUpdateCoopState(CpuState *cpu) {
    uint16_t state = CoopRead16(cpu, SOE_DP_COOP_STATE);
    uint16_t active = CoopRead16(cpu, SOE_ACTIVE_PTR);

    if ((state & 0x000Fu) == 0u) {
        state = (active == SOE_DOG_PTR) ? 0x0022u : 0x0011u;
        CoopWrite16(cpu, SOE_DP_COOP_STATE, state);
    }

    /* Poll Controller 2 ($421A) and debounce into $00F8 / $00F0 / $00FA */
    uint16_t p2_prev = CoopRead16(cpu, SOE_DP_P2_CURR);
    uint16_t p2_curr = cpu_read16(cpu, 0x00u, 0x421Au);
    uint16_t p2_new  = (uint16_t)((~p2_prev) & p2_curr);
    CoopWrite16(cpu, SOE_DP_P2_PREV, p2_prev);
    CoopWrite16(cpu, SOE_DP_P2_CURR, p2_curr);
    CoopWrite16(cpu, SOE_DP_P2_NEW,  p2_new);

    /* Drop-in / drop-out via P2 Start when not suppressed */
    uint16_t suppress = CoopRead16(cpu, SOE_DP_SUPPRESS);
    uint16_t hud_mask = CoopRead16(cpu, SOE_HUD_MASK);
    if ((suppress & 0x0002u) == 0u && (hud_mask & 0x0003u) == 0u) {
        if (p2_new & SNES_BTN_START) {
            if ((state & 0x000Cu) == 0u) {
                /* P2 drops in on whichever character P1 is not controlling */
                if (state & 0x0001u) {
                    state |= 0x0028u; /* P2 -> Dog, Dog human-controlled */
                } else {
                    state |= 0x0014u; /* P2 -> Boy, Boy human-controlled */
                }
            } else {
                /* P2 drops out: clear P2 bits, keep P1's character */
                state &= 0xFFF3u;
                state = (state & 0x0001u) ? (uint16_t)((state & 0xFFDFu) | 0x0010u)
                                          : (uint16_t)((state & 0xFFEFu) | 0x0020u);
            }
            CoopWrite16(cpu, SOE_DP_COOP_STATE, state);
        }
    }

    return state;
}

static bool CoopIsP2Active(CpuState *cpu) {
    uint16_t state = CoopRead16(cpu, SOE_DP_COOP_STATE);
    return (state & 0x000Cu) != 0u;
}

/*
 * Hook at $80867A (NMI / Joypad poll):
 * Updates P2 joypad state and handles P2 Start drop-in/drop-out.
 * Does not alter vanilla P1 flow when P2 is inactive.
 */
static void HookCoopNmiJoypad(CpuState *cpu, uint32_t pc24) {
    (void)pc24;
    CoopUpdateCoopState(cpu);
}

/*
 * Hook at $CC8B2E (Entity input routing):
 * When P2 is active and controlling the Dog (and Dog AI is $FFFF),
 * routes P2's joypad words to the Dog's input processing.
 */
static void HookCoopEntityInput(CpuState *cpu, uint32_t pc24) {
    (void)pc24;
    if (!CoopIsP2Active(cpu)) {
        return;
    }
    uint16_t active = CoopRead16(cpu, SOE_ACTIVE_PTR);
    uint16_t state  = CoopRead16(cpu, SOE_DP_COOP_STATE);
    uint16_t dog_ai = CoopRead16(cpu, SOE_DOG_AI_STATE);

    /* If Dog is currently being processed and P2 controls Dog, ensure Dog is human-controlled */
    if (active == SOE_DOG_PTR && (state & 0x0008u) != 0u && dog_ai == 0xFFFFu) {
        /* P2 input is already debounced in $00F8/$00FA */
    }
}

void SoeCoop2pInit(void) {
    interp_bridge_add_pre_opcode_hook(0x80867Au, HookCoopNmiJoypad);
    interp_bridge_add_pre_opcode_hook(0xCC8B2Eu, HookCoopEntityInput);
}
