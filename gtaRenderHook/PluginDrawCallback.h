// Dispatches per-mesh building draw notifications registered via RHSetBuildingDrawCallback (RenderHookAPI.h).
#pragma once
#include "RenderHookAPI.h"

int CurrentBuildingDrawPass();

// Returns RH_DRAW_* flags from the active callback, or 0 when no callback is registered.
unsigned int NotifyBuildingDraw( void *atomic, const RwMatrix *worldMatrix, int meshIndex, int pass,
                                 bool dualPassEdges );
