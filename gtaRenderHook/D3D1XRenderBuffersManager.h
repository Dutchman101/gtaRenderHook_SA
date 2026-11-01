#pragma once
#include "D3D1XConstantBuffer.h"
#include <unordered_map>
/*!
    Matrix buffer structure, represents common transformation matrices used for rendering.
*/
struct PerFrameMatrixBuffer
{
    RwMatrix mView;
    RwMatrix mProjection;
    RwMatrix mViewProjection;
    RwMatrix mInvView;
    RwMatrix mInvViewProj;
    // Object motion state for the main camera: unjittered previous view-projection, current NDC jitter (xy),
    // z = 1 when motion is valid, w = 1 when an external TAA handled the previous frame (disabling dither patterns).
    RwMatrix mPrevViewProj;
    float    vMotionJitter[4];
};
struct PerObjectMatrixBuffer
{
    RwMatrix mWorld;
    RwMatrix mWorldInv;
    RwMatrix mWorldPrev;
};
/*!

*/
struct MaterialBuffer
{
    RwRGBAReal	diffuseColor;
    float		diffuseIntensity;
    float		specularIntensity;
    float		glossiness;
    float		metallness;
    float       alphaCutoff; // dual-pass alpha test cutoff (0 = disabled)
    float       alphaBlendPass; // 1 during the soft-edge blended pass of a dual-pass mesh
    int         hasNormalTex;
    int			hasSpecTex;
    float       alphaMipCoverage; // per-mip alpha boost when dual-pass soft edges are disabled
    float       padd__[3];
};
struct RwGraphicsMatrix;
class CD3DRenderer;
/*! \class CD3D1XRenderBuffersManager
    \brief Render buffers manager class.

    This class manages render buffers for current frame or current draw call. Responsible for matrix and material state update.
*/
class CD3D1XRenderBuffersManager
{
public:
    /*!
        Initializes per-frame, per-object and per-material constant buffers
    */
    CD3D1XRenderBuffersManager();
    /*!
        Releases allocated resources
    */
    ~CD3D1XRenderBuffersManager();

    /*!
        Updates current view and projection matrices
    */
    void UpdateViewProjMatricles( RwMatrix &view, RwMatrix &proj );
    /*!
        Updates current view matrix
    */
    void UpdateViewMatrix( RwMatrix &view );
    /*!
        Configures per-frame camera motion inputs (unjittered previous view-projection and current NDC jitter).
    */
    void SetMotionCamera( const RwMatrix &prevViewProj, float jitterX, float jitterY, bool externalTAA );
    /*!
        Advances the rendered frame counter for per-object world matrix history.
    */
    void NextFrame();
    /*!
        Returns the current rendered frame counter.
    */
    unsigned int GetFrame() const { return m_nFrame; }
    /*!
        Updates current world matrix
    */
    void UpdateWorldMatrix( RwMatrix *ltm );
    /*!
        Returns current world matrix
    */
    RwMatrix * GetCurrentWorldMatrix()
    {
        return m_pCurrentWorldMatrix;
    }
    /*!
        Multiplies 2 matrices in homogenous space(using 4th vector in matrix)
    */
    void Multipy4x4Matrices( RwGraphicsMatrix* res, RwGraphicsMatrix* a, RwGraphicsMatrix* b );
    /*!
        Inverts 4x4 matrix
    */
    void Inverse4x4Matrix( RwGraphicsMatrix* a, RwGraphicsMatrix* b );
    /*!
        Transposes 4x4 matrix
    */
    void Transpose4x4Matrix( RwGraphicsMatrix* a, RwGraphicsMatrix* b );
    /*!
        Updates current world space matrix buffer
    */
    void SetMatrixBuffer();
    /*!
        Updates current diffuse color for material
    */
    void UpdateMaterialDiffuseColor( const RwRGBA &color );
    /*!
        Updates current emmissive color for material
    */
    void UpdateMaterialEmmissiveColor( const RwRGBA &color );
    /*!
        Updates current specular intensity for material
    */
    void UpdateMaterialSpecularInt( const float &intensity );
    /*!
        Updates current glossiness for material
    */
    void UpdateMaterialGlossiness( const float &intensity );
    /*!
        Updates current metallness for material
    */
    void UpdateMaterialMetalness( const float &intensity );
    /*!
        Updates current value of specular texture avaliablity
    */
    void UpdateHasSpecTex( const int &hastex );
    /*!
        Updates current value of normal texture avaliablity
    */
    void UpdateHasNormalTex( const int &hastex );
    /*!
        Updates dual-pass alpha cutout, soft-edge blend pass flag, and per-mip coverage boost.
    */
    void UpdateAlphaPass( float cutoff, bool blendPass, float mipCoverage = 0.0f );
    /*!
        Flushes all material buffer changes if there was any
    */
    void FlushMaterialBuffer();
private:
    CD3D1XConstantBuffer<PerFrameMatrixBuffer> *	m_pPerFrameMatrixBuffer = nullptr;
    CD3D1XConstantBuffer<PerObjectMatrixBuffer> *	m_pPerObjectMatrixBuffer = nullptr;
    CD3D1XConstantBuffer<MaterialBuffer> *			m_pPerMaterialBuffer = nullptr;
    bool m_bMaterialBufferRequiresUpdate = false;
    RwMatrix *		m_pCurrentWorldMatrix = nullptr;
    RwMatrix *		m_pOldWorldMatrix = nullptr;
    // world matrix as last uploaded, see SetMatrixBuffer
    RwMatrix		m_mUploadedWorld{};
    RwMatrix		m_mUploadedWorldPrev{};
    struct WorldHistory
    {
        RwMatrix current;
        RwMatrix previous;
        unsigned int frame;
    };
    std::unordered_map<RwMatrix *, WorldHistory> m_worldHistory;
    unsigned int m_nFrame = 2; // start at 2 so default-initialized frame 0 is never treated as the previous frame
};
extern CD3D1XRenderBuffersManager* g_pRenderBuffersMgr;
