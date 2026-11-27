#ifndef DEBUG_H
#define DEBUG_H
#include "SettingsHolder.h"
#include <chrono>
#include <climits>
#include <mutex>
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
    // Borrowed, not owned. Call sites pass literals and these are built once per frame, so a copy would cost two
    // allocations per construction for a label that never changes.
    const char *m_Name;
public:
    explicit CPerfTimer( const char *Name ) : m_Name{ Name }
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

        return string( m_Name ) + ": " + buff + " ms";
    }
};

/*
    Debugging class. The two dedupe sets and the log stream are guarded by one mutex, so a producer on any thread is safe:
    this object is process wide and the frame path is not the only thing that reports. gDebugSettings is not covered by that
    mutex, since it is read here and reloaded elsewhere, so a reload concurrent with a report is a caller's problem.
    The cap bounds the error and record channels only: printMsg consults neither set. Past the cap a message is counted and
    not written, and the count is written at 1, 2, 4, 8 and so on, so a log that stopped reporting says both that it stopped
    and how much. The count advances only once a line has landed, so a failed write leaves the notice pending.
    These definitions stay in this header rather than in a source file. That split is not done, and a later change that makes
    it is not undoing a decision recorded here.
*/
class CDebug
{
public:
    // Past this many distinct messages the sets stop growing. Far above what this tree's own call sites produce, so our
    // reporting never reaches it; what does is a caller formatting an address into a message, which can produce a new
    // string per value. The count published past it is what keeps such a caller from silence.
    static constexpr size_t MAX_REPORTED_MESSAGES = 4096;
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
                // Ignored on purpose, and this is the one channel that may. Chatter spends no claim and keeps no entry, so
                // a line that didnt land leaves nothing stale to retry, and writeLine reaches the debugger regardless.
                writeLine( "Debug Message: ", msg );
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

    // Records an error and returns: callers decide what happens next, so a failure path can report and then return, while
    // CALL_D3D_API performs the hard failure. Recorded whether or not DebugMessaging is on, since a failure that reports
    // nothing cant be told from one that worked. Each message is written once per run and only when its line landed, so an
    // error raised every frame cant flood the log; the message is the key, so every caller names its own component.
    void printError( const string &msg )
    {
        try
        {
            if ( ClaimReportOnce( m_reportedErrors, m_errorCap, msg, "Debug Error: " ) != EReportClaim::Fresh )
                return;
            if ( writeLine( "Debug Error: ", msg ) )
                return;
            // The claim is only worth holding while the line it stands for exists. Otherwise the message stays in the set,
            // every later occurrence reads as a duplicate, and one failed write loses that report for the run. An erase on
            // a set of strings does not allocate.
            lock_guard<mutex> guard( m_reportMutex );
            m_reportedErrors.erase( msg );
        }
        catch ( ... )
        {
        }
    }

    // A literal report comes here so the temporary is built inside the guard instead of in the caller's argument list, so
    // a report from a handler already running after a failed allocation cant allocate once more and leave as an exception.
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

    // Written whatever DebugMessaging and DebugLevel are set to, because a hook that installed with no record reads the
    // same as one that never ran. Answers whether the line reached the file, because a caller spending a once a run claim
    // needs to know: the site audit is the one record with no later occurrence to retry it.
    bool printRecord( const string &msg )
    {
        try
        {
            const EReportClaim claim = ClaimReportOnce( m_reportedRecords, m_recordCap, msg, "Debug Record: " );
            // Already in the file, so the record this caller asked about exists even though nothing was written now.
            if ( claim == EReportClaim::Duplicate )
                return true;
            // Past the cap, so no line was written and none will be until the notice below says how many were dropped.
            if ( claim == EReportClaim::Suppressed )
                return false;
            if ( writeLine( "Debug Record: ", msg ) )
                return true;
            // Same rule as the error channel: the set claims a line that has to exist, so a line that didnt land gives the
            // message back and a later occurrence writes it again.
            lock_guard<mutex> guard( m_reportMutex );
            m_reportedRecords.erase( msg );
        }
        catch ( ... )
        {
        }
        return false;
    }

    bool printRecord( const char* msg )
    {
        if ( !msg )
            return false;
        try
        {
            return printRecord( string( msg ) );
        }
        catch ( ... )
        {
        }
        return false;
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

    // How much one dedupe set has refused, and the count it next writes a line at. One per set, beside the set it
    // describes: a shared one would be spent by whichever filled first, leaving the other silent.
    struct SSuppressedCount
    {
        unsigned long m_ulSuppressed = 0;
        unsigned long m_ulReportAt = 1;
    };

    // What a claim on a message came to. A duplicate is not a loss, since the line it stands for is already in the file,
    // while a message past the cap was never written. Duplicate is answered from the set and a caller that couldnt write
    // its line takes the entry back out, so the two mean the same only while one producer at a time holds a claim. That
    // holds for the record channel; the error channel never reads Duplicate, so its producers are not bound by it.
    enum class EReportClaim
    {
        Duplicate,
        Fresh,
        Suppressed
    };
    // The claim on a message and the recording of it are made in one step under the same lock, so two threads cant both
    // believe they were first. The line is written on a later visit to that lock, which is why the requirement above has to
    // hold for the set to stand for what the file contains. The insertion allocates, which is why callers wrap this.
    EReportClaim ClaimReportOnce( set<string> &seen, SSuppressedCount &suppressed, const string &msg, const char *prefix )
    {
        // The cap line is built and written after the lock is released, never inside it: writeLine takes the same mutex, and
        // a non-recursive one taken twice on one thread stops here rather than reporting anything.
        bool bReportCap = false;
        unsigned long ulSuppressed = 0;
        {
            lock_guard<mutex> guard( m_reportMutex );
            // Membership is tested before the cap, because a message already in the set has a line in the file whether or
            // not the set is full, and that answer is what the record channel's caller spends its claim on. A found message
            // costs no insert, so this order is also cheaper for the repeats a per frame caller sends.
            if ( seen.find( msg ) != seen.end() )
            {
                return EReportClaim::Duplicate;
            }
            if ( seen.size() >= MAX_REPORTED_MESSAGES )
            {
                ulSuppressed = ++suppressed.m_ulSuppressed;
                bReportCap = ulSuppressed >= suppressed.m_ulReportAt;
            }
            else
            {
                seen.insert( msg );
                return EReportClaim::Fresh;
            }
        }
        if ( bReportCap )
        {
            // Names what stopped happening and how much, rather than claiming the run was healthy. The number is the point: a
            // log that stopped reporting and one with nothing to report produce the same file, so only a count tells a reader
            // which they hold. A doubling count keeps a session to a few lines and survives one write that doesnt land.
            char capLine[288];
            snprintf( capLine, sizeof( capLine ),
                      "has kept %zu distinct messages, which is the cap this plugin sets; %lu further "
                      "message(s) arrived and were counted and not written, and this line repeats at 1, 2, 4, 8 and so on",
                      MAX_REPORTED_MESSAGES, ulSuppressed );
            // Advanced only once the line has landed, so a failed write leaves the count where it is and the next suppressed
            // message brings this line back. The doubling stops at the width of the count rather than wrapping.
            if ( writeLine( prefix, capLine ) )
            {
                lock_guard<mutex> guard( m_reportMutex );
                suppressed.m_ulReportAt =
                    ulSuppressed > ( ULONG_MAX / 2 ) ? ULONG_MAX : ulSuppressed * 2;
            }
        }
        return EReportClaim::Suppressed;
    }

    // Shared by every channel so the file line and the debugger copy cant drift, and guarded because the stream and the
    // file name are one object however many threads report. Returns whether the file line was written, because a caller
    // that resends a notice reads that answer. The stream is cleared before the open and tested after the close: one that
    // failed once stays failed until cleared, so without both, one transient error makes every later line fail the same way.
    bool writeLine( const char *prefix, const string &msg )
    {
        bool bWritten = false;
        try
        {
            lock_guard<mutex> guard( m_reportMutex );
            if ( pLogStream )
            {
                if ( !pLogStream->is_open() )
                {
                    pLogStream->clear();
                    pLogStream->open( m_fileName, fstream::out | fstream::app );
                }
                *pLogStream << prefix << msg << "\n";
                pLogStream->close();
                bWritten = !pLogStream->fail();
            }
            OutputDebugString( ( msg + "\n" ).c_str() );
        }
        catch ( ... )
        {
        }
        return bWritten;
    }
    ofstream* pLogStream = nullptr;
    string m_fileName{};
    set<string> m_reportedErrors;
    set<string> m_reportedRecords;
    mutex m_reportMutex;
    SSuppressedCount m_errorCap;
    SSuppressedCount m_recordCap;
};

extern CDebug* g_pDebug;
#endif // !DEBUG_H

