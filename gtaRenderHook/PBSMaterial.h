#pragma once
#include "game_sa/CFileLoader.h"
#include <cstdint>
#include <string_view>
#include <unordered_map>

// RenderWare resolves material texture names case-insensitively, folding only the ASCII letters a-z
// (RwTexDictionaryFindNamedTexture does exactly that before its byte compare), and Windows resolves file names
// case-insensitively too. Material names therefore have to be folded the same way on both sides of every lookup
// and every file-name comparison, or a .mat file found by the directory scan can never be matched by the mesh
// that asks for it. Only a-z is folded, so this stays byte for byte the rule the engine itself applies.
constexpr char FoldPBSMaterialNameChar( char c )
{
    return ( c >= 'a' && c <= 'z' ) ? static_cast<char>( c - ( 'a' - 'A' ) ) : c;
}

// Hash over the folded name. The lookup runs once per drawn mesh, so the hash is computed per query rather than
// cached, and it has to agree with CPBSMaterialNameEqual on which bytes are significant.
struct CPBSMaterialNameHash
{
    std::size_t operator()( std::string_view name ) const
    {
        // 32-bit hahs: the hash is returned as std::size_t, which is 32 bits on the 32-bit target, so the offset
        // basis and the prime are both 32 bits and the multiply stays inside the returned width.
        std::uint32_t hash = 2166136261u;
        for ( char c : name )
            hash = ( hash ^ static_cast<unsigned char>( FoldPBSMaterialNameChar( c ) ) ) * 16777619u;
        return static_cast<std::size_t>( hash );
    }
};

struct CPBSMaterialNameEqual
{
    bool operator()( std::string_view lhs, std::string_view rhs ) const
    {
        if ( lhs.size() != rhs.size() )
            return false;
        for ( std::size_t i = 0; i < lhs.size(); i++ )
            if ( FoldPBSMaterialNameChar( lhs[i] ) != FoldPBSMaterialNameChar( rhs[i] ) )
                return false;
        return true;
    }
};

/*!
    Physically based shader material
*/
class CPBSMaterial
{
public:
    CPBSMaterial( const std::string&fname );
    // The manager keys its map on views into m_sName, so this object may be neither copied nor moved: either would
    // leave a key pointing at storage that no longer holds the name.
    CPBSMaterial( const CPBSMaterial & )             = delete;
    CPBSMaterial & operator=( const CPBSMaterial & ) = delete;
    CPBSMaterial( CPBSMaterial && )                  = delete;
    CPBSMaterial & operator=( CPBSMaterial && )      = delete;
  const std::string m_sName{};
    RwTexture * m_tSpecRoughness{};
  RwTexture * m_tNormals{};
};

class CPBSMaterialMgr
{
public:
    static void LoadMaterials();
    static void SetMaterial( const char* textureName );
    static CPBSMaterial* FindMaterial( const char* textureName );
    static RwTexDictionary* materialsTXD;
    // Keys are views into the owning CPBSMaterial's own m_sName, so an entry has to be erased before its material is
    // freed: the key storage dies with the object the view points into.
    static std::unordered_map<std::string_view, CPBSMaterial*, CPBSMaterialNameHash, CPBSMaterialNameEqual> materials;
};
