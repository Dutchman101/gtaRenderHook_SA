#ifndef DEBUG_H
#define DEBUG_H
#include "SettingsHolder.h"
#include <chrono>
#include <set>
using namespace std;
typedef chrono::high_resolution_clock hr_clock;

// Debug Counter, useful for call counting, per frame or per session
class CCounter
{
private:
    int		m_Count = 0;
    int		m_FrameCount = 0;
    int		m_FullChangeCount = 0;
    bool	m_TimerStart = false;
    int		m_minCount = 100000000;
    int		m_maxCount = 0;
    string	m_Name{};
public:
    explicit CCounter( const string& name ) :m_Name{ name }
    {
    }
    // Increase count.
    void Increase()
    {
        if ( m_TimerStart )
            m_Count++;
    }
    // Begin counting
    void Start()
    {
        m_TimerStart = true;
    }
    // Stop counting
    void Stop()
    {
        m_TimerStart = false;
        m_FullChangeCount += m_Count;
        m_FrameCount++;
        m_minCount = min( m_minCount, m_Count );
        m_maxCount = max( m_maxCount, m_Count );
        m_Count = 0;
    }
    // Return counter debug info.
    string GetCounterResult()
    {
        return  m_Name + " Count: " + to_string( m_Count ) +
            "; Per Frame - Min:" + to_string( m_minCount ) +
            "; Max:" + to_string( m_maxCount ) +
            "; Avg:" + to_string( m_FullChangeCount / max( m_FrameCount, 1 ) );
    }
};



class CPerfTimer
{
private:
    chrono::time_point<hr_clock> beg;
    __int64 msCount = -1;
    string	m_Name{};
public:
    explicit CPerfTimer( const string &Name ) : m_Name{ Name }
    {
    }
    // Begin counting
    void Start()
    {
        beg = hr_clock::now();
    }
    // Stop counting
    void Stop()
    {
        msCount = chrono::duration_cast<std::chrono::microseconds>( hr_clock::now() - beg ).count();
    }
    // Return counter debug info.
    string GetTimerResult()
    {
        char buff[100];
        snprintf( buff, sizeof( buff ), "%.3f", ( msCount / 1000.0 ) );

        return m_Name + ": " + string( buff ) + " ms";
    }
};

/*
    Debugging class, thread unsafe right now.
    TODO: Make it thread-safe, create cpp move all defenitions to cpp
*/
class CDebug
{
public:
    explicit CDebug( const std::string &acFileName ) :
        pLogStream{ new ofstream( acFileName ) },
        m_fileName{ acFileName }
    {
    }
    // Prints debug message to log file. logLevel: 0 - errors only, 1 - method logs, 2 - in/out log
    // The body is guarded because this is called from failure paths, including the device-creation
    // ones, and the message copy allocates: an unguarded throw here would replace the reported fault.
    void printMsg( const string &msg, int logLevel )
    {
        try
        {
            if ( gDebugSettings.GetToggleField( "DebugMessaging" ) &&
                 logLevel <= gDebugSettings.GetInt( "DebugLevel" ) )
            {
                if ( pLogStream )
                {
                    if ( !pLogStream->is_open() )
                        pLogStream->open( m_fileName, fstream::out | fstream::app );
                    *pLogStream << "Debug Message: " << msg << "\n";
                    OutputDebugString( ( msg + "\n" ).c_str() );
                    pLogStream->close();
                }
            }
        }
        catch ( ... )
        {
        }
    }

    // The literal form keeps the temporary the reference version needs inside the guard instead of in
    // the caller's argument list, for the same reason its printError counterpart exists.
    void printMsg( const char* msg, int logLevel )
    {
        if ( !msg )
            return;
        try
        {
            printMsg( string( msg ), logLevel );
        }
        catch ( ... )
        {
        }
    }

    // Records an error and returns. Callers decide what happens next, so a failure path can report
    // and then return instead of unwinding the process. CALL_D3D_API performs the hard failure.
    // An error is recorded whether or not DebugMessaging is on: that toggle controls the chatter of
    // printMsg, and a failure path that reports nothing cannot be told apart from one that worked.
    // Each distinct message is written once per run so an error raised every frame cannot flood the log.
    // Because the message is the key, every caller must name its own component in the text: a message
    // shared by several callers would report only the first of them.
    // Both the dedupe set and the message copy allocate, so the body is wrapped: this is where failures
    // are reported, and a failure to report must not become a failure that terminates.
    void printError( const string &msg )
    {
        try
        {
            if ( !m_reportedErrors.insert( msg ).second )
                return;
            if ( pLogStream )
            {
                if ( !pLogStream->is_open() )
                    pLogStream->open( m_fileName, fstream::out | fstream::app );
                *pLogStream << "Debug Error: " << msg << "\n";
                pLogStream->close();
            }
            OutputDebugString( ( msg + "\n" ).c_str() );
        }
        catch ( ... )
        {
        }
    }

    // A literal report comes here: the temporary the reference version needs is built inside the
    // guard instead of in the caller's argument list. Without it a report from a handler that is
    // already running after a failed allocation would allocate once more and leave as an exception.
    void printError( const char* msg )
    {
        if ( !msg )
            return;
        try
        {
            printError( string( msg ) );
        }
        catch ( ... )
        {
        }
    }

    void SetD3DName( ID3D11DeviceChild* dc, const std::string& c_szName )
    {
        if ( !dc )
            return;
        dc->SetPrivateData( WKPDID_D3DDebugObjectName, c_szName.length(), c_szName.data() );
    }

    ~CDebug()
    {
        delete pLogStream;
    }
private:
    ofstream* pLogStream = nullptr;
    string m_fileName{};
    set<string> m_reportedErrors;
};

extern CDebug* g_pDebug;
#endif // !DEBUG_H

