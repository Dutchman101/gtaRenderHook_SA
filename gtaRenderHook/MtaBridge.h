#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGISwapChain;
struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DTexture9;

// Presents the Direct3D 11 frame through the MTA client's Direct3D 9 device.
//
// Creating our device through the d3d9.dll Direct3DCreate9 export returns MTA's factory, because MTA
// replaces that export, so the device reaches MTA's begin, end and present handlers and MTA draws its
// interface inside present. The shared surface route is tried first since it copies nothing; it needs
// 9Ex, so when the create refuses the frame is copied through system memory instead.
class CMtaBridge
{
public:
    CMtaBridge() = default;
    ~CMtaBridge();

    CMtaBridge( const CMtaBridge& ) = delete;
    CMtaBridge& operator=( const CMtaBridge& ) = delete;

    // Must run on the window thread: 9 only allows creating and resetting a device there.
    bool Init( ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain, HWND hWnd );

    // Call while the 11 objects handed to Init are alive, since the surfaces reference them.
    void Shutdown();

    bool IsActive() const { return m_bActive; }

    // Copies the frame just rendered and presents it, which also runs MTA's post frame pulse and its
    // interface draw. Returns false when nothing was presented and the caller must present the chain.
    bool Composite( bool bVSync );

private:
    bool CreateD3D9Device();
    bool CreateTargets();
    bool AcquireBackBuffer();
    bool CreateSharedTarget();
    bool CreateReadbackTarget();
    bool ResetD3D9();
    bool RecoverDevice();
    // One bit per reason the frame copy can fail, so each reason reaches the log once.
    enum FrameCopyFailure : unsigned int
    {
        FRAME_COPY_MAP_FAILED = 1u << 0,
        FRAME_COPY_LOCK_FAILED = 1u << 1,
        FRAME_COPY_PITCH_TOO_NARROW = 1u << 2
    };

    void NoteRequestedVSync( bool bVSync );
    void FillPresentParameters( D3DPRESENT_PARAMETERS& parameters ) const;
    void ReportFrameCopyFailure( unsigned int uiReason, const char* pReason );
    bool UploadReadback();
    bool DrawAndPresent();
    void ReleaseTargets();

    ID3D11Device*         m_pDevice = nullptr;
    ID3D11DeviceContext*  m_pContext = nullptr;
    IDXGISwapChain*       m_pSwapChain = nullptr;

    // The buffer the frame was rendered into. Kept as a texture because the chain is only read now.
    ID3D11Texture2D*      m_pBackBuffer = nullptr;
    // System memory copy of the frame, used by the copy route.
    ID3D11Texture2D*      m_pStaging = nullptr;
    // Texture whose memory is also opened on the 9 device, used by the shared surface route.
    ID3D11Texture2D*      m_pShared = nullptr;

    IDirect3D9*           m_pD3D9 = nullptr;
    IDirect3DDevice9*     m_pDevice9 = nullptr;
    IDirect3DTexture9*    m_pTexture9 = nullptr;

    HWND                  m_hWnd = nullptr;
    // Swap chain buffer size. Device and surfaces stay at it, since a reset would release MTA's default pool resources.
    unsigned int          m_uiSourceWidth = 0;
    unsigned int          m_uiSourceHeight = 0;
    // Size the surfaces were built at, checked against the frame size every frame.
    unsigned int          m_uiSurfaceWidth = 0;
    unsigned int          m_uiSurfaceHeight = 0;
    D3DFORMAT             m_TextureFormat = D3DFMT_UNKNOWN;

    bool                  m_bActive = false;
    bool                  m_bShared = false;
    bool                  m_bDeviceLost = false;
    // Set once the texture holds a frame, since a fresh texture holds no defined content.
    bool                  m_bTextureHasFrame = false;
    // Set when the copy has to swap red and blue, R8G8B8A8 frame into an A8R8G8B8 texture.
    bool                  m_bSwizzle = false;
    bool                  m_bVSync = true;
    bool                  m_bIntervalMismatchReported = false;
    bool                  m_bSizeMismatchReported = false;
    // Set once a reset failed for something other than loss, so it is reported once.
    bool                  m_bResetFailureReported = false;
    // Set once the chain buffer failed to open, so a chain that stays unreadable is reported once.
    bool                  m_bBackBufferFailureReported = false;
    // Set once the chain buffer reported no size, which a minimized window does on every frame.
    bool                  m_bBackBufferSizeReported = false;
    // Reasons the frame copy already reported, so a persistent failure doesnt fill the log.
    unsigned int          m_uiFrameCopyFailuresReported = 0;
};
