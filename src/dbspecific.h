#ifndef DBSPECIFIC_H
#define DBSPECIFIC_H

// Items specific to databases.
//
// Obviously we'd like to minimize this, but if they are needed this file isolates them.  I'd like for there to be a
// single build of pyodbc on each platform and not have a bunch of defines for supporting different databases.


// ---------------------------------------------------------------------------------------------------------------------
// SQL Server


#define SQL_SS_VARIANT -150     // SQL Server 2008 SQL_VARIANT type
#define SQL_SS_XML -152         // SQL Server 2005 XML type
#define SQL_DB2_DECFLOAT -360   // IBM DB/2 DECFLOAT type
#define SQL_DB2_XML -370        // IBM DB/2 XML type
#define SQL_SS_TIME2 -154       // SQL Server 2008 time type

struct SQL_SS_TIME2_STRUCT
{
   SQLUSMALLINT hour;
   SQLUSMALLINT minute;
   SQLUSMALLINT second;
   SQLUINTEGER  fraction;
};

// The SQLGUID type isn't always available when compiling, so we'll make our own with a
// different name.

struct PYSQLGUID
{
    // I was hoping to use uint32_t, etc., but they aren't included in a Python build.  I'm not
    // going to require that the compilers supply anything beyond that.  There is PY_UINT32_T,
    // but there is no 16-bit version.  We'll stick with Microsoft's WORD and DWORD which I
    // believe the ODBC headers will have to supply.
    DWORD Data1;
    WORD Data2;
    WORD Data3;
    byte Data4[8];
};

// ---------------------------------------------------------------------------
// BCP ODBC Extension Definitions
// Minimal self-contained header replacements for Microsoft's bcp.h
// Allows pyODBC to call BCP APIs without additional SDK headers.
// ---------------------------------------------------------------------------

#ifndef DBINT
typedef long DBINT;        // 32-bit signed integer for BCP sizes/counts
#endif

// Return codes
#ifndef BCP_SUCCESS
#define BCP_SUCCESS        0
#endif
#ifndef BCP_ERROR
#define BCP_ERROR          (-1)
#endif
#ifndef BCP_EOF
#define BCP_EOF            (-2)
#endif

// Directions for bcp_init
#ifndef DB_IN
#define DB_IN              1   // From program to SQL Server
#endif
#ifndef DB_OUT
#define DB_OUT             2   // From SQL Server to program
#endif

// Options for bcp_control
#ifndef BCPMAXERRS
#define BCPMAXERRS         1
#endif
#ifndef BCPFIRST
#define BCPFIRST           2
#endif
#ifndef BCPLAST
#define BCPLAST            3
#endif
#ifndef BCPBATCH
#define BCPBATCH           4
#endif
#ifndef BCPKEEPNULLS
#define BCPKEEPNULLS       5
#endif
#ifndef BCPABORT
#define BCPABORT           6
#endif
#ifndef BCPHINTS
#define BCPHINTS           7
#endif
#ifndef BCPKEEPIDENTITY
#define BCPKEEPIDENTITY    8
#endif

// Special data length markers for bcp_collen
#ifndef SQL_NULL_DATA
#define SQL_NULL_DATA      (-1)
#endif
#ifndef SQL_VARLEN_DATA
#define SQL_VARLEN_DATA    (-10)
#endif

#endif // DBSPECIFIC_H
