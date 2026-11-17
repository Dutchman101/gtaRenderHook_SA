// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include <cstring>
#include "MtaBridge.h"
#include "CDebug.h"

namespace
{
    // Pre transformed screen vertex: position in pixels, rhw at one.
    struct SScreenVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        float u;
        float v;
    };

    const DWORD SCREEN_VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

    template <typename T>
    void ReleaseInterface( T*& pInterface )
    {
        if ( pInterface != nullptr )
        {
            pInterface->Release();
            pInterface = nullptr;
        }
    }

    // Lost, needs reset, or removed. None is worth drawing into.
    bool IsDeviceUnavailable( HRESULT hr )
    {
        return hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET || hr == D3DERR_DEVICEREMOVED;
    }
} // namespace

CMtaBridge::~CMtaBridge()
{
    Shutdown();
}

bool CMtaBridge::Init( ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain, HWND hWnd )
{
    Shutdown();

    if ( pDevice == nullptr || pContext == nullptr || pSwapChain == nullptr || hWnd == nullptr )
    {
        g_pDebug->printError( "MTA bridge: device, context, swap chain or window was missing" );
        return false;
    }

    m_pDevice = pDevice;
    m_pDevice->AddRef();
    m_pContext = pContext;
    m_pContext->AddRef();
    m_pSwapChain = pSwapChain;
    m_pSwapChain->AddRef();
    m_hWnd = hWnd;

    // Frame size comes from the swap chain buffer, and everything below is built to it.
    if ( !AcquireBackBuffer() )
    {
        Shutdown();
        return false;
    }

    if ( !CreateD3D9Device() )
    {
        Shutdown();
        return false;
    }

    if ( !CreateTargets() )
    {
        // The 9 device exists and MTA is using it, so it stays and only presentation is dropped.
        g_pDebug->printError( "MTA bridge: the frame surfaces could not be created, presenting through the swap chain instead" );
        ReleaseInterface( m_pBackBuffer );
        return false;
    }

    m_bActive = true;
    // Closed again, since holding a chain buffer stops the engine resizing the chain.
    ReleaseInterface( m_pBackBuffer );
    g_pDebug->printMsg( m_bShared ? "MTA bridge: presenting through Direct3D 9, shared surface" : "MTA bridge: presenting through Direct3D 9, frame copy", 1 );
    return true;
}

void CMtaBridge::Shutdown()
{
    ReleaseTargets();

    ReleaseInterface( m_pDevice9 );
    ReleaseInterface( m_pD3D9 );
    ReleaseInterface( m_pSwapChain );
    ReleaseInterface( m_pContext );
    ReleaseInterface( m_pDevice );

    m_hWnd = nullptr;
    m_uiSourceWidth = 0;
    m_uiSourceHeight = 0;
    m_uiSurfaceWidth = 0;
    m_uiSurfaceHeight = 0;
    m_TextureFormat = D3DFMT_UNKNOWN;
    m_bActive = false;
    m_bShared = false;
    m_bDeviceLost = false;
    m_bSwizzle = false;
    m_bVSync = true;
    m_bIntervalMismatchReported = false;
    m_bSizeMismatchReported = false;
    m_bResetFailureReported = false;
    m_bBackBufferFailureReported = false;
    m_bBackBufferSizeReported = false;
}

bool CMtaBridge::CreateD3D9Device()
{
    // The export is only the host's when its module is already loaded, and loading it here cannot make it so.
    HMODULE hD3D9 = GetModuleHandleA( "d3d9.dll" );
    if ( hD3D9 == nullptr )
    {
        // The error call, not a debug message: that one is filtered out at the default debug level.
        g_pDebug->printError( "MTA bridge: d3d9.dll was not loaded at device setup, so its export is the "
                              "original one and no client interface attaches to the frame, presenting "
                              "through the swap chain instead" );
        return false;
    }

    typedef IDirect3D9* (WINAPI *PDirect3DCreate9)(UINT);
    PDirect3DCreate9 pCreate = reinterpret_cast<PDirect3DCreate9>( GetProcAddress( hD3D9, "Direct3DCreate9" ) );
    if ( pCreate == nullptr )
    {
        g_pDebug->printError( "MTA bridge: the Direct3DCreate9 export was not found" );
        return false;
    }

    m_pD3D9 = pCreate( D3D_SDK_VERSION );
    if ( m_pD3D9 == nullptr )
    {
        g_pDebug->printError( "MTA bridge: Direct3DCreate9 returned no interface" );
        return false;
    }

    D3DPRESENT_PARAMETERS parameters{};
    FillPresentParameters( parameters );

    // Software vertex processing covers the one screen quad, and FPU preserve keeps the thread on its own precision, else the device drops it to single.
    // Must stay a single create on this factory: MTA's wrapper only fully handles its first call, so a second device would never be wrapped.
    HRESULT hr = m_pD3D9->CreateDevice( D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, m_hWnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters, &m_pDevice9 );
    if ( FAILED( hr ) || m_pDevice9 == nullptr )
    {
        // A failed create can still store a device, so it is released rather than dropped.
        ReleaseInterface( m_pDevice9 );
        char message[160];
        snprintf( message, sizeof( message ), "MTA bridge: the Direct3D 9 device could not be created (%08x)", static_cast<unsigned int>( hr ) );
        g_pDebug->printError( message );
        return false;
    }

    // Sharing is decided by trying it in CreateTargets, since the Ex factory id can refuse and a refusal would read as unavailable.
    return true;
}

void CMtaBridge::FillPresentParameters( D3DPRESENT_PARAMETERS& parameters ) const
{
    parameters = D3DPRESENT_PARAMETERS{};
    parameters.BackBufferWidth = m_uiSourceWidth;
    parameters.BackBufferHeight = m_uiSourceHeight;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.BackBufferCount = 1;
    parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    parameters.MultiSampleQuality = 0;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = m_hWnd;
    parameters.Windowed = TRUE;
    // MTA's wrapper turns the depth flag on itself, and a device asking for depth without a format is refused, so the format has to be set.
    parameters.EnableAutoDepthStencil = TRUE;
    parameters.AutoDepthStencilFormat = D3DFMT_D24S8;
    parameters.Flags = 0;
    parameters.FullScreen_RefreshRateInHz = 0;
    parameters.PresentationInterval = m_bVSync ? D3DPRESENT_INTERVAL_DEFAULT : D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool CMtaBridge::CreateTargets()
{
    if ( !AcquireBackBuffer() )
        return false;

    m_uiSurfaceWidth = m_uiSourceWidth;
    m_uiSurfaceHeight = m_uiSourceHeight;

    // Shared first, it moves nothing through system memory, and the create inside is the only reliable answer about sharing.
    m_bShared = CreateSharedTarget();
    if ( !m_bShared )
        return CreateReadbackTarget();

    return true;
}

bool CMtaBridge::AcquireBackBuffer()
{
    ReleaseInterface( m_pBackBuffer );

    if ( FAILED( m_pSwapChain->GetBuffer( 0, __uuidof( ID3D11Texture2D ), reinterpret_cast<void**>( &m_pBackBuffer ) ) ) || m_pBackBuffer == nullptr )
    {
        // A refused call can still store a buffer, and a leftover one blocks the engine chain resize.
        ReleaseInterface( m_pBackBuffer );

        // Runs every frame while active, so reported once.
        if ( !m_bBackBufferFailureReported )
        {
            m_bBackBufferFailureReported = true;
            g_pDebug->printError( "MTA bridge: the swap chain buffer could not be opened" );
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    m_pBackBuffer->GetDesc( &description );
    m_uiSourceWidth = description.Width;
    m_uiSourceHeight = description.Height;

    if ( m_uiSourceWidth == 0 || m_uiSourceHeight == 0 )
    {
        // Nothing can be drawn into a buffer with no extent, and holding it blocks the chain resize. A minimized window lands here each frame.
        ReleaseInterface( m_pBackBuffer );
        if ( !m_bBackBufferSizeReported )
        {
            m_bBackBufferSizeReported = true;
            g_pDebug->printError( "MTA bridge: the swap chain buffer reported no size" );
        }
        return false;
    }

    // A buffer that opened re-arms both reports, so a failure that happens later is still told.
    m_bBackBufferFailureReported = false;
    m_bBackBufferSizeReported = false;
    return true;
}

bool CMtaBridge::CreateSharedTarget()
{
    D3D11_TEXTURE2D_DESC sourceDescription{};
    m_pBackBuffer->GetDesc( &sourceDescription );

    // Only one format pair fits a shared resource copy, so anything else takes the readback route.
    if ( sourceDescription.Format != DXGI_FORMAT_R8G8B8A8_UNORM )
        return false;

    if ( FAILED( m_pD3D9->CheckDeviceFormat( D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_A8B8G8R8 ) ) )
        return false;

    D3D11_TEXTURE2D_DESC sharedDescription{};
    sharedDescription.Width = m_uiSourceWidth;
    sharedDescription.Height = m_uiSourceHeight;
    sharedDescription.MipLevels = 1;
    sharedDescription.ArraySize = 1;
    sharedDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sharedDescription.SampleDesc.Count = 1;
    sharedDescription.SampleDesc.Quality = 0;
    sharedDescription.Usage = D3D11_USAGE_DEFAULT;
    sharedDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    sharedDescription.CPUAccessFlags = 0;
    // Only 2D textures with a single mip level can be shared, and the plain flag publishes the handle.
    sharedDescription.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    if ( FAILED( m_pDevice->CreateTexture2D( &sharedDescription, nullptr, &m_pShared ) ) || m_pShared == nullptr )
    {
        ReleaseInterface( m_pShared );
        return false;
    }

    IDXGIResource* pResource = nullptr;
    if ( FAILED( m_pShared->QueryInterface( __uuidof( IDXGIResource ), reinterpret_cast<void**>( &pResource ) ) ) || pResource == nullptr )
    {
        // Cleared here too, so no exit leaves it set. Nothing else owns the pointer.
        ReleaseInterface( pResource );
        ReleaseInterface( m_pShared );
        return false;
    }

    HANDLE hShared = nullptr;
    const HRESULT hrHandle = pResource->GetSharedHandle( &hShared );
    pResource->Release();
    if ( FAILED( hrHandle ) || hShared == nullptr )
    {
        ReleaseInterface( m_pShared );
        return false;
    }

    // The 9 side opens the same memory, so the draw reads what the copy wrote. Only ever sampled, so no target usage.
    if ( FAILED( m_pDevice9->CreateTexture( m_uiSourceWidth, m_uiSourceHeight, 1, 0, D3DFMT_A8B8G8R8, D3DPOOL_DEFAULT, &m_pTexture9, &hShared ) ) ||
         m_pTexture9 == nullptr )
    {
        ReleaseInterface( m_pTexture9 );
        ReleaseInterface( m_pShared );
        return false;
    }

    m_TextureFormat = D3DFMT_A8B8G8R8;
    return true;
}

bool CMtaBridge::CreateReadbackTarget()
{
    D3D11_TEXTURE2D_DESC stagingDescription{};
    m_pBackBuffer->GetDesc( &stagingDescription );

    // The copy swaps channels by byte position, so only the four channel formats it knows can be read.
    switch ( stagingDescription.Format )
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            break;
        default:
            g_pDebug->printError( "MTA bridge: the frame format is not one the copy route can read, presenting through the swap chain instead" );
            return false;
    }

    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;

    if ( FAILED( m_pDevice->CreateTexture2D( &stagingDescription, nullptr, &m_pStaging ) ) || m_pStaging == nullptr )
    {
        // Cleared here too, so no exit leaves it set.
        g_pDebug->printError( "MTA bridge: the frame copy target could not be created" );
        ReleaseInterface( m_pStaging );
        return false;
    }

    // Same byte order as the frame means no channel work, otherwise the outer channels are swapped.
    const bool bFrameIsRedFirst = stagingDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM || stagingDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    // Asks about the managed texture below, which carries no usage flags.
    const bool bWideOrderAvailable = SUCCEEDED( m_pD3D9->CheckDeviceFormat( D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                                                              D3DRTYPE_TEXTURE, D3DFMT_A8B8G8R8 ) );

    m_TextureFormat = bWideOrderAvailable ? D3DFMT_A8B8G8R8 : D3DFMT_A8R8G8B8;

    const bool bTextureIsRedFirst = m_TextureFormat == D3DFMT_A8B8G8R8;
    m_bSwizzle = bFrameIsRedFirst != bTextureIsRedFirst;

    // Managed textures are lockable everywhere and take no usage flags, so dynamic would make this fail on every device.
    if ( FAILED( m_pDevice9->CreateTexture( m_uiSourceWidth, m_uiSourceHeight, 1, 0, m_TextureFormat, D3DPOOL_MANAGED, &m_pTexture9, nullptr ) ) ||
         m_pTexture9 == nullptr )
    {
        // Cleared here too. MTA's device wrapper returns before storing on failure, so this is hygiene.
        g_pDebug->printError( "MTA bridge: the frame texture could not be created" );
        ReleaseInterface( m_pTexture9 );
        ReleaseInterface( m_pStaging );
        return false;
    }

    return true;
}

bool CMtaBridge::ResetD3D9()
{
    D3DPRESENT_PARAMETERS parameters{};
    FillPresentParameters( parameters );

    const HRESULT hr = m_pDevice9->Reset( &parameters );
    if ( FAILED( hr ) )
    {
        if ( hr == D3DERR_DEVICEREMOVED )
        {
            // Removed never comes back, so the caller stops retrying and reports it.
            m_bDeviceLost = false;
        }
        else if ( hr == D3DERR_DEVICELOST )
            m_bDeviceLost = true;
        else
        {
            // Anything else doesnt clear on its own, so the flag is cleared and the caller told not to retry.
            m_bDeviceLost = false;
            if ( !m_bResetFailureReported )
            {
                // Reported once, not once per retried frame.
                m_bResetFailureReported = true;
                char message[160];
                snprintf( message, sizeof( message ), "MTA bridge: the Direct3D 9 device could not be reset (%08x)", static_cast<unsigned int>( hr ) );
                g_pDebug->printError( message );
            }
        }
        return false;
    }

    // A reset that worked re-arms the report, so a failure that happens later is still told.
    m_bResetFailureReported = false;
    return true;
}

void CMtaBridge::NoteRequestedVSync( bool bVSync )
{
    if ( bVSync == m_bVSync || m_bIntervalMismatchReported )
        return;

    // The interval is fixed at creation and Reset keeps it. Following the setting would mean a reset, which makes the interface rebuild everything.
    m_bIntervalMismatchReported = true;
    g_pDebug->printMsg( "MTA bridge: the requested swap interval differs from the one the device was created with, keeping the created interval", 1 );
}

bool CMtaBridge::RecoverDevice()
{
    const HRESULT hrCooperative = m_pDevice9->TestCooperativeLevel();
    if ( hrCooperative == D3DERR_DEVICEREMOVED )
    {
        // Removed never comes back, so the caller stops retrying from here on.
        m_bDeviceLost = false;
        return false;
    }
    if ( hrCooperative == D3DERR_DEVICELOST )
    {
        // Only loss clears on its own, and the caller decides from this flag whether to retry.
        m_bDeviceLost = true;
        return false;
    }
    if ( FAILED( hrCooperative ) && hrCooperative != D3DERR_DEVICENOTRESET )
    {
        // Nothing else clears on its own, so this isnt worth another attempt.
        m_bDeviceLost = false;
        return false;
    }

    ReleaseTargets();

    // A loss usually comes from the resize that changed the frame size, so the new size is read here.
    if ( !AcquireBackBuffer() )
    {
        // Surfaces are gone, so the repair stays pending, else the next frame has nothing that rebuilds one.
        m_bDeviceLost = true;
        return false;
    }
    ReleaseInterface( m_pBackBuffer );

    if ( !ResetD3D9() )
        return false;

    m_bDeviceLost = false;
    return CreateTargets();
}

bool CMtaBridge::Composite( bool bVSync )
{
    // No chain buffer reference is left behind on any path out, since holding one blocks the engine resize.
    if ( !m_bActive || m_pDevice9 == nullptr || m_pContext == nullptr )
    {
        ReleaseInterface( m_pBackBuffer );
        return false;
    }

    // Recovered before the frame opens: a lost device has no usable frame to compare sizes with.
    if ( m_bDeviceLost && !RecoverDevice() )
    {
        // Still lost is worth another try. Anything else leaves no route through this device.
        if ( !m_bDeviceLost )
        {
            m_bActive = false;
            g_pDebug->printError( "MTA bridge: presentation through Direct3D 9 was abandoned after the device could not be recovered" );
        }
        // The repair opened the chain buffer on its way through, so it is closed here.
        ReleaseInterface( m_pBackBuffer );
        return false;
    }

    // Opened for this frame and closed before returning, else the engine's chain resize fails.
    if ( !AcquireBackBuffer() )
        return false;

    if ( m_uiSourceWidth != m_uiSurfaceWidth || m_uiSourceHeight != m_uiSurfaceHeight )
    {
        // Surfaces are built at a fixed size, so another size needs the reset a lost device takes.
        if ( !RecoverDevice() )
        {
            // Lost again between the two recoveries, so retry next frame. Anything else has no route.
            if ( !m_bDeviceLost )
            {
                m_bActive = false;
                g_pDebug->printError( "MTA bridge: presentation through Direct3D 9 was abandoned after the device could not be rebuilt at the new frame size" );
            }
            ReleaseInterface( m_pBackBuffer );
            return false;
        }

        if ( !m_bSizeMismatchReported )
        {
            m_bSizeMismatchReported = true;
            g_pDebug->printMsg( "MTA bridge: the present surfaces were rebuilt at the new frame size", 1 );
        }

        // Rebuilt surfaces hold no frame yet, so this frame goes to the chain.
        ReleaseInterface( m_pBackBuffer );
        return false;
    }

    if ( m_pTexture9 == nullptr )
    {
        // Cant happen while the checks above hold. Kept so a missing surface is never dereferenced.
        ReleaseInterface( m_pBackBuffer );
        return false;
    }

    NoteRequestedVSync( bVSync );

    if ( m_bShared )
    {
        m_pContext->CopyResource( m_pShared, m_pBackBuffer );
        // The 9 side reads the same memory and the copy is only queued, so flush before drawing.
        m_pContext->Flush();
        m_bTextureHasFrame = true;
    }
    else
    {
        // A failed copy leaves the previous frame in the texture, and presenting it still runs MTA's present
        // handler, which advances the client's frame, so one frame behind beats stalling it. A fresh texture goes to the chain.
        if ( !UploadReadback() && !m_bTextureHasFrame )
        {
            ReleaseInterface( m_pBackBuffer );
            return false;
        }
    }

    const bool bPresented = DrawAndPresent();

    ReleaseInterface( m_pBackBuffer );
    return bPresented;
}

void CMtaBridge::ReportFrameCopyFailure( unsigned int uiReason, const char* pReason )
{
    // A stale frame looks like a frozen one, so each reason is reported once, not every frame.
    if ( ( m_uiFrameCopyFailuresReported & uiReason ) != 0 )
        return;

    m_uiFrameCopyFailuresReported |= uiReason;
    g_pDebug->printError( string( "MTA bridge: " ) + pReason + ", presenting the previous frame instead" );
}

bool CMtaBridge::UploadReadback()
{
    m_pContext->CopyResource( m_pStaging, m_pBackBuffer );

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if ( FAILED( m_pContext->Map( m_pStaging, 0, D3D11_MAP_READ, 0, &mapped ) ) || mapped.pData == nullptr )
    {
        ReportFrameCopyFailure( FRAME_COPY_MAP_FAILED, "the frame copy target could not be read back" );
        return false;
    }

    bool bCopied = false;
    D3DLOCKED_RECT locked{};
    // Locked and unlocked exactly once, else a lock that succeeds with no bits would stay held and block every later lock.
    const HRESULT hrLock = m_pTexture9->LockRect( 0, &locked, nullptr, 0 );
    const bool bLocked = SUCCEEDED( hrLock );
    if ( bLocked && locked.pBits != nullptr )
    {
        const unsigned char* pSource = static_cast<const unsigned char*>( mapped.pData );
        unsigned char* pDestination = static_cast<unsigned char*>( locked.pBits );
        // Both sides were made at the frame size, so the walk is bounded by that, not the window.
        const size_t rowBytes = static_cast<size_t>( m_uiSourceWidth ) * 4u;

        // Both stores write a whole row, so it has to fit each pitch. The locked pitch is signed and is checked
        // before widening, else it would become a huge row limit and a backwards offset. The source uses the mapped pitch.
        if ( locked.Pitch > 0 && rowBytes <= static_cast<size_t>( locked.Pitch ) && rowBytes <= static_cast<size_t>( mapped.RowPitch ) )
        {
            for ( unsigned int y = 0; y < m_uiSourceHeight; ++y )
            {
                const unsigned char* pSourceRow = pSource + static_cast<size_t>( y ) * mapped.RowPitch;
                unsigned char* pDestinationRow = pDestination + static_cast<size_t>( y ) * locked.Pitch;

                if ( m_bSwizzle )
                {
                    for ( unsigned int x = 0; x < m_uiSourceWidth; ++x )
                    {
                        const size_t offset = static_cast<size_t>( x ) * 4u;
                        pDestinationRow[offset + 0] = pSourceRow[offset + 2];
                        pDestinationRow[offset + 1] = pSourceRow[offset + 1];
                        pDestinationRow[offset + 2] = pSourceRow[offset + 0];
                        pDestinationRow[offset + 3] = pSourceRow[offset + 3];
                    }
                }
                else
                {
                    memcpy( pDestinationRow, pSourceRow, rowBytes );
                }
            }

            bCopied = true;
            m_bTextureHasFrame = true;
            // Re-armed on success, else one transient failure silences that reason for good.
            m_uiFrameCopyFailuresReported = 0;
        }
        else
        {
            // Names the condition, because either pitch can produce it.
            ReportFrameCopyFailure( FRAME_COPY_PITCH_TOO_NARROW, "the frame row is wider than a row pitch" );
        }
    }
    else
    {
        ReportFrameCopyFailure( FRAME_COPY_LOCK_FAILED, "the interface texture could not be locked" );
    }

    if ( bLocked )
        m_pTexture9->UnlockRect( 0 );

    m_pContext->Unmap( m_pStaging, 0 );

    // Reported above, and the caller presents the frame already in the texture.
    return bCopied;
}

bool CMtaBridge::DrawAndPresent()
{
    // Bound here rather than assumed, since the interface code inside present can leave another target behind.
    IDirect3DSurface9* pBackBuffer = nullptr;
    if ( FAILED( m_pDevice9->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer ) ) || pBackBuffer == nullptr )
    {
        // Cleared here too, and the release costs nothing if the call already cleared it.
        ReleaseInterface( pBackBuffer );

        // Lost device or a chain being rebuilt, told apart by the cooperative level, and only the lost case is reported.
        const HRESULT hrCooperativeBuffer = m_pDevice9->TestCooperativeLevel();
        if ( IsDeviceUnavailable( hrCooperativeBuffer ) )
            m_bDeviceLost = true;
        return false;
    }

    HRESULT hrBind = m_pDevice9->SetRenderTarget( 0, pBackBuffer );
    if ( SUCCEEDED( hrBind ) )
        hrBind = m_pDevice9->SetDepthStencilSurface( nullptr );
    pBackBuffer->Release();
    if ( FAILED( hrBind ) )
    {
        const HRESULT hrCooperativeBind = m_pDevice9->TestCooperativeLevel();
        if ( IsDeviceUnavailable( hrCooperativeBind ) )
            m_bDeviceLost = true;
        return false;
    }

    // Screen space over the whole back buffer, and a leftover viewport would clip the quad.
    D3DVIEWPORT9 viewport{};
    viewport.Width = m_uiSurfaceWidth;
    viewport.Height = m_uiSurfaceHeight;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    m_pDevice9->SetViewport( &viewport );

    // Only a lost device when the cooperative level says so. Any other refusal means a scene is already
    // open, which MTA reports as expected. Nothing is erased yet, so the buffer still holds a frame.
    const HRESULT hrBeginScene = m_pDevice9->BeginScene();
    if ( FAILED( hrBeginScene ) )
    {
        const HRESULT hrCooperative = m_pDevice9->TestCooperativeLevel();
        if ( IsDeviceUnavailable( hrCooperative ) )
        {
            m_bDeviceLost = true;
            return false;
        }

        return SUCCEEDED( m_pDevice9->Present( nullptr, nullptr, nullptr, nullptr ) );
    }

    // The device is shared with MTA's interface code, so nothing here is assumed about state.
    m_pDevice9->SetVertexShader( nullptr );
    m_pDevice9->SetPixelShader( nullptr );
    m_pDevice9->SetFVF( SCREEN_VERTEX_FVF );
    m_pDevice9->SetRenderState( D3DRS_LIGHTING, FALSE );
    m_pDevice9->SetRenderState( D3DRS_ZENABLE, FALSE );
    m_pDevice9->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    m_pDevice9->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
    m_pDevice9->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
    // What the interface draws inside present leaves behind is inherited here, so the states that would move the quad are set.
    m_pDevice9->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA );
    m_pDevice9->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
    m_pDevice9->SetRenderState( D3DRS_STENCILENABLE, FALSE );
    // The material drawing turns fog on and its restore doesnt put it back, so it is off here.
    m_pDevice9->SetRenderState( D3DRS_FOGENABLE, FALSE );
    m_pDevice9->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 );
    m_pDevice9->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
    m_pDevice9->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE );
    m_pDevice9->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
    m_pDevice9->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
    m_pDevice9->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );

    // Cleared after the scene opens and the scissor is off, else it would erase the frame and get clipped.
    if ( FAILED( m_pDevice9->Clear( 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB( 0, 0, 0 ), 1.0f, 0 ) ) )
    {
        // The scene opened above is closed here too, so no arm leaves one open.
        m_pDevice9->EndScene();
        const HRESULT hrCooperativeClear = m_pDevice9->TestCooperativeLevel();
        if ( IsDeviceUnavailable( hrCooperativeClear ) )
            m_bDeviceLost = true;
        return false;
    }

    // The client decodes sRGB on this device's samplers when its colour correction is on, so the frame
    // would be decoded twice. Saved and restored around the draw; an unreadable value is left alone.
    DWORD previousSrgbTexture = 0;
    const bool bSrgbTextureRestorable = SUCCEEDED( m_pDevice9->GetSamplerState( 0, D3DSAMP_SRGBTEXTURE, &previousSrgbTexture ) );
    if ( bSrgbTextureRestorable )
        m_pDevice9->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, FALSE );

    // The same colour correction turns on the sRGB write when it is on, and that pair is one setting: the
    // read side decodes on the way in and the write side encodes on the way out. Clearing only the read
    // side would leave the frame encoded a second time on its way into the back buffer, which no longer
    // matches what the engine produced, so the write side is saved and cleared with it and put back after
    // the draw. This device is shared with the interface code, so the previous value is read rather than
    // assumed, the same way the sampler above is handled.
    DWORD previousSrgbWrite = FALSE;
    const bool bSrgbWriteRestorable = SUCCEEDED( m_pDevice9->GetRenderState( D3DRS_SRGBWRITEENABLE, &previousSrgbWrite ) );
    if ( bSrgbWriteRestorable )
        m_pDevice9->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );

    m_pDevice9->SetTexture( 0, m_pTexture9 );

    // Half pixel offset lands on pixel centres, and the quad covers the frame size unscaled.
    const float width = static_cast<float>( m_uiSourceWidth );
    const float height = static_cast<float>( m_uiSourceHeight );
    const SScreenVertex quad[4] = {
        { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
        { width - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
        { -0.5f, height - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
        { width - 0.5f, height - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
    };
    m_pDevice9->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, quad, sizeof( SScreenVertex ) );

    m_pDevice9->SetTexture( 0, nullptr );
    if ( bSrgbTextureRestorable )
        m_pDevice9->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, previousSrgbTexture );
    if ( bSrgbWriteRestorable )
        m_pDevice9->SetRenderState( D3DRS_SRGBWRITEENABLE, previousSrgbWrite );
    m_pDevice9->EndScene();

    // Present runs MTA's handler, which raises the post frame pulse and draws the interface over the image.
    const HRESULT hrPresent = m_pDevice9->Present( nullptr, nullptr, nullptr, nullptr );
    if ( IsDeviceUnavailable( hrPresent ) )
    {
        m_bDeviceLost = true;
        return false;
    }

    return SUCCEEDED( hrPresent );
}

void CMtaBridge::ReleaseTargets()
{
    ReleaseInterface( m_pTexture9 );
    ReleaseInterface( m_pShared );
    ReleaseInterface( m_pStaging );
    ReleaseInterface( m_pBackBuffer );
    m_TextureFormat = D3DFMT_UNKNOWN;
    m_bSwizzle = false;
    m_bShared = false;
    m_bTextureHasFrame = false;
    m_uiFrameCopyFailuresReported = 0;
}
