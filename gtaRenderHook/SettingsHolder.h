#pragma once

#pragma warning( push, 0 )
#include <tinyxml2.h>
#include <AntTweakBar.h>
#pragma warning( pop )
#include <atomic>
#include <unordered_map>
#include "SettingsFields.h"
class SettingsBlock;
class CD3D1XShader;
#include "D3D1XShaderDefines.h"
/*!
    \class SettingsHolder
    \brief Holds render hook settings in memory.
*/
class SettingsHolder
{
public:
    /// Global instance of settings holder.
    static SettingsHolder* Instance()
    {
        static SettingsHolder instance;
        return &instance;
    }
    /// Adds new settings block to settings holder
    void AddSettingBlock( SettingsBlock* block );
    /// Resets settings blocks to their default values
    void ResetSettings();
    /// Saves settings to default settings file
    void SaveSettings();
    /// Reloads settings from xml file
    void ReloadFile();
    /// Reloads shaders binded to settings blocks
    void ReloadShadersIfRequired();
    /// Initializes AntTweakBar GUI
    void InitGUI();
    /// Renders AntTweakBar GUI
    void DrawGUI();
    /// Returns true if AntTweakBar GUI is enabled
    bool IsGUIEnabled();
    /// Enables AntTweakBar GUI Display
    void EnableGUI();
    /// Disables AntTweakBar GUI Display
    void DisableGUI();
    /// Ends the AntTweakBar library and discards the GUI state its termination destroys. Safe on a library that was
    /// never started, so a teardown does not have to know whether an init ran first.
    void TerminateGUI();
    /// Records whether the AntTweakBar library is initialised. Set where a TwInit succeeds and cleared where the
    /// library is terminated, so it is the one fact that answers whether any entry point above may call into it.
    void SetGUILibraryAvailable( bool bAvailable );
    /// True while the library is initialised, so the settings bar either exists or can still be created.
    bool IsGUILibraryAvailable() const;
private:
    /// Loads settings from XML file
    void LoadSettings( const tinyxml2::XMLDocument &file );

    std::list<SettingsBlock*>	m_aSettingsBlocks;
    tinyxml2::XMLDocument			mSettingsFile;
    static std::string m_sSettingsFileName;
    bool m_bInitialized = false;
    // Written by the start and stop path and read by the frame path and the AntTweakBar message hook, which a host driving
    // frames on its own thread can reach from another thread, so the handle and the two flags below are atomic and every use
    // names its ordering. The same defence the frame gate in RwD3D1XEngine.h carries. See TerminateGUI for what it cant order.
    std::atomic<bool> m_bDrawTweakBar = false;
    std::atomic<TwBar*> m_pGuiholder = nullptr;
    std::atomic<bool> m_bGUILibraryAvailable = false;
};

/*!
    Settings block base class, used to hold various settings
*/
class SettingsBlock
{
public:
    SettingsBlock()
    {
        Reset();
        SettingsHolder::Instance()->AddSettingBlock( this );
    }
    /*
        \brief Saves contents of SettingsBlock to \ref XMLDocument.
        \returns Pointer to created XMLElement.
    */
    virtual tinyxml2::XMLElement* Save( tinyxml2::XMLDocument* doc );
    /*
        Loads contents of SettingsBlock from \ref XMLDocument.
    */
    virtual void Load( const tinyxml2::XMLDocument &doc );
    /*
        Resets contents of SettingsBlock to default values.
    */
    virtual void Reset();
    /*
        Initializes AntTweakBar GUI
    */
    virtual void InitGUI( TwBar* guiholder );
    /*
        Reloads shaders if required.
    */
    virtual void ReloadShaders();
    /*
        Returns value of integer settings field if this value exists 0 otherwise.
    */
    int GetInt( const std::string& name )
    {
        IntSField* field = dynamic_cast<IntSField*>( m_aFields[name] );
        return field != nullptr ? field->GetValue() : 0;
    }
    /*
        Returns value of unsigned integer settings field if this value exists 0 otherwise.
    */
    unsigned int GetUInt( const std::string& name )
    {
        UIntSField* field = dynamic_cast<UIntSField*>( m_aFields[name] );
        return field != nullptr ? field->GetValue() : 0;
    }
    /*
        Returns value of floating-point settings field if this value exists 0.0f otherwise.
    */
    float GetFloat( const std::string& name )
    {
        FloatSField* field = dynamic_cast<FloatSField*>( m_aFields[name] );
        return field != nullptr ? field->GetValue() : 0.0f;
    }
    /*
        Returns value of boolean settings field if this value exists false otherwise.
    */
    bool GetToggleField( const std::string& name )
    {
        ToggleSField* field = dynamic_cast<ToggleSField*>( m_aFields[name] );
        return field != nullptr ? field->GetValue() : false;
    }
    /// Name of settings block
    std::string m_sName;
    /// Pointers to shaders binded to this SettingsBlock
    std::vector<CD3D1XShader*> m_aShaderPointers;
    /// Shader define list for shaders binded to this SettingsBlock.
    CD3D1XShaderDefineList m_ShaderDefineList;

    bool m_bShaderReloadRequired = false;
protected:
    std::unordered_map<std::string, SettingsField*> m_aFields;
};

// Global shader settings block class, used to hold shader defenitions
class ShaderDefinesSettingsBlock : public SettingsBlock
{
public:
    ShaderDefinesSettingsBlock()
    {
        m_sName = "ShaderSettings";
        m_aFields["UsePBR"] = new ToggleSField( "UsePBR", true, false, true, m_sName, true );
        Reset();
    }
};
// Debug settings block class, used to hold debugging settings
class DebugSettingsBlock : public SettingsBlock
{
public:
    DebugSettingsBlock()
    {
        m_sName = "Debug";
        m_aFields["Windowed"] = new ToggleSField( "Windowed", true, false, false, m_sName, true );
        m_aFields["UseDefaultAdapter"] = new ToggleSField( "UseDefaultAdapter", true, false, false, m_sName, true );
        m_aFields["UseIdleHook"] = new ToggleSField( "UseIdleHook", false, true, false, m_sName, true );
        m_aFields["ShowPreformanceCounters"] = new ToggleSField( "ShowPreformanceCounters", false, false, false, m_sName );
        m_aFields["DebugMessaging"] = new ToggleSField( "DebugMessaging", false, false, false, m_sName, true );
        m_aFields["DebugRenderTarget"] = new ToggleSField( "DebugRenderTarget", false, true, false, m_sName );
        m_aFields["DebugLevel"] = new IntSField( "DebugLevel", true, false, false, m_sName, 0, 0, 2 );
        Reset();
    }
    virtual void Reset() override;
    virtual void InitGUI( TwBar* guiholder ) override;
public:
    unsigned int DebugRenderTargetNumber;
    std::vector<RwRaster*> DebugRenderTargetList;
};

extern DebugSettingsBlock gDebugSettings;
extern ShaderDefinesSettingsBlock gShaderDefineSettings;