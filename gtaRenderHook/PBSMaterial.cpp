
// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "PBSMaterial.h"
#include "CDebug.h"
#include <filesystem>

RwTexDictionary *                               CPBSMaterialMgr::materialsTXD;
std::unordered_map<std::string_view, CPBSMaterial *, CPBSMaterialNameHash, CPBSMaterialNameEqual>
    CPBSMaterialMgr::materials{};
namespace
{
    constexpr const char *kMaterialExtension = ".mat";

    // Case-insensitive suffix test using the same folding as the name lookup above, so the extension a file is
    // accepted under and the name it is later matched by can never disagree on case.
    bool HasSuffix( std::string_view text, std::string_view suffix )
    {
        return text.size() >= suffix.size() &&
               CPBSMaterialNameEqual{}( text.substr( text.size() - suffix.size() ), suffix );
    }
}

CPBSMaterial::CPBSMaterial( const std::string &fname )
    : m_sName( HasSuffix( fname, kMaterialExtension )
                   ? std::filesystem::path( fname ).stem().string()
                   : fname )
{
    if ( CPBSMaterialMgr::materialsTXD == nullptr )
        return;
    auto file = fopen(
        ( HasSuffix( fname, kMaterialExtension ) ? fname : ( std::string( "materials\\" ) + m_sName + kMaterialExtension ) ).c_str(), "rt" );
    char specFileName[80]{};
    char normalFileName[80]{};
    if ( file )
    {
        auto res = fscanf( file, "%79s\n", specFileName );
        if ( res != 1 )
        {
            fclose( file );
            return;
        }
        m_tSpecRoughness = _RwTexDictionaryFindNamedTexture(
            CPBSMaterialMgr::materialsTXD, specFileName );

        res = fscanf( file, "%79s\n", normalFileName );

        if ( res != 1 )
        {
            fclose( file );
            return;
        }
        m_tNormals = _RwTexDictionaryFindNamedTexture(
            CPBSMaterialMgr::materialsTXD, normalFileName );
        fclose( file );
    }
    // m_tSpecRoughness
}

void CPBSMaterialMgr::LoadMaterials()
{
    // Each map key is a view into the m_sName of the material it points at, so the entry has to be erased before that
    // material is freed.
    for ( auto it = materials.begin(); it != materials.end(); )
    {
        CPBSMaterial *material = it->second;
        it = materials.erase( it );
        delete material;
    }
    materialsTXD = nullptr;
    std::string     path( "materials/" );
    // exists() answers "is there something at this path" and reports two different answers as false: nothing is there,
    // and there is something this process could not query. Only the error code separates them, so both exists() calls
    // below read it or a folder that is present but unreadable gets reported as missing.
    std::error_code ec;
    if ( !std::filesystem::exists( path, ec ) )
    {
        if ( ec )
            g_pDebug->printError( "Materials folder: it could not be read, so material rendering will be disabled." );
        else
            g_pDebug->printMsg(
                "Materials folder not found, material rendering will be disabled.",
                1 );
        return;
    }
    
    if ( !std::filesystem::exists( "materials\\materials.txd", ec ) )
    {
        if ( ec )
            g_pDebug->printError( "Materials TXD: it could not be read, so material rendering will be disabled." );
        else
            g_pDebug->printMsg(
                "Materials TXD not found, material rendering will be disabled.",
                1 );
        return;
    }
    materialsTXD = CFileLoader::LoadTexDictionary( "materials\\materials.txd" );
    if ( materialsTXD == nullptr )
    {
        g_pDebug->printError( "Materials TXD: it could not be loaded, so material rendering will be disabled." );
        return;
    }
    // The walk advances with the error_code overload of increment(), because the range-for this replaces advanced
    // with operator++, which throws on a directory it cannot read. A failed increment is checked and ends the walk:
    // only exhaustion is specified to leave the iterator at the end, so the position after a failed increment is not
    // known and retrying it could revisit the same entry for as long as the failure lasts.
    std::error_code scanEc;
    auto dir_iter = std::filesystem::recursive_directory_iterator( path, scanEc );
    if ( scanEc )
    {
        g_pDebug->printError( "Materials folder: it could not be read, so material rendering will be disabled." );
        return;
    }
    while ( dir_iter != std::filesystem::recursive_directory_iterator() )
    {
        const std::filesystem::directory_entry &p = *dir_iter;
        // A directory can carry the same extension as a material file, so only regular files are opened.
        std::error_code fileEc;
        if ( p.is_regular_file( fileEc ) && !fileEc &&
             HasSuffix( p.path().extension().string(), kMaterialExtension ) )
        {
            auto materialptr = new CPBSMaterial( p.path().string() );
            if ( materialptr->m_tSpecRoughness != nullptr &&
                 materialptr->m_tSpecRoughness->raster != nullptr )
            {
                const std::string_view name( materialptr->m_sName );
                auto it = materials.find( name );
                if ( it != materials.end() )
                {
                    // The stored key is a view into the material being replaced, so the entry has to go before the
                    // delete or the map would be left holding a key into freed storage.
                    CPBSMaterial *duplicate = it->second;
                    materials.erase( it );
                    delete duplicate;
                }
                materials.emplace( name, materialptr );
            }
            else
            {
                delete materialptr;
            }
        }
        dir_iter.increment( scanEc );
        if ( scanEc )
        {
            g_pDebug->printError( "Materials scan: an error aborted the directory walk, so material rendering may be incomplete." );
            break;
        }
    }
}

void CPBSMaterialMgr::SetMaterial( const char *textureName ) {}

CPBSMaterial *CPBSMaterialMgr::FindMaterial( const char *textureName )
{
    if ( textureName == nullptr || textureName[0] == '\0' || materials.empty() )
        return nullptr;
    auto it = materials.find( std::string_view( textureName ) );
    return ( it != materials.end() ) ? it->second : nullptr;
}
