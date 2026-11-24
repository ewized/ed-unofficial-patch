#pragma once

#include <d3d11.h>

namespace edvr {

bool renderBoundaryRegisterOwner(ID3D11Device* device, ID3D11DeviceContext* context) noexcept;
// Clients issuing render-thread GPU work also require the thread of the first owned Present.
bool renderBoundaryValidateOwner(ID3D11Device* device, ID3D11DeviceContext* context,
                                 bool requireRenderThread = false) noexcept;
void renderBoundaryNoteOwnedPresent(ID3D11Device* device) noexcept;
void renderBoundaryPresent(ID3D11Device* device) noexcept;

}
