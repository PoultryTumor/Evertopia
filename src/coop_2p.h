#pragma once

/*
 * Native 2-Player Co-Op (ported from FuSoYa's SoE 2-Player Edition).
 *
 * Completely dormant when Player 2 has not dropped in (single-player
 * behavior is 100% identical to vanilla). When a second controller is
 * connected and Player 2 presses Start, Player 2 takes control of the Dog
 * (or whichever character Player 1 is not controlling).
 */
void SoeCoop2pInit(void);
