#ifndef DBSPECIFIC_H
#define DBSPECIFIC_H

// Items specific to databases.
//
// Obviously we'd like to minimize this, but if they are needed this file isolates them.  I'd like for there to be a
// single build of pyodbc on each platform and not have a bunch of defines for supporting different databases.

#ifdef _MSC_VER
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

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

// BCP function prototypes
typedef SQLRETURN (SQL_API *bcp_init_t)(HDBC, const SQLCHAR*, const SQLCHAR*, const SQLCHAR*, DBINT);
typedef SQLRETURN (SQL_API *bcp_bind_t)(HDBC, SQLPOINTER, SQLINTEGER, SQLLEN, SQLPOINTER, SQLINTEGER, SQLSMALLINT, SQLINTEGER);
typedef SQLRETURN (SQL_API *bcp_sendrow_t)(HDBC);
typedef DBINT (SQL_API *bcp_batch_t)(HDBC);
typedef DBINT (SQL_API *bcp_done_t)(HDBC);

// BCP constants
#define DB_IN 1
#define DB_OUT 2
#define SQL_NTS (-3)
#define SQL_COPT_SS_BCP 1204
#define SQL_BCP_ON 1
#define SQL_BCP_OFF 0

// Function declarations
extern bcp_init_t bcp_init;
extern bcp_bind_t bcp_bind;
extern bcp_sendrow_t bcp_sendrow;
extern bcp_batch_t bcp_batch;
extern bcp_done_t bcp_done;

#endif // DBSPECIFIC_H
