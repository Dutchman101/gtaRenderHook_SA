#pragma once
enum class eTextureDimension
{
    TT_Unk,
    TT_1D,
    TT_2D,
    TT_3D
};

class ID3D1XRenderTargetViewable
{
public:
    virtual ID3D11RenderTargetView* GetRenderTargetView() const = 0;
};

class ID3D1XDepthStencilViewable
{
public:
    virtual ID3D11DepthStencilView* GetDepthStencilView() const = 0;
};

class ID3D1XShaderViewable
{
  public:
    virtual ID3D11ShaderResourceView *GetShaderResourceView() const = 0;
};

/*!
    Capability of a raster resource that owns an unordered access view. Nothing in this hierarchy creates one yet, so
    the query below resolves to null for every current resource and a caller that binds UAVs gets its slots cleared
    rather than filled.
*/
class ID3D1XUnorderedAccessViewable
{
  public:
    virtual ID3D11UnorderedAccessView *GetUnorderedAccessView() const = 0;
};

class CD3D1XBaseTexture
{
public:
    CD3D1XBaseTexture( RwRaster* parent, eTextureDimension dimension, const std::string& resourceTypeName );
    virtual ~CD3D1XBaseTexture();
    virtual void SetDebugName( const std::string& name );
    virtual void Reallocate();
    ID3D11Resource* GetResource() const { return m_pTextureResource; }
    /*!
        Returns if texture is currently locked for reading
    */
    bool	IsLockedToRead() const { return m_bIsLocked; }
    /*!
        Clears the read lock flag without touching the staging copy, for a lock that failed after setting it
    */
    void	ResetLockedToRead() { m_bIsLocked = false; }
    /*!
        Returns the raster this texture was created from
    */
    RwRaster*	GetParent() const { return m_pParent; }
    /*!
        Locks texture to preform some operation on texture buffer
    */
    virtual void* LockToRead();
    /*!
        Unlocks texture from reading
    */
    virtual void	UnlockFromRead();
protected:
    ID3D11Resource*		m_pTextureResource = nullptr;
    RwRaster*			m_pParent = nullptr;
    eTextureDimension	m_dimension = eTextureDimension::TT_Unk;
    std::string			m_resourceTypeName = "BaseTexture";
    bool m_bIsLocked = false;
};

