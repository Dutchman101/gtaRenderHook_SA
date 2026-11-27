// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "SettingsHolder.h"
#include "CDebug.h"
#include "D3D1XShader.h"
#include "D3D1XStateManager.h"
#include "RwRenderEngine.h"
#include "RwD3D1XEngine.h"
#include "D3DRenderer.h"
#pragma warning( push, 0 )
#include <game_sa\CScene.h>
#pragma warning( pop )
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <crtdbg.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

DebugSettingsBlock gDebugSettings;
ShaderDefinesSettingsBlock gShaderDefineSettings;
std::string SettingsHolder::m_sSettingsFileName = "settings.xml";

void SettingsHolder::AddSettingBlock( SettingsBlock * block )
{
    m_aSettingsBlocks.push_back( block );
}

// Resets settings
void SettingsHolder::ResetSettings()
{
    for ( auto block : m_aSettingsBlocks )
        block->Reset();
}

void SettingsHolder::LoadSettings( const tinyxml2::XMLDocument &file )
{
    // Load settings
    for ( auto block : m_aSettingsBlocks )
        block->Load( file );
}

void SettingsHolder::SaveSettings()
{
    tinyxml2::XMLDocument newSettings;
    // Save each settings block
    for ( auto block : m_aSettingsBlocks )
        newSettings.InsertEndChild( block->Save( &newSettings ) );

    newSettings.SaveFile( m_sSettingsFileName.c_str() );
}

void SettingsHolder::ReloadFile()
{
    // Try to find settings file, if not found create default.
    if ( mSettingsFile.LoadFile( m_sSettingsFileName.c_str() ) == tinyxml2::XMLError::XML_ERROR_FILE_NOT_FOUND )
    {
        ResetSettings();
        SaveSettings();
        // Read back as well as written. Every shader define list is built inside a block's Load, so a first run that
        // stopped at SaveSettings would compile each settings-driven shader with none of its defines set.
        mSettingsFile.LoadFile( m_sSettingsFileName.c_str() );
    }
    LoadSettings( mSettingsFile );
    m_bInitialized = true;
}

void SettingsHolder::ReloadShadersIfRequired()
{
    for ( auto block : m_aSettingsBlocks )
    {
        if ( block->m_bShaderReloadRequired )
            block->ReloadShaders();
    }
}
void TW_CALL SaveDataCallBack( void *value )
{
    SettingsHolder::Instance()->SaveSettings();
}

void TW_CALL ReloadDataCallBack( void *value )
{
    SettingsHolder::Instance()->ReloadFile();
}

void SettingsHolder::InitGUI()
{
    // Nothing below is safe on a library that was never initialised, and TwInit is the only thing that makes it safe.
    // Without the test the frame path called TwNewBar every idle frame, because a bar never created leaves the handle
    // null and the second test would then never stop the retry.
    if ( !m_bGUILibraryAvailable.load( std::memory_order_acquire ) ||
        m_pGuiholder.load( std::memory_order_acquire ) != nullptr )
        return;
    // Init new settings bar
    TwDefine( " GLOBAL iconpos=topleft " );
    // Published once, before the blocks and buttons are added, so a second call arrives at a handle already set. A refused
    // bar is not published: leaving the handle null is what lets the next frame retry.
    TwBar* pGuiholder = TwNewBar( "Settings" );
    if ( pGuiholder == nullptr )
        return;
    m_pGuiholder.store( pGuiholder, std::memory_order_release );
    // The bar is shown and hidden with F12, so its own minimise button would leave it collapsed with no way back.
    TwDefine( " Settings label='RenderHook Settings (F12)' position='16 16' size='260 360' iconifiable=false " );
    // Init each settings block
    for ( auto block : m_aSettingsBlocks )
        block->InitGUI( pGuiholder );
    // Init save and reload buttons
    TwAddButton( pGuiholder, "Save", SaveDataCallBack, nullptr, "" );
    TwAddButton( pGuiholder, "Reload", ReloadDataCallBack, nullptr, "" );
}

void SettingsHolder::DrawGUI()
{
    // TwDraw writes to the device context and every call below only repairs what TwDraw changed, so the gate covers the
    // whole body: with the bar hidden nothing is drawn and there is nothing to repair.
    if ( !m_bDrawTweakBar.load( std::memory_order_acquire ) ||
        m_pGuiholder.load( std::memory_order_acquire ) == nullptr ) return;

    TwDraw();
    if ( g_pStateMgr )
    {
        CD3D1XShader::InvalidateStateCache();
        g_pStateMgr->InvalidateStateCache();
        if ( Scene.m_pRwCamera && Scene.m_pRwCamera->frameBuffer )
        {
            auto dxEngine = static_cast<CRwD3D1XEngine*>( g_pRwCustomEngine );
            if ( dxEngine && dxEngine->getRenderer() )
                dxEngine->getRenderer()->BeginUpdate( Scene.m_pRwCamera );
        }
    }
}

bool SettingsHolder::IsGUIEnabled()
{
    return m_bDrawTweakBar.load( std::memory_order_acquire );
}

void SettingsHolder::EnableGUI()
{
    // Called every frame while the bar is visible, so the definition below runs only on the transition. Refused while the
    // library is not initialised, because the flag it sets is what the input hooks test before forwarding a message.
    if ( !m_bGUILibraryAvailable.load( std::memory_order_acquire ) ||
        m_bDrawTweakBar.load( std::memory_order_acquire ) )
        return;
    m_bDrawTweakBar.store( true, std::memory_order_release );
    if ( m_pGuiholder.load( std::memory_order_acquire ) != nullptr )
        TwDefine( " Settings iconified=false " );
}

void SettingsHolder::DisableGUI()
{
    if ( !m_bDrawTweakBar.load( std::memory_order_acquire ) )
        return;
    m_bDrawTweakBar.store( false, std::memory_order_release );
    // The bar takes the mouse capture while visible, so the capture is released only on the enabled to disabled transition.
    ReleaseCapture();
    // AntTweakBarHookMsgProc stops forwarding messages to the bar while it is disabled, so a button release arriving after
    // this point never reaches AntTweakBar and a drag would still be running when the bar returns. It is delivered here.
    if ( m_pGuiholder.load( std::memory_order_acquire ) != nullptr )
        TwMouseButton( TW_MOUSE_RELEASED, TW_MOUSE_LEFT );
}

void SettingsHolder::TerminateGUI()
{
    // The library owns the bar and destroys it at terminate. A handle left here would be non-null, so InitGUI's test would
    // return early every frame and the settings screen would never come back. The visible flag goes with it, because
    // EnableGUI returns early while it is already set and the new bar would then never be shown.
    m_pGuiholder.store( nullptr, std::memory_order_release );
    m_bDrawTweakBar.store( false, std::memory_order_release );
    // Read and cleared in one step, so the answer and the refusal come out of the same write and every entry point above
    // refuses from here on. The handle and both flags are atomic, so another thread reads either the value before the clear
    // or the one written by it. A frame already inside TwDraw isnt stopped, and is what makes the terminate fail.
    const bool bLibraryWasLive = m_bGUILibraryAvailable.exchange( false, std::memory_order_release );
    if ( !bLibraryWasLive )
        return;
    // Zero has two causes in the library and they differ in what follows, so the line below names both. TwFreeAsyncDrawing
    // refusing because a draw is in flight returns before anything is freed, so the next TwInit is refused. ITwGraph::Delete
    // failing returns zero after the library was torn down anyway, so the next TwInit succeeds and the bar is rebuilt.
    if ( !TwTerminate() && g_pDebug )
        g_pDebug->printError( "AntTweakBar did not confirm its shutdown, so the settings bar is unavailable until the "
                              "engine is started again: either a draw was still in flight and the library and its bar "
                              "remain loaded, in which case the next start cannot re-initialise it, or its graph object "
                              "could not be deleted after the library was torn down, in which case the next start "
                              "rebuilds the bar." );
}

void SettingsHolder::SetGUILibraryAvailable( bool bAvailable )
{
    m_bGUILibraryAvailable.store( bAvailable, std::memory_order_release );
}

bool SettingsHolder::IsGUILibraryAvailable() const
{
    return m_bGUILibraryAvailable.load( std::memory_order_acquire );
}

void DebugSettingsBlock::Reset()
{
    SettingsBlock::Reset();
    DebugRenderTargetNumber = 0;
}

void DebugSettingsBlock::InitGUI( TwBar * guiholder )
{
    SettingsBlock::InitGUI( guiholder );
    std::string rtIdSettings = "min=0 max=";
    TwAddVarRW( guiholder, "RenderTarget ID", TwType::TW_TYPE_UINT32, &DebugRenderTargetNumber,
        ( rtIdSettings + std::to_string( DebugRenderTargetList.empty() ? 0 : ( DebugRenderTargetList.size() - 1 ) ) + " group=Debug" ).c_str() );
}

tinyxml2::XMLElement * SettingsBlock::Save( tinyxml2::XMLDocument * doc )
{
    auto settingsNode = doc->NewElement( m_sName.c_str() );
    for ( auto field : m_aFields )
        if ( field.second->ToXML( settingsNode ) < 0 )
            break;

    return settingsNode;
}

void SettingsBlock::Load( const tinyxml2::XMLDocument & doc )
{
    auto debugSettingsNode = doc.FirstChildElement( m_sName.c_str() );
    // Debug
    for ( auto field : m_aFields )
        if ( field.second->FromXML( debugSettingsNode ) < 0 )
            break;
}

void SettingsBlock::Reset()
{
    for ( auto field : m_aFields )
        field.second->Reset();
}

void SettingsBlock::InitGUI( TwBar * guiholder )
{
    for ( auto field : m_aFields )
        field.second->Draw( guiholder );
}

void SettingsBlock::ReloadShaders()
{
    for ( auto shader : m_aShaderPointers )
        shader->Reload( &m_ShaderDefineList );
    m_bShaderReloadRequired = false;
}
