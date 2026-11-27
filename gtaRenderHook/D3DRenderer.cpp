// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "D3DRenderer.h"
#include "D3D1XDepthStencilTexture.h"
#include "D3D1XBaseTexture.h"
#include "D3D1X2DRenderTarget.h"
#include <directxcolors.h>
#include "D3D1XStateManager.h"
#include "D3DSpecificHelpers.h"
#include "CDebug.h"
#include "RwD3D1XEngine.h"
#include "MtaBridge.h"

CD3DRenderer::CD3DRenderer( HWND& window )
{
    SetProcessDPIAware();
    m_hWnd = window;
    // dxgi factory initialization
    if ( !CALL_D3D_API( CreateDXGIFactory( __uuidof( IDXGIFactory ), reinterpret_cast<void**>( &m_pdxgiFactory ) ),
                        "Failed to create DXGI Factory." ) ) return;

    IDXGIAdapter * pAdapter = nullptr;
    // Init adapter list. EnumAdapters only writes the output on success, so the enumeration ends
    // at the first failure and only adapters that were actually handed over are kept.
    if ( !gDebugSettings.GetToggleField( "UseDefaultAdapter" ) )
    {
        for ( UINT i = 0; SUCCEEDED( m_pdxgiFactory->EnumAdapters( i, &pAdapter ) ) && pAdapter != nullptr; ++i )
            m_vAdapters.push_back( pAdapter );
    }
    else
    {
        if ( SUCCEEDED( m_pdxgiFactory->EnumAdapters( 0, &pAdapter ) ) && pAdapter != nullptr )
            m_vAdapters.push_back( pAdapter );
    }

    // This index is read before InitDevice can check it, so the adapter list is validated here:
    // a list that enumerated nothing has to stop here instead of being dereferenced.
    if ( m_vAdapters.empty() || m_uiCurrentAdapter >= m_vAdapters.size() )
    {
        // Formatted into a fixed buffer because this reports a failed enumeration out of a constructor,
        // so building a string here would allocate on the way out of the failure it reports.
        char message[160];
        snprintf( message, sizeof( message ),
                  "Display mode enumeration skipped, %zu adapter(s) were enumerated for adapter index %u.",
                  m_vAdapters.size(), m_uiCurrentAdapter );
        g_pDebug->printError( message );
        return;
    }

    // Cleared before the call that fills it in, because the wrapper below only guarantees the pointer on success.
    IDXGIOutput *pOutput = nullptr;
    if ( !CALL_D3D_API( m_vAdapters[m_uiCurrentAdapter]->EnumOutputs( 0, &pOutput ),
                        "Failed to enumerate adapter output #0." ) ) return;

    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;

    // retrieve avaliable display mode count.
    UINT modeCount = 0;
    if ( !CALL_D3D_API( pOutput->GetDisplayModeList( format, 0, &modeCount, nullptr ),
                        "Failed to get display mode count." ) )
    {
        pOutput->Release();
        return;
    }

    std::vector<DXGI_MODE_DESC> modeDescriptions{ modeCount };
    // get display mode list
    if ( !CALL_D3D_API( pOutput->GetDisplayModeList( format, 0, &modeCount, modeDescriptions.data() ),
                        "Failed to retrieve display mode list." ) )
    {
        pOutput->Release();
        return;
    }

    // populate adapter mode list, if mode description has refresh rate less than 60 Hz
    // TODO: perhaps max refresh rate should be choosen seperatly, maybe from settings file
    // GetDisplayModeList reports how many entries it wrote, which can be fewer than the list was sized for; the leftover
    // entries stay zeroed and must not become display modes.
    for ( UINT i = 0; i < modeCount && i < modeDescriptions.size(); i++ )
    {
        // Some outputs report a zero refresh-rate denominator; keep those entries instead of
        // dividing by zero.
        if ( modeDescriptions[i].RefreshRate.Denominator == 0 ||
             modeDescriptions[i].RefreshRate.Numerator / modeDescriptions[i].RefreshRate.Denominator <= 60 )
            m_vAdapterModes.push_back( modeDescriptions[i] );
    }

    pOutput->Release();
}

CD3DRenderer::~CD3DRenderer()
{
    // Safety net for a renderer destroyed without going through DeInitDevice: Close can run before Stop, so without this
    // the device, the context and both swap chain references outlive the object that owned them. DeInitDevice clears every
    // member it releases, so in the usual Stop then Close order there is nothing left for it to do here.
    DeInitDevice();

    for ( auto pAdapter : m_vAdapters )
        pAdapter->Release();
    if ( m_pdxgiFactory )
    {
        m_pdxgiFactory->Release();
        m_pdxgiFactory = nullptr;
    }
}

bool CD3DRenderer::InitDevice()
{
    HRESULT hr = S_OK;
    // return if something is wrong, e.g. we have set some wrong parameters.
    // TODO: try to avoid these errors, for example reset current adapter mode to 0 or something like that, but perhaps this is just a garbage thought
    // A single available display mode is perfectly usable, so only an empty list is fatal, and
    // the index test has to reject the value equal to the size rather than one past it.
    if ( m_vAdapterModes.empty() ||
         m_uiCurrentAdapterMode >= m_vAdapterModes.size() ||
         m_uiCurrentAdapter >= m_vAdapters.size() )
    {
        // Same reason as the enumeration report above: this is the device-failure path.
        char message[224];
        snprintf( message, sizeof( message ),
                  "Device initialization skipped, %zu adapter(s) and %zu display mode(s) are available for adapter %u mode %u.",
                  m_vAdapters.size(), m_vAdapterModes.size(), m_uiCurrentAdapter, m_uiCurrentAdapterMode );
        g_pDebug->printError( message );
        return false;
    }

    auto currentAdapterMode = m_vAdapterModes[m_uiCurrentAdapterMode];
    auto currentAdapter = m_vAdapters[m_uiCurrentAdapter];
    bool isWindowed = gDebugSettings.GetToggleField( "Windowed" );
    g_pDebug->printMsg( "Device initialization: start", 1 );
    // resize and reposition rendering window if required
    // TODO: add ability to choose wether to center it if we are in windowed mode
    // GetClientRect reports whether it wrote the rectangle, and the members keep their cleared value when it didnt, which the
    // comparison below reads as a mismatch. The render engine reads both back through getWidth and getHeight.
    RECT rc{};
    if ( GetClientRect( m_hWnd, &rc ) != 0 )
    {
        m_uiWidth = static_cast<UINT>( rc.right - rc.left );
        m_uiHeight = static_cast<UINT>( rc.bottom - rc.top );
    }
    if ( isWindowed || m_uiWidth != currentAdapterMode.Width || m_uiHeight != currentAdapterMode.Height )
    {
        m_uiWidth = currentAdapterMode.Width;
        m_uiHeight = currentAdapterMode.Height;
        if ( isWindowed )
        {
            SetWindowLongPtr( m_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE );
            SetWindowLongPtr( m_hWnd, GWL_EXSTYLE, 0 );
        }
        SetWindowPos( m_hWnd, nullptr, 0, 0, currentAdapterMode.Width, currentAdapterMode.Height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
    }
    // initialize device creation flags
    // TODO: add ability to set some custom flags
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    //createDeviceFlags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    // initialize driver types and feature levels
    // TODO: move them to header, or store them in memory, perhaps
    D3D_DRIVER_TYPE driverTypes[] =
    {
        D3D_DRIVER_TYPE_UNKNOWN,
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };

    UINT numDriverTypes = ARRAYSIZE( driverTypes );

    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };

    UINT numFeatureLevels = ARRAYSIZE( featureLevels );


    // Staged into locals for both retry loops and handed to the members only once a device exists. The create call states
    // what its outputs hold on success and nothing about failure, while DeInitDevice releases whatever the members hold, so a
    // failed iteration that left a non device in a member would be released as one. Each iteration clears the locals first.
    ID3D11Device *pCreatedDevice = nullptr;
    ID3D11DeviceContext *pCreatedContext = nullptr;
    D3D_FEATURE_LEVEL createdFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    // loop over each driver type to find supported one
    for ( UINT driverTypeIndex = 0; driverTypeIndex < numDriverTypes; driverTypeIndex++ )
    {
        m_driverType = driverTypes[driverTypeIndex];
        // The adapter is paired with UNKNOWN alone: the only driver type the create call accepts alongside a non null
        // adapter, and any other type with one is refused, which covers the two software types below. HARDWARE still passes
        // no adapter, which isnt the same as passing currentAdapter: the call picks the default one.
        IDXGIAdapter *pSelectedAdapter = m_driverType == D3D_DRIVER_TYPE_UNKNOWN ? currentAdapter : nullptr;
        pCreatedDevice = nullptr;
        pCreatedContext = nullptr;
        hr = D3D11CreateDevice( pSelectedAdapter, m_driverType, nullptr, createDeviceFlags, featureLevels, numFeatureLevels,
                                D3D11_SDK_VERSION, &pCreatedDevice, &createdFeatureLevel, &pCreatedContext );
        // if we failed to create device with invalid arg error,
        // probably it's because current feature level is not supported by GPU,
        // so we will try to find supported feature level set
        if ( hr == E_INVALIDARG )
        {

            // try to find supported feature level by iterating over whole set and removing one feature level after another until success
            // Each attempt asks for every level from i to the end, so the bound stops one short: the final attempt starts at
            // the second to last level and already covers the last one.
            for ( UINT i = 1; i < numFeatureLevels - 1; i++ )
            {
                pCreatedDevice = nullptr;
                pCreatedContext = nullptr;
                hr = D3D11CreateDevice( pSelectedAdapter, m_driverType, nullptr, createDeviceFlags, &featureLevels[i], numFeatureLevels - i,
                                        D3D11_SDK_VERSION, &pCreatedDevice, &createdFeatureLevel, &pCreatedContext );
                if ( SUCCEEDED( hr ) )
                    break;
            }
        }

        if ( SUCCEEDED( hr ) )
            break;
    }

    if ( FAILED( hr ) )
    {
        g_pDebug->printError( "Failed to create device, invalid input arguments" );
        return false;
    }

    // The members are assigned here and nowhere earlier, so every failure path above leaves them as they were.
    m_pd3dDevice = pCreatedDevice;
    m_pImmediateContext = pCreatedContext;
    m_featureLevel = createdFeatureLevel;

    g_pDebug->printMsg( "D3D11 Device created", 1 );
    // Query debug interface
    m_pd3dDevice->QueryInterface( __uuidof( ID3D11Debug ), (void**)&m_pDebug );

    // Create swap chain for device.
    // Held by a smart pointer rather than released by hand below, because both calls in that block throw instead of
    // returning a failure value: a throw skips every statement after it, so a release written last never runs.
    CComPtr<IDXGIFactory2> dxgiFactory2;
    m_pdxgiFactory->QueryInterface( __uuidof( IDXGIFactory2 ), reinterpret_cast<void**>( &dxgiFactory2 ) );
    if ( dxgiFactory2 != nullptr )
    {
        // DirectX 11.1 or later
        m_pImmediateContext->QueryInterface( __uuidof( ID3DUserDefinedAnnotation ), reinterpret_cast<void**>( &m_pRenderingAnnotation ) );
        // Initialize swap chain parameters
        // TODO: add ability to set flags and some params in settings file
        DXGI_SWAP_CHAIN_DESC1 sd;
        ZeroMemory( &sd, sizeof( sd ) );
        sd.Width = currentAdapterMode.Width;
        sd.Height = currentAdapterMode.Height;
        sd.Format = currentAdapterMode.Format;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.Flags = isWindowed ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0;
        sd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsd;
        ZeroMemory( &fsd, sizeof( fsd ) );
        fsd.Scaling = currentAdapterMode.Scaling;
        fsd.ScanlineOrdering = currentAdapterMode.ScanlineOrdering;
        fsd.Windowed = isWindowed;
        fsd.RefreshRate = currentAdapterMode.RefreshRate;

        CALL_D3D_API( dxgiFactory2->CreateSwapChainForHwnd( m_pd3dDevice, m_hWnd, &sd, &fsd, nullptr, &m_pSwapChain1 ),
                      "Failed to create swap chain using DX11.1 API." );
        // Second reference to the chain above, typed as the base interface the rest of the engine keeps. DeInitDevice releases
        // both and nulls each, which is what keeps them balanced. A test wedged around these calls would be a constant true,
        // because CALL_D3D_API returns only after it has succeeded.
        CALL_D3D_API( m_pSwapChain1->QueryInterface( __uuidof( IDXGISwapChain1 ),
                                                    reinterpret_cast<void**>( &m_pSwapChain ) ),
                      "Failed to query the created swap chain for IDXGISwapChain1." );
    }
    else
    {
        // DirectX 11.0 systems
        DXGI_SWAP_CHAIN_DESC sd;
        ZeroMemory( &sd, sizeof( sd ) );
        sd.BufferCount = 1;
        sd.BufferDesc.Width = currentAdapterMode.Width;
        sd.BufferDesc.Height = currentAdapterMode.Height;
        sd.BufferDesc.Format = currentAdapterMode.Format;
        sd.BufferDesc.RefreshRate = currentAdapterMode.RefreshRate;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = m_hWnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = isWindowed;
        sd.Flags = isWindowed ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0;
        CALL_D3D_API( m_pdxgiFactory->CreateSwapChain( m_pd3dDevice, &sd, &m_pSwapChain ), "Failed to create swap chain using DX11 API" );
    }

    // MTA hooks the d3d9.dll factory export, so this device is the one it attaches to and flips the
    // window with. If creation fails the chain presents on its own, so the frame still gets there.
    if ( m_pMtaBridge )
    {
        m_pMtaBridge->Shutdown();
        delete m_pMtaBridge;
        m_pMtaBridge = nullptr;
    }

    // Kept even when it couldnt present, since it may already hold the 9 device MTA is using.
    m_pMtaBridge = new CMtaBridge();
    m_pMtaBridge->Init( m_pd3dDevice, m_pImmediateContext, m_pSwapChain, m_hWnd );

    g_pDebug->printMsg( "Device initialization: end", 1 );

    return true;
}

void CD3DRenderer::DeInitDevice()
{
    // Torn down before the objects it was built from are released.
    if ( m_pMtaBridge )
    {
        m_pMtaBridge->Shutdown();
        delete m_pMtaBridge;
        m_pMtaBridge = nullptr;
    }

    if ( m_pImmediateContext )
    {
        m_pImmediateContext->ClearState();
        m_pImmediateContext->Flush();
    }

    // The DX11.1 path holds two references to the same swap chain, because the created chain is
    // queried again straight after creation, so both references have to be released here.
    if ( m_pSwapChain1 )
    {
        m_pSwapChain1->Release();
        m_pSwapChain1 = nullptr;
    }
    if ( m_pSwapChain )
    {
        m_pSwapChain->Release();
        m_pSwapChain = nullptr;
    }
    if ( m_pImmediateContext )
    {
        m_pImmediateContext->Release();
        m_pImmediateContext = nullptr;
    }
    if ( m_pd3dDevice )
    {
        m_pd3dDevice->Release();
        m_pd3dDevice = nullptr;
    }
    if ( m_pRenderingAnnotation )
    {
        m_pRenderingAnnotation->Release();
        m_pRenderingAnnotation = nullptr;
    }
    if ( m_pDebug )
    {
        m_pDebug->ReportLiveDeviceObjects( D3D11_RLDO_DETAIL );
        m_pDebug->Release();
        m_pDebug = nullptr;
    }
}

void CD3DRenderer::BeginUpdate( RwCamera *camera )
{
    RwD3D1XRaster* pd3dDepthRaster = GetD3D1XRaster( camera->zBuffer );
    RwD3D1XRaster* pd3dRTRaster = GetD3D1XRaster( camera->frameBuffer );

    // set render targets and depth buffers for camera
    // texture flags check is used if you want to use UAV for rendering
    // TODO: remove UAV set for camera, and make some separate functions for that
    if ( camera->frameBuffer )
    {
        // Set render targets
        auto rtTex = dynamic_cast<ID3D1XRenderTargetViewable*>( pd3dRTRaster->resourse );
        auto rtrv = rtTex ? rtTex->GetRenderTargetView() : nullptr;
        if ( camera->zBuffer != nullptr )
        {
            auto depthTex = dynamic_cast<CD3D1XDepthStencilTexture*>( pd3dDepthRaster->resourse );
            g_pStateMgr->SetRenderTargets( 1, &rtrv, depthTex ? depthTex->GetDepthStencilView() : nullptr );
        }
        else
            g_pStateMgr->SetRenderTargets( 1, &rtrv, nullptr );
    }
    /*else if(camera->frameBuffer&&pd3dRTRaster->textureFlags == 64) {
        // TODO: remove com pointers, or left as is, don't know if that will affect anything
        CComPtr<ID3D11RenderTargetView> renderTargets;
        CComPtr<ID3D11DepthStencilView> depthStencil;
        m_pImmediateContext->OMGetRenderTargets(1, &renderTargets, &depthStencil);

        auto uav = pd3dRTRaster->resourse->GetUAV();
        m_pImmediateContext->OMSetRenderTargetsAndUnorderedAccessViews(1, &renderTargets.p, nullptr, 2, 1, &uav, nullptr);
    }*/
    else
    {
        if ( camera->zBuffer != nullptr )
        {
            auto depthTex = dynamic_cast<CD3D1XDepthStencilTexture*>( pd3dDepthRaster->resourse );
            g_pStateMgr->SetRenderTargets( 0, nullptr, depthTex ? depthTex->GetDepthStencilView() : nullptr );
        }
    }
    // Setup the viewport
    if ( camera->frameBuffer )
    {
        D3D11_VIEWPORT vp;
        vp.Width = (FLOAT)camera->frameBuffer->width;
        vp.Height = (FLOAT)camera->frameBuffer->height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        g_pStateMgr->SetViewport( vp );
    }
    g_pStateMgr->FlushRenderTargets();
}

void CD3DRenderer::EndUpdate( RwCamera *camera )
{
    // remove UAV from camera, will remove this later
    RwD3D1XRaster* pd3dRTRaster = GetD3D1XRaster( camera->frameBuffer );
    if ( camera->frameBuffer && pd3dRTRaster->textureFlags == 64 )
    {
        ID3D11UnorderedAccessView* uavs = nullptr;
        m_pImmediateContext->OMSetRenderTargetsAndUnorderedAccessViews( 0, { nullptr }, nullptr, 2, 1, &uavs, nullptr );
        // That call leaves the device with no colour target and no depth view bound, so the state manager is told the same
        // thing. Leaving its cache on the camera's target would let a later SetRenderTargets for those same pointers compare
        // equal, raise no update, and draw into whatever the device really holds.
        g_pStateMgr->SetRenderTargets( 0, nullptr, nullptr );
    }
}

void CD3DRenderer::Clear( RwCamera *camera, RwRGBA& color, RwInt32 flags )
{
    RwD3D1XRaster* pd3dDepthRaster = GetD3D1XRaster( camera->zBuffer );
    RwD3D1XRaster* pd3dRTRaster = GetD3D1XRaster( camera->frameBuffer );

    if ( flags&rwCAMERACLEARIMAGE && camera->frameBuffer )
    {
        auto rtTex = dynamic_cast<ID3D1XRenderTargetViewable*>( pd3dRTRaster->resourse );
        ID3D11RenderTargetView* rtv = rtTex ? rtTex->GetRenderTargetView() : nullptr;
        if ( rtv )
        {
            float clearColor[] = { color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f, color.alpha / 255.0f };
            m_pImmediateContext->ClearRenderTargetView( rtv, clearColor );
        }
    }
    if ( ( flags&rwCAMERACLEARZ || flags & rwCAMERACLEARSTENCIL ) && camera->zBuffer )
    {
        auto depthTex = dynamic_cast<CD3D1XDepthStencilTexture*>( pd3dDepthRaster->resourse );
        // Clear to far-plane depth (0 for reversed Z, 1 for standard depth).
        const float farDepth = CRwD3D1XEngine::IsReversedZCamera( camera ) ? 0.0f : 1.0f;
        ID3D11DepthStencilView* dsv = depthTex ? depthTex->GetDepthStencilView() : nullptr;
        if ( dsv )
        {
            if ( !( flags&rwCAMERACLEARZ ) )
                m_pImmediateContext->ClearDepthStencilView( dsv, D3D11_CLEAR_STENCIL, farDepth, 0 );
            else if ( !( flags&rwCAMERACLEARSTENCIL ) )
                m_pImmediateContext->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, farDepth, 0 );
            else
                m_pImmediateContext->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, farDepth, 0 );
        }
    }
}

void CD3DRenderer::Present( bool VSync )
{
    // Ahead of the bridge check, so a frame the bridge composites is still a frame whose fixed sites were checked.
    // Installing a value proves only what the site held at install time: another module patches in levels and can rewrite
    // a site afterwards. The idle entry runs the same check before it touches the world, and this is here too because a
    // frame can reach this present without passing one; both call one function, which also runs the one site audit a run.
    RevalidateOwnedSites();

    // Once MTA's device exists it owns the frame: copy, present, then its interface draw.
    if ( m_pMtaBridge != nullptr && m_pMtaBridge->IsActive() )
    {
        if ( m_pMtaBridge->Composite( VSync ) )
            return;
    }

    // The chain is null between DeInitDevice releasing it and the next device being opened, and it is also null when Start
    // stopped after InitDevice returned false, which is before any chain was created. Both are lifetimes this plugin drives
    // rather than a fault, so the flip is skipped the way a successful composite skips it above.
    if ( m_pSwapChain == nullptr )
        return;

    m_pSwapChain->Present( VSync ? 1 : 0, 0 );
}

void CD3DRenderer::DrawIndexed( UINT indexCount, UINT startIndex, UINT baseVertex )
{
    m_pImmediateContext->DrawIndexed( indexCount, startIndex, baseVertex );
}

unsigned int CD3DRenderer::getAvaliableTextureMemory()
{
    if ( m_uiCurrentAdapter >= m_vAdapters.size() )
        return 0;
    DXGI_ADAPTER_DESC desc;
    // Untested on purpose. CALL_D3D_API returns only after the call has succeeded and throws otherwise, so a test around
    // this line would be a constant false. The return 0 above answers a different question and stays: an adapter index this
    // renderer holds no adapter for is a state that does happen.
    CALL_D3D_API( m_vAdapters[m_uiCurrentAdapter]->GetDesc( &desc ), "Failed to retrieve adapter description." );
    return desc.DedicatedVideoMemory;
}

const char* CD3DRenderer::getAdapterInfo( UINT n )
{
    if ( n >= getAdapterCount() )
        return "null";
    char* info = m_sAdapterInfo;
    DXGI_ADAPTER_DESC desc;

    // wcstombs reports failure when the locale cannot represent a character and stores nothing for an empty description, so
    // either case has to report no information. The buffer is a member, so it is cleared here as well: wcstombs writes only
    // what it converts and never a terminator, so a shorter description would keep the previous adapter's tail.
    memset( info, 0, sizeof( m_sAdapterInfo ) );
    size_t converted = static_cast<size_t>( -1 );
    // Untested for the reason the texture memory accessor gives: this helper throws rather than returning a failure value, so
    // a test here would be a constant true. The fallback below covers the wcstombs outcomes, which is all it ever covered.
    CALL_D3D_API( m_vAdapters[n]->GetDesc( &desc ), "Failed to retrieve adapter description." );
    converted = wcstombs( info, desc.Description, sizeof( m_sAdapterInfo ) - 1 );
    if ( converted == static_cast<size_t>( -1 ) || converted == 0 )
    {
        memset( info, 0, sizeof( m_sAdapterInfo ) );
        wcstombs( info, L"no info", sizeof( m_sAdapterInfo ) - 1 );
    }

    return info;
}
