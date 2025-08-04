// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "connection.h"
#include "cursor.h"
#include "pyodbcmodule.h"
#include "errors.h"
#include "cnxninfo.h"
#include "dbspecific.h"


static char connection_doc[] =
    "Connection objects manage connections to the database.\n"
    "\n"
    "Each manages a single ODBC HDBC.";

static Connection* Connection_Validate(PyObject* self)
{
    Connection* cnxn;

    if (self == 0 || !Connection_Check(self))
    {
        PyErr_SetString(PyExc_TypeError, "Connection object required");
        return 0;
    }

    cnxn = (Connection*)self;

    if (cnxn->hdbc == SQL_NULL_HANDLE)
    {
        PyErr_SetString(ProgrammingError, "Attempt to use a closed connection.");
        return 0;
    }

    return cnxn;
}


static char* StrDup(const char* text) {
  // Like StrDup but uses PyMem_Malloc for the memory.  This is only used for internal
  // encodings which are known to be ASCII.
  size_t cb = strlen(text) + 1;
  char* pb = (char*)PyMem_Malloc(cb);
  if (!pb) {
    PyErr_NoMemory();
    return 0;
  }
  memcpy(pb, text, cb);
  return pb;
}


static bool Connect(PyObject* pConnectString, HDBC hdbc, long timeout, PyObject* encoding)
{
    assert(PyUnicode_Check(pConnectString));

    SQLRETURN ret;

    if (timeout > 0)
    {
        Py_BEGIN_ALLOW_THREADS
        ret = SQLSetConnectAttrW(hdbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)(uintptr_t)timeout, SQL_IS_UINTEGER);
        Py_END_ALLOW_THREADS
        if (!SQL_SUCCEEDED(ret))
            RaiseErrorFromHandle(0, "SQLSetConnectAttr(SQL_ATTR_LOGIN_TIMEOUT)", hdbc, SQL_NULL_HANDLE);
    }

    const char* szEncoding = 0;
    Object encBytes;
    if (encoding)
    {
        if (PyUnicode_Check(encoding))
        {
          szEncoding = PyUnicode_AsUTF8(encoding);
        }
    }

    SQLWChar cstring(pConnectString, szEncoding ? szEncoding : ENCSTR_UTF16NE);
    if (!cstring.isValid())
        return false;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLDriverConnectW(hdbc, 0, cstring, SQL_NTS, 0, 0, 0, SQL_DRIVER_NOPROMPT);
    Py_END_ALLOW_THREADS
    if (SQL_SUCCEEDED(ret))
        return true;

    RaiseErrorFromHandle(0, "SQLDriverConnect", hdbc, SQL_NULL_HANDLE);

    return false;
}

static bool ApplyPreconnAttrs(HDBC hdbc, SQLINTEGER ikey, PyObject *value, char *strencoding)
{
    SQLRETURN ret;
    SQLPOINTER ivalue = 0;
    SQLINTEGER vallen = 0;

    SQLWChar sqlchar;

    if (PyLong_Check(value))
    {
        if (_PyLong_Sign(value) >= 0)
        {
            ivalue = (SQLPOINTER)PyLong_AsUnsignedLong(value);
            vallen = SQL_IS_UINTEGER;
        } else
        {
            ivalue = (SQLPOINTER)PyLong_AsLong(value);
            vallen = SQL_IS_INTEGER;
        }
    }
    else if (PyByteArray_Check(value))
    {
        ivalue = (SQLPOINTER)PyByteArray_AsString(value);
        vallen = SQL_IS_POINTER;
    }
    else if (PyBytes_Check(value))
    {
        ivalue = PyBytes_AsString(value);
        vallen = SQL_IS_POINTER;
    }
    else if (PyUnicode_Check(value))
    {
        sqlchar.set(value, strencoding ? strencoding : "utf-16le");
        ivalue = sqlchar.get();
        vallen = SQL_NTS;
    }
    else if (PySequence_Check(value))
    {
        // To allow for possibility of setting multiple attributes more than once.
        Py_ssize_t len = PySequence_Size(value);
        for (Py_ssize_t i = 0; i < len; i++)
        {
            Object v(PySequence_GetItem(value, i));
            if (!ApplyPreconnAttrs(hdbc, ikey, v.Get(), strencoding))
                return false;
        }
        return true;
    }
    else
    {
        RaiseErrorV(0, PyExc_TypeError, "Unsupported attrs_before type: %s",
                    Py_TYPE(value)->tp_name);
        return false;
    }

    Py_BEGIN_ALLOW_THREADS
    ret = SQLSetConnectAttrW(hdbc, ikey, ivalue, vallen);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle(0, "SQLSetConnectAttr", hdbc, SQL_NULL_HANDLE);
        Py_BEGIN_ALLOW_THREADS
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        Py_END_ALLOW_THREADS
        return false;
    }
    return true;
}

PyObject* Connection_New(PyObject* pConnectString, bool fAutoCommit, long timeout, bool fReadOnly,
                         PyObject* attrs_before, PyObject* encoding)
{
    //
    // Allocate HDBC and connect
    //

    Object attrs_before_o(attrs_before);
    HDBC hdbc = SQL_NULL_HANDLE;
    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle(0, "SQLAllocHandle", SQL_NULL_HANDLE, SQL_NULL_HANDLE);

    //
    // Attributes that must be set before connecting.
    //

    if (attrs_before)
    {
        Py_ssize_t pos = 0;
        PyObject* key = 0;
        PyObject* value = 0;

        Object encodingholder;
        char *strencoding = (encoding != 0) ?
            (PyUnicode_Check(encoding) ? PyBytes_AsString(encodingholder = PyCodec_Encode(encoding, "utf-8", "strict")) :
             PyBytes_Check(encoding) ? PyBytes_AsString(encoding) : 0) : 0;

        while (PyDict_Next(attrs_before, &pos, &key, &value))
        {
            SQLINTEGER ikey = 0;

            if (PyLong_Check(key))
                ikey = (int)PyLong_AsLong(key);
            if (!ApplyPreconnAttrs(hdbc, ikey, value, strencoding))
            {
                return 0;
            }
        }
    }

    if (!Connect(pConnectString, hdbc, timeout, encoding))
    {
        // Connect has already set an exception.
        Py_BEGIN_ALLOW_THREADS
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        Py_END_ALLOW_THREADS
        return 0;
    }

    //
    // Connected, so allocate the Connection object.
    //

    // Set all variables to something valid, so we don't crash in dealloc if this function fails.

#ifdef _MSC_VER
#pragma warning(disable : 4365)
#endif
    Connection* cnxn = PyObject_NEW(Connection, &ConnectionType);
#ifdef _MSC_VER
#pragma warning(default : 4365)
#endif

    if (cnxn == 0)
    {
        Py_BEGIN_ALLOW_THREADS
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        Py_END_ALLOW_THREADS
        return 0;
    }

    cnxn->hdbc         = hdbc;
    cnxn->nAutoCommit  = fAutoCommit ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
    cnxn->searchescape = 0;
    cnxn->maxwrite     = 0;
    cnxn->timeout      = 0;
    cnxn->map_sqltype_to_converter = 0;

    cnxn->attrs_before = attrs_before_o.Detach();

    // This is an inefficient default, but should work all the time.  When we are offered
    // single-byte text we don't actually know what the encoding is.  For example, with SQL
    // Server the encoding is based on the database's collation.  We ask the driver / DB to
    // convert to SQL_C_WCHAR and use the ODBC default of UTF-16LE.
    cnxn->sqlchar_enc.optenc = OPTENC_UTF16NE;
    cnxn->sqlchar_enc.name   = StrDup(ENCSTR_UTF16NE);
    cnxn->sqlchar_enc.ctype  = SQL_C_WCHAR;

    cnxn->sqlwchar_enc.optenc = OPTENC_UTF16NE;
    cnxn->sqlwchar_enc.name   = StrDup(ENCSTR_UTF16NE);
    cnxn->sqlwchar_enc.ctype  = SQL_C_WCHAR;

    cnxn->metadata_enc.optenc = OPTENC_UTF16NE;
    cnxn->metadata_enc.name   = StrDup(ENCSTR_UTF16NE);
    cnxn->metadata_enc.ctype  = SQL_C_WCHAR;

    // Note: I attempted to use UTF-8 here too since it can hold any type, but SQL Server fails
    // with a data truncation error if we send something encoded in 2 bytes to a column with 1
    // character.  I don't know if this is a bug in SQL Server's driver or if I'm missing
    // something, so we'll stay with the default ODBC conversions.
    cnxn->unicode_enc.optenc = OPTENC_UTF16NE;
    cnxn->unicode_enc.name   = StrDup(ENCSTR_UTF16NE);
    cnxn->unicode_enc.ctype  = SQL_C_WCHAR;

    if (!cnxn->sqlchar_enc.name || !cnxn->sqlwchar_enc.name || !cnxn->metadata_enc.name || !cnxn->unicode_enc.name)
    {
        PyErr_NoMemory();
        Py_DECREF(cnxn);
        return 0;
    }

    //
    // Initialize autocommit mode.
    //

    // The DB API says we have to default to manual-commit, but ODBC defaults to auto-commit.  We also provide a
    // keyword parameter that allows the user to override the DB API and force us to start in auto-commit (in which
    // case we don't have to do anything).

    if (fAutoCommit == false)
    {
        Py_BEGIN_ALLOW_THREADS
        ret = SQLSetConnectAttr(cnxn->hdbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)cnxn->nAutoCommit, SQL_IS_UINTEGER);
        Py_END_ALLOW_THREADS

        if (!SQL_SUCCEEDED(ret))
        {
            RaiseErrorFromHandle(cnxn, "SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT)", cnxn->hdbc, SQL_NULL_HANDLE);
            Py_DECREF(cnxn);
            return 0;
        }
    }

    if (fReadOnly)
    {
        Py_BEGIN_ALLOW_THREADS
        ret = SQLSetConnectAttr(cnxn->hdbc, SQL_ATTR_ACCESS_MODE, (SQLPOINTER)SQL_MODE_READ_ONLY, 0);
        Py_END_ALLOW_THREADS

        if (!SQL_SUCCEEDED(ret))
        {
            RaiseErrorFromHandle(cnxn, "SQLSetConnectAttr(SQL_ATTR_ACCESS_MODE)", cnxn->hdbc, SQL_NULL_HANDLE);
            Py_DECREF(cnxn);
            return 0;
        }
    }

    //
    // Gather connection-level information we'll need later.
    //

    Object info(GetConnectionInfo(pConnectString, cnxn));

    if (!info.IsValid())
    {
        Py_DECREF(cnxn);
        return 0;
    }

    CnxnInfo* p = (CnxnInfo*)info.Get();
    cnxn->odbc_major             = p->odbc_major;
    cnxn->odbc_minor             = p->odbc_minor;
    cnxn->supports_describeparam = p->supports_describeparam;
    cnxn->datetime_precision     = p->datetime_precision;
    cnxn->need_long_data_len     = p->need_long_data_len;
    cnxn->varchar_maxlength      = p->varchar_maxlength;
    cnxn->wvarchar_maxlength     = p->wvarchar_maxlength;
    cnxn->binary_maxlength       = p->binary_maxlength;

    return reinterpret_cast<PyObject*>(cnxn);
}

static char set_attr_doc[] =
    "set_attr(attr_id, value) -> None\n\n"
    "Calls SQLSetConnectAttr with the given values.\n\n"
    "attr_id\n"
    "  The attribute id (integer) to set.  These are ODBC or driver constants.\n\n"
    "value\n"
    "  An integer value.\n\n"
    "At this time, only integer values are supported and are always passed as SQLUINTEGER.";

static PyObject* Connection_set_attr(PyObject* self, PyObject* args)
{
    int id;
    int value;
    if (!PyArg_ParseTuple(args, "ii", &id, &value))
        return 0;

    Connection* cnxn = (Connection*)self;

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLSetConnectAttr(cnxn->hdbc, id, (SQLPOINTER)(intptr_t)value, SQL_IS_INTEGER);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle(cnxn, "SQLSetConnectAttr", cnxn->hdbc, SQL_NULL_HANDLE);
    Py_RETURN_NONE;
}

static char conv_clear_doc[] =
    "clear_output_converters() --> None\n\n"
    "Remove all output converter functions.";

static PyObject* Connection_conv_clear(PyObject* self, PyObject* args)
{
    UNUSED(args);
    Connection* cnxn = (Connection*)self;
    Py_XDECREF(cnxn->map_sqltype_to_converter);
    cnxn->map_sqltype_to_converter = 0;
    Py_RETURN_NONE;
}

static int Connection_clear(PyObject* self)
{
    // Internal method for closing the connection.  (Not called close so it isn't confused with the external close
    // method.)

    Connection* cnxn = (Connection*)self;

    if (cnxn->hdbc != SQL_NULL_HANDLE)
    {
        TRACE("cnxn.clear cnxn=%p hdbc=%d\n", cnxn, cnxn->hdbc);

        HDBC hdbc = cnxn->hdbc;
        cnxn->hdbc = SQL_NULL_HANDLE;
        Py_BEGIN_ALLOW_THREADS
        if (cnxn->nAutoCommit == SQL_AUTOCOMMIT_OFF)
            SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK);

        SQLDisconnect(hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        Py_END_ALLOW_THREADS
    }

    Py_XDECREF(cnxn->searchescape);
    cnxn->searchescape = 0;

    PyMem_Free((void*)cnxn->sqlchar_enc.name);
    cnxn->sqlchar_enc.name = 0;
    PyMem_Free((void*)cnxn->sqlwchar_enc.name);
    cnxn->sqlwchar_enc.name = 0;
    PyMem_Free((void*)cnxn->metadata_enc.name);
    cnxn->metadata_enc.name = 0;
    PyMem_Free((void*)cnxn->unicode_enc.name);
    cnxn->unicode_enc.name = 0;

    Py_XDECREF(cnxn->attrs_before);
    cnxn->attrs_before = 0;

    Py_XDECREF(cnxn->map_sqltype_to_converter);
    cnxn->map_sqltype_to_converter = 0;

    return 0;
}

static void Connection_dealloc(PyObject* self)
{
    Connection_clear(self);
    PyObject_Del(self);
}

static char close_doc[] =
    "Close the connection now (rather than whenever __del__ is called).\n"
    "\n"
    "The connection will be unusable from this point forward and a ProgrammingError\n"
    "will be raised if any operation is attempted with the connection.  The same\n"
    "applies to all cursor objects trying to use the connection.\n"
    "\n"
    "Note that closing a connection without committing the changes first will cause\n"
    "an implicit rollback to be performed.";

static PyObject* Connection_close(PyObject* self, PyObject* args)
{
    UNUSED(args);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    Connection_clear(self);

    Py_RETURN_NONE;
}

static PyObject* Connection_cursor(PyObject* self, PyObject* args)
{
    UNUSED(args);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    return (PyObject*)Cursor_New(cnxn);
}

static PyObject* Connection_execute(PyObject* self, PyObject* args)
{
    PyObject* result = 0;

    Cursor* cursor;
    Connection* cnxn = Connection_Validate(self);

    if (!cnxn)
        return 0;

    cursor = Cursor_New(cnxn);
    if (!cursor)
        return 0;

    result = Cursor_execute((PyObject*)cursor, args);

    Py_DECREF((PyObject*)cursor);

    return result;
}

enum
{
    GI_YESNO,
    GI_STRING,
    GI_UINTEGER,
    GI_USMALLINT,
};

struct GetInfoType
{
    SQLUSMALLINT infotype;
    int datatype; // GI_XXX
};

static const GetInfoType aInfoTypes[] = {

    // SQL_CONVERT_X
    { SQL_CONVERT_FUNCTIONS, GI_UINTEGER },
    { SQL_CONVERT_BIGINT, GI_UINTEGER },
    { SQL_CONVERT_BINARY, GI_UINTEGER },
    { SQL_CONVERT_BIT, GI_UINTEGER },
    { SQL_CONVERT_CHAR, GI_UINTEGER },
    { SQL_CONVERT_DATE, GI_UINTEGER },
    { SQL_CONVERT_DECIMAL, GI_UINTEGER },
    { SQL_CONVERT_DOUBLE, GI_UINTEGER },
    { SQL_CONVERT_FLOAT, GI_UINTEGER },
    { SQL_CONVERT_INTEGER, GI_UINTEGER },
    { SQL_CONVERT_LONGVARCHAR, GI_UINTEGER },
    { SQL_CONVERT_NUMERIC, GI_UINTEGER },
    { SQL_CONVERT_REAL, GI_UINTEGER },
    { SQL_CONVERT_SMALLINT, GI_UINTEGER },
    { SQL_CONVERT_TIME, GI_UINTEGER },
    { SQL_CONVERT_TIMESTAMP, GI_UINTEGER },
    { SQL_CONVERT_TINYINT, GI_UINTEGER },
    { SQL_CONVERT_VARBINARY, GI_UINTEGER },
    { SQL_CONVERT_VARCHAR, GI_UINTEGER },
    { SQL_CONVERT_LONGVARBINARY, GI_UINTEGER },
    { SQL_CONVERT_WCHAR, GI_UINTEGER },
    { SQL_CONVERT_INTERVAL_DAY_TIME, GI_UINTEGER },
    { SQL_CONVERT_INTERVAL_YEAR_MONTH, GI_UINTEGER },
    { SQL_CONVERT_WLONGVARCHAR, GI_UINTEGER },
    { SQL_CONVERT_WVARCHAR, GI_UINTEGER },
    { SQL_CONVERT_GUID, GI_UINTEGER },

    { SQL_ACCESSIBLE_PROCEDURES, GI_YESNO },
    { SQL_ACCESSIBLE_TABLES, GI_YESNO },
    { SQL_ACTIVE_ENVIRONMENTS, GI_USMALLINT },
    { SQL_AGGREGATE_FUNCTIONS, GI_UINTEGER },
    { SQL_ALTER_DOMAIN, GI_UINTEGER },
    { SQL_ALTER_TABLE, GI_UINTEGER },
    { SQL_ASYNC_MODE, GI_UINTEGER },
    { SQL_BATCH_ROW_COUNT, GI_UINTEGER },
    { SQL_BATCH_SUPPORT, GI_UINTEGER },
    { SQL_BOOKMARK_PERSISTENCE, GI_UINTEGER },
    { SQL_CATALOG_LOCATION, GI_USMALLINT },
    { SQL_CATALOG_NAME, GI_YESNO },
    { SQL_CATALOG_NAME_SEPARATOR, GI_STRING },
    { SQL_CATALOG_TERM, GI_STRING },
    { SQL_CATALOG_USAGE, GI_UINTEGER },
    { SQL_COLLATION_SEQ, GI_STRING },
    { SQL_COLUMN_ALIAS, GI_YESNO },
    { SQL_CONCAT_NULL_BEHAVIOR, GI_USMALLINT },
    { SQL_CORRELATION_NAME, GI_USMALLINT },
    { SQL_CREATE_ASSERTION, GI_UINTEGER },
    { SQL_CREATE_CHARACTER_SET, GI_UINTEGER },
    { SQL_CREATE_COLLATION, GI_UINTEGER },
    { SQL_CREATE_DOMAIN, GI_UINTEGER },
    { SQL_CREATE_SCHEMA, GI_UINTEGER },
    { SQL_CREATE_TABLE, GI_UINTEGER },
    { SQL_CREATE_TRANSLATION, GI_UINTEGER },
    { SQL_CREATE_VIEW, GI_UINTEGER },
    { SQL_CURSOR_COMMIT_BEHAVIOR, GI_USMALLINT },
    { SQL_CURSOR_ROLLBACK_BEHAVIOR, GI_USMALLINT },
    { SQL_DATABASE_NAME, GI_STRING },
    { SQL_DATA_SOURCE_NAME, GI_STRING },
    { SQL_DATA_SOURCE_READ_ONLY, GI_YESNO },
    { SQL_DATETIME_LITERALS, GI_UINTEGER },
    { SQL_DBMS_NAME, GI_STRING },
    { SQL_DBMS_VER, GI_STRING },
    { SQL_DDL_INDEX, GI_UINTEGER },
    { SQL_DEFAULT_TXN_ISOLATION, GI_UINTEGER },
    { SQL_DESCRIBE_PARAMETER, GI_YESNO },
    { SQL_DM_VER, GI_STRING },
    { SQL_DRIVER_NAME, GI_STRING },
    { SQL_DRIVER_ODBC_VER, GI_STRING },
    { SQL_DRIVER_VER, GI_STRING },
    { SQL_DROP_ASSERTION, GI_UINTEGER },
    { SQL_DROP_CHARACTER_SET, GI_UINTEGER },
    { SQL_DROP_COLLATION, GI_UINTEGER },
    { SQL_DROP_DOMAIN, GI_UINTEGER },
    { SQL_DROP_SCHEMA, GI_UINTEGER },
    { SQL_DROP_TABLE, GI_UINTEGER },
    { SQL_DROP_TRANSLATION, GI_UINTEGER },
    { SQL_DROP_VIEW, GI_UINTEGER },
    { SQL_DYNAMIC_CURSOR_ATTRIBUTES1, GI_UINTEGER },
    { SQL_DYNAMIC_CURSOR_ATTRIBUTES2, GI_UINTEGER },
    { SQL_EXPRESSIONS_IN_ORDERBY, GI_YESNO },
    { SQL_FILE_USAGE, GI_USMALLINT },
    { SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, GI_UINTEGER },
    { SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2, GI_UINTEGER },
    { SQL_GETDATA_EXTENSIONS, GI_UINTEGER },
    { SQL_GROUP_BY, GI_USMALLINT },
    { SQL_IDENTIFIER_CASE, GI_USMALLINT },
    { SQL_IDENTIFIER_QUOTE_CHAR, GI_STRING },
    { SQL_INDEX_KEYWORDS, GI_UINTEGER },
    { SQL_INFO_SCHEMA_VIEWS, GI_UINTEGER },
    { SQL_INSERT_STATEMENT, GI_UINTEGER },
    { SQL_INTEGRITY, GI_YESNO },
    { SQL_KEYSET_CURSOR_ATTRIBUTES1, GI_UINTEGER },
    { SQL_KEYSET_CURSOR_ATTRIBUTES2, GI_UINTEGER },
    { SQL_KEYWORDS, GI_STRING },
    { SQL_LIKE_ESCAPE_CLAUSE, GI_YESNO },
    { SQL_MAX_ASYNC_CONCURRENT_STATEMENTS, GI_UINTEGER },
    { SQL_MAX_BINARY_LITERAL_LEN, GI_UINTEGER },
    { SQL_MAX_CATALOG_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_CHAR_LITERAL_LEN, GI_UINTEGER },
    { SQL_MAX_COLUMNS_IN_GROUP_BY, GI_USMALLINT },
    { SQL_MAX_COLUMNS_IN_INDEX, GI_USMALLINT },
    { SQL_MAX_COLUMNS_IN_ORDER_BY, GI_USMALLINT },
    { SQL_MAX_COLUMNS_IN_SELECT, GI_USMALLINT },
    { SQL_MAX_COLUMNS_IN_TABLE, GI_USMALLINT },
    { SQL_MAX_COLUMN_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_CONCURRENT_ACTIVITIES, GI_USMALLINT },
    { SQL_MAX_CURSOR_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_DRIVER_CONNECTIONS, GI_USMALLINT },
    { SQL_MAX_IDENTIFIER_LEN, GI_USMALLINT },
    { SQL_MAX_INDEX_SIZE, GI_UINTEGER },
    { SQL_MAX_PROCEDURE_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_ROW_SIZE, GI_UINTEGER },
    { SQL_MAX_ROW_SIZE_INCLUDES_LONG, GI_YESNO },
    { SQL_MAX_SCHEMA_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_STATEMENT_LEN, GI_UINTEGER },
    { SQL_MAX_TABLES_IN_SELECT, GI_USMALLINT },
    { SQL_MAX_TABLE_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_USER_NAME_LEN, GI_USMALLINT },
    { SQL_MULTIPLE_ACTIVE_TXN, GI_YESNO },
    { SQL_MULT_RESULT_SETS, GI_YESNO },
    { SQL_NEED_LONG_DATA_LEN, GI_YESNO },
    { SQL_NON_NULLABLE_COLUMNS, GI_USMALLINT },
    { SQL_NULL_COLLATION, GI_USMALLINT },
    { SQL_NUMERIC_FUNCTIONS, GI_UINTEGER },
    { SQL_ODBC_INTERFACE_CONFORMANCE, GI_UINTEGER },
    { SQL_ODBC_VER, GI_STRING },
    { SQL_OJ_CAPABILITIES, GI_UINTEGER },
    { SQL_ORDER_BY_COLUMNS_IN_SELECT, GI_YESNO },
    { SQL_PARAM_ARRAY_ROW_COUNTS, GI_UINTEGER },
    { SQL_PARAM_ARRAY_SELECTS, GI_UINTEGER },
    { SQL_PROCEDURES, GI_YESNO },
    { SQL_PROCEDURE_TERM, GI_STRING },
    { SQL_QUOTED_IDENTIFIER_CASE, GI_USMALLINT },
    { SQL_ROW_UPDATES, GI_YESNO },
    { SQL_SCHEMA_TERM, GI_STRING },
    { SQL_SCHEMA_USAGE, GI_UINTEGER },
    { SQL_SCROLL_OPTIONS, GI_UINTEGER },
    { SQL_SEARCH_PATTERN_ESCAPE, GI_STRING },
    { SQL_SERVER_NAME, GI_STRING },
    { SQL_SPECIAL_CHARACTERS, GI_STRING },
    { SQL_SQL92_DATETIME_FUNCTIONS, GI_UINTEGER },
    { SQL_SQL92_FOREIGN_KEY_DELETE_RULE, GI_UINTEGER },
    { SQL_SQL92_FOREIGN_KEY_UPDATE_RULE, GI_UINTEGER },
    { SQL_SQL92_GRANT, GI_UINTEGER },
    { SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, GI_UINTEGER },
    { SQL_SQL92_PREDICATES, GI_UINTEGER },
    { SQL_SQL92_RELATIONAL_JOIN_OPERATORS, GI_UINTEGER },
    { SQL_SQL92_REVOKE, GI_UINTEGER },
    { SQL_SQL92_ROW_VALUE_CONSTRUCTOR, GI_UINTEGER },
    { SQL_SQL92_STRING_FUNCTIONS, GI_UINTEGER },
    { SQL_SQL92_VALUE_EXPRESSIONS, GI_UINTEGER },
    { SQL_SQL_CONFORMANCE, GI_UINTEGER },
    { SQL_STANDARD_CLI_CONFORMANCE, GI_UINTEGER },
    { SQL_STATIC_CURSOR_ATTRIBUTES1, GI_UINTEGER },
    { SQL_STATIC_CURSOR_ATTRIBUTES2, GI_UINTEGER },
    { SQL_STRING_FUNCTIONS, GI_UINTEGER },
    { SQL_SUBQUERIES, GI_UINTEGER },
    { SQL_SYSTEM_FUNCTIONS, GI_UINTEGER },
    { SQL_TABLE_TERM, GI_STRING },
    { SQL_TIMEDATE_ADD_INTERVALS, GI_UINTEGER },
    { SQL_TIMEDATE_DIFF_INTERVALS, GI_UINTEGER },
    { SQL_TIMEDATE_FUNCTIONS, GI_UINTEGER },
    { SQL_TXN_CAPABLE, GI_USMALLINT },
    { SQL_TXN_ISOLATION_OPTION, GI_UINTEGER },
    { SQL_UNION, GI_UINTEGER },
    { SQL_USER_NAME, GI_STRING },
    { SQL_XOPEN_CLI_YEAR, GI_STRING },
};


static PyObject* Connection_getinfo(PyObject* self, PyObject* args)
{
    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    unsigned long infotype;
    if (!PyArg_ParseTuple(args, "k", &infotype))
        return 0;

    unsigned int i = 0;
    for (; i < _countof(aInfoTypes); i++)
    {
        if (aInfoTypes[i].infotype == infotype)
            break;
    }

    if (i == _countof(aInfoTypes))
        return RaiseErrorV(0, ProgrammingError, "Unsupported getinfo value: %d", infotype);

    char szBuffer[0x1000];
    SQLSMALLINT cch = 0;

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetInfo(cnxn->hdbc, (SQLUSMALLINT)infotype, szBuffer, sizeof(szBuffer), &cch);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle(cnxn, "SQLGetInfo", cnxn->hdbc, SQL_NULL_HANDLE);
        return 0;
    }

    PyObject* result = 0;

    switch (aInfoTypes[i].datatype)
    {
    case GI_YESNO:
        result = (szBuffer[0] == 'Y') ? Py_True : Py_False;
        Py_INCREF(result);
        break;

    case GI_STRING:
        result = PyUnicode_FromStringAndSize(szBuffer, (Py_ssize_t)cch);
        break;

    case GI_UINTEGER:
    {
        SQLUINTEGER n = *(SQLUINTEGER*)szBuffer; // Does this work on PPC or do we need a union?
        result = PyLong_FromLong((long)n);
        break;
    }

    case GI_USMALLINT:
        result = PyLong_FromLong(*(SQLUSMALLINT*)szBuffer);
        break;
    }

    return result;
}

PyObject* Connection_endtrans(Connection* cnxn, SQLSMALLINT type)
{
    // If called from Cursor.commit, it is possible that `cnxn` is deleted by another thread when we release them
    // below.  (The cursor has had its reference incremented by the method it is calling, but nothing has incremented
    // the connections count.  We could, but we really only need the HDBC.)
    HDBC hdbc = cnxn->hdbc;

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLEndTran(SQL_HANDLE_DBC, hdbc, type);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle(cnxn, "SQLEndTran", hdbc, SQL_NULL_HANDLE);
        return 0;
    }

    Py_RETURN_NONE;
}

static PyObject* Connection_commit(PyObject* self, PyObject* args)
{
    UNUSED(args);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    TRACE("commit: cnxn=%p hdbc=%d\n", cnxn, cnxn->hdbc);

    return Connection_endtrans(cnxn, SQL_COMMIT);
}

static PyObject* Connection_rollback(PyObject* self, PyObject* args)
{
    UNUSED(args);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    TRACE("rollback: cnxn=%p hdbc=%d\n", cnxn, cnxn->hdbc);

    return Connection_endtrans(cnxn, SQL_ROLLBACK);
}

static char cursor_doc[] =
    "Return a new Cursor object using the connection.";

static char execute_doc[] =
    "execute(sql, [params]) --> Cursor\n"
    "\n"
    "Create a new Cursor object, call its execute method, and return it.  See\n"
    "Cursor.execute for more details.\n"
    "\n"
    "This is a convenience method that is not part of the DB API.  Since a new\n"
    "Cursor is allocated by each call, this should not be used if more than one SQL\n"
    "statement needs to be executed.";

static char commit_doc[] =
    "Commit any pending transaction to the database.";

static char rollback_doc[] =
    "Causes the the database to roll back to the start of any pending transaction.";

static char getinfo_doc[] =
    "getinfo(type) --> str | int | bool\n"
    "\n"
    "Calls SQLGetInfo, passing `type`, and returns the result formatted as a Python object.";


PyObject* Connection_getautocommit(PyObject* self, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    PyObject* result = (cnxn->nAutoCommit == SQL_AUTOCOMMIT_ON) ? Py_True : Py_False;
    Py_INCREF(result);
    return result;
}

static int Connection_setautocommit(PyObject* self, PyObject* value, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return -1;

    if (value == 0)
    {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the autocommit attribute.");
        return -1;
    }

    uintptr_t nAutoCommit = PyObject_IsTrue(value) ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLSetConnectAttr(cnxn->hdbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)nAutoCommit, SQL_IS_UINTEGER);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle(cnxn, "SQLSetConnectAttr", cnxn->hdbc, SQL_NULL_HANDLE);
        return -1;
    }

    cnxn->nAutoCommit = nAutoCommit;

    return 0;
}


static PyObject* Connection_getclosed(PyObject* self, void* closure)
{
    UNUSED(closure);
    Connection* cnxn;

    if (self == 0 || !Connection_Check(self))
    {
        PyErr_SetString(PyExc_TypeError, "Connection object required");
        return 0;
    }

    cnxn = (Connection*)self;

    if (cnxn->hdbc == SQL_NULL_HANDLE)
    {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}


static PyObject* Connection_getsearchescape(PyObject* self, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = (Connection*)self;

    if (!cnxn->searchescape)
    {
        char sz[8] = { 0 };
        SQLSMALLINT cch = 0;

        SQLRETURN ret;
        Py_BEGIN_ALLOW_THREADS
        ret = SQLGetInfo(cnxn->hdbc, SQL_SEARCH_PATTERN_ESCAPE, &sz, _countof(sz), &cch);
        Py_END_ALLOW_THREADS
        if (!SQL_SUCCEEDED(ret))
            return RaiseErrorFromHandle(cnxn, "SQLGetInfo", cnxn->hdbc, SQL_NULL_HANDLE);

        cnxn->searchescape = PyUnicode_FromStringAndSize(sz, (Py_ssize_t)cch);
    }

    Py_INCREF(cnxn->searchescape);
    return cnxn->searchescape;
}

static PyObject* Connection_getmaxwrite(PyObject* self, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    return PyLong_FromSsize_t(cnxn->maxwrite);
}

static int Connection_setmaxwrite(PyObject* self, PyObject* value, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return -1;

    if (value == 0)
    {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the maxwrite attribute.");
        return -1;
    }
    long maxwrite = PyLong_AsLong(value);
    if (PyErr_Occurred())
        return -1;

    Py_ssize_t minval = 255;

    if (maxwrite != 0 && maxwrite < minval)
    {
        PyErr_Format(PyExc_ValueError, "Cannot set maxwrite less than %d unless setting to 0.", (int)minval);
        return -1;
    }

    cnxn->maxwrite = maxwrite;

    return 0;
}


static PyObject* Connection_gettimeout(PyObject* self, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    return PyLong_FromLong(cnxn->timeout);
}

static int Connection_settimeout(PyObject* self, PyObject* value, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return -1;

    if (value == 0)
    {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the timeout attribute.");
        return -1;
    }
    long timeout = PyLong_AsLong(value);
    if (timeout == -1 && PyErr_Occurred())
        return -1;
    if (timeout < 0)
    {
        PyErr_SetString(PyExc_ValueError, "Cannot set a negative timeout.");
        return -1;
    }

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLSetConnectAttr(cnxn->hdbc, SQL_ATTR_CONNECTION_TIMEOUT, (SQLPOINTER)(uintptr_t)timeout, SQL_IS_UINTEGER);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle(cnxn, "SQLSetConnectAttr", cnxn->hdbc, SQL_NULL_HANDLE);
        return -1;
    }

    cnxn->timeout = timeout;

    return 0;
}

static bool _remove_converter(PyObject* self, SQLSMALLINT sqltype)
{
    Connection* cnxn = (Connection*)self;

    if (!cnxn->map_sqltype_to_converter)
    {
        // There are no converters, so nothing to remove.
        return true;
    }

    Object n(PyLong_FromLong(sqltype));
    if (!n.IsValid())
        return false;

    if (!PyDict_Contains(cnxn->map_sqltype_to_converter, n.Get()))
        return true;

    return PyDict_DelItem(cnxn->map_sqltype_to_converter, n.Get()) == 0;
}


static bool _add_converter(PyObject* self, SQLSMALLINT sqltype, PyObject* func)
{
    Connection* cnxn = (Connection*)self;

    if (!cnxn->map_sqltype_to_converter) {
        cnxn->map_sqltype_to_converter = PyDict_New();
        if (!cnxn->map_sqltype_to_converter)
            return false;
    }

    Object n(PyLong_FromLong(sqltype));
    if (!n.IsValid())
        return false;

    return PyDict_SetItem(cnxn->map_sqltype_to_converter, n.Get(), func) != -1;
}

static char conv_add_doc[] =
    "add_output_converter(sqltype, func) --> None\n"
    "\n"
    "Register an output converter function that will be called whenever a value with\n"
    "the given SQL type is read from the database.\n"
    "\n"
    "sqltype\n"
    "  The integer SQL type value to convert, which can be one of the defined\n"
    "  standard constants (e.g. pyodbc.SQL_VARCHAR) or a database-specific value\n"
    "  (e.g. -151 for the SQL Server 2008 geometry data type).\n"
    "\n"
    "func\n"
    "  The converter function which will be called with a single parameter, the\n"
    "  value, and should return the converted value.  If the value is NULL, the\n"
    "  parameter will be None.  Otherwise it will be a "
    "bytes object.\n"
    "\n"
    "If func is None, any existing converter is removed."
    ;

static PyObject* Connection_conv_add(PyObject* self, PyObject* args)
{
    int sqltype;
    PyObject* func;
    if (!PyArg_ParseTuple(args, "iO", &sqltype, &func))
        return 0;

    if (func != Py_None)
    {
        if (!_add_converter(self, (SQLSMALLINT)sqltype, func))
            return 0;
    }
    else
    {
        if (!_remove_converter(self, (SQLSMALLINT)sqltype))
            return 0;
    }

    Py_RETURN_NONE;
}

static char conv_remove_doc[] =
    "remove_output_converter(sqltype) --> None\n"
    "\n"
    "Remove an output converter function that was registered with\n"
    "add_output_converter.  It is safe to call if no converter is\n"
    "registered for the type.\n"
    "\n"
    "sqltype\n"
    "  The integer SQL type value being converted, which can be one of the defined\n"
    "  standard constants (e.g. pyodbc.SQL_VARCHAR) or a database-specific value\n"
    "  (e.g. -151 for the SQL Server 2008 geometry data type).\n"
    ;

static PyObject* Connection_conv_remove(PyObject* self, PyObject* args)
{
    int sqltype;
    if (!PyArg_ParseTuple(args, "i", &sqltype))
        return 0;

    if (!_remove_converter(self, (SQLSMALLINT)sqltype))
        return 0;

    Py_RETURN_NONE;
}

static char conv_get_doc[] =
    "get_output_converter(sqltype) --> <class 'function'>\n"
    "\n"
    "Get the output converter function that was registered with\n"
    "add_output_converter.  It is safe to call if no converter is\n"
    "registered for the type (returns None).\n"
    "\n"
    "sqltype\n"
    "  The integer SQL type value being converted, which can be one of the defined\n"
    "  standard constants (e.g. pyodbc.SQL_VARCHAR) or a database-specific value\n"
    "  (e.g. -151 for the SQL Server 2008 geometry data type).\n"
    ;

PyObject* Connection_GetConverter(Connection* cnxn, SQLSMALLINT type)
{
    // This is our internal function.  It returns a *borrowed* reference to the converter
    // function (so do not deference it).
    //
    // Returns 0 if (1) there is no converter for the type or (2) an error occurred.  You'll
    // need to call PyErr_Occurred to differentiate.

    if (!cnxn->map_sqltype_to_converter) {
        Py_RETURN_NONE;
    }

    Object n(PyLong_FromLong(type));
    if (!n.IsValid())
        return 0;

    return PyDict_GetItem(cnxn->map_sqltype_to_converter, n.Get());
}

static PyObject* Connection_conv_get(PyObject* self, PyObject* args)
{
    int sqltype;
    if (!PyArg_ParseTuple(args, "i", &sqltype))
        return 0;

    Connection* cnxn = (Connection*)self;
    PyObject* func = Connection_GetConverter(cnxn, (SQLSMALLINT)sqltype);

    if (func) {
        Py_INCREF(func);
        return func;
    }

    Py_RETURN_NONE;
}

static void NormalizeCodecName(const char* src, char* dest, size_t cbDest)
{
    // Copies the codec name to dest, lowercasing it and replacing underscores with dashes.
    // (Same as _Py_normalize_encoding which is not public.)  It also wraps the value with
    // pipes so we can search with it.
    //
    // UTF_8 --> |utf-8|
    //
    // This is an internal function - it will truncate so you should use a buffer bigger than
    // anything you expect to search for.

    char* pch = &dest[0];
    char* pchLast = pch + cbDest - 2; // -2 -> leave room for pipe and null

    *pch++ = '|';

    while (*src && pch < pchLast)
    {
        if (isupper(*src))
        {
            *pch++ = (char)tolower(*src++);
        }
        else if (*src == '_')
        {
            *pch++ = '-';
            src++;
        }
        else
        {
            *pch++ = *src++;
        }
    }

    *pch++ = '|';
    *pch = '\0';
}

static bool SetTextEncCommon(TextEnc& enc, const char* encoding, int ctype)
{
    // Code common to setencoding and setdecoding.

    if (!encoding)
    {
        PyErr_Format(PyExc_ValueError, "encoding is required");
        return false;
    }

    // Normalize the names so we don't have to worry about case or dashes vs underscores.
    // We'll lowercase everything and convert underscores to dashes.  The results are then
    // surrounded with pipes so we can search strings.  (See the `strstr` calls below.)
    char lower[30];
    NormalizeCodecName(encoding, lower, sizeof(lower));

    if (!PyCodec_KnownEncoding(encoding))
    {
        PyErr_Format(PyExc_ValueError, "not a registered codec: '%s'", encoding);
        return false;
    }

    if (ctype != 0 && ctype != SQL_WCHAR && ctype != SQL_CHAR)
    {
        PyErr_Format(PyExc_ValueError, "Invalid ctype %d.  Must be SQL_CHAR or SQL_WCHAR", ctype);
        return false;
    }

    char* cpy = StrDup(encoding);
    if (!cpy)
    {
        PyErr_NoMemory();
        return false;
    }

    PyMem_Free((void*)enc.name);
    enc.name = cpy;

    if (strstr("|utf-8|utf8|", lower))
    {
        enc.optenc = OPTENC_UTF8;
        enc.ctype  = (SQLSMALLINT)(ctype ? ctype : SQL_C_CHAR);
    }
    else if (strstr("|utf-16|utf16|", lower))
    {
        enc.optenc = OPTENC_UTF16;
        enc.ctype  = (SQLSMALLINT)(ctype ? ctype : SQL_C_WCHAR);
    }
    else if (strstr("|utf-16-be|utf-16be|utf16be|", lower))
    {
        enc.optenc = OPTENC_UTF16BE;
        enc.ctype  = (SQLSMALLINT)(ctype ? ctype : SQL_C_WCHAR);
    }
    else if (strstr("|utf-16-le|utf-16le|utf16le|", lower))
    {
        enc.optenc = OPTENC_UTF16LE;
        enc.ctype  = (SQLSMALLINT)(ctype ? ctype : SQL_C_WCHAR);
    }
    else if (strstr("|utf-32|utf32|", lower))
    {
        enc.optenc = OPTENC_UTF32;
        enc.ctype  = (SQLSMALLINT)(ctype ? ctype : SQL_C_WCHAR);
    }
    else if (strstr("|utf-32-be|utf-32be|utf32be|", lower))
    {
        enc.optenc = OPTENC_UTF32BE;
        enc.ctype  = (SQLSMALLINT)(ctype ? ctype : SQL_C_WCHAR);
    }
    else if (strstr("|utf-32-le|utf-32le|utf32le|", lower))
    {
        enc.optenc = OPTENC_UTF32LE;
        enc.ctype  = (SQLSMALLINT)(ctype ? ctype : SQL_C_WCHAR);
    }
    else if (strstr("|latin-1|latin1|iso-8859-1|iso8859-1|", lower))
    {
        enc.optenc = OPTENC_LATIN1;
        enc.ctype  = (SQLSMALLINT)(ctype ? ctype : SQL_C_CHAR);
    }
    else
    {
        enc.optenc = OPTENC_NONE;
        enc.ctype  = SQL_C_CHAR;
    }

    return true;
}

static PyObject* Connection_setencoding(PyObject* self, PyObject* args, PyObject* kwargs)
{
    Connection* cnxn = (Connection*)self;

    // In Python 3 we only support encodings for Unicode text.
    char* encoding = 0;
    int ctype = 0;
    static char *kwlist[] = { "encoding", "ctype", 0 };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|si", kwlist, &encoding, &ctype))
        return 0;
    TextEnc& enc = cnxn->unicode_enc;

    if (!SetTextEncCommon(enc, encoding, ctype))
        return 0;

    Py_RETURN_NONE;
}

static char setdecoding_doc[] =
    "setdecoding(sqltype, encoding=None, ctype=None) --> None\n"
    "\n"
    "Configures how text of type `ctype` (SQL_CHAR or SQL_WCHAR) is decoded\n"
    "when read from the database.\n"
    "\n"
    "When reading, the database will assign one of the sqltypes to text columns.\n"
    "pyodbc uses this lookup the decoding information set by this function.\n"
    "sqltype: pyodbc.SQL_CHAR or pyodbc.SQL_WCHAR\n\n"
    "encoding: A registered Python encoding such as \"utf-8\".\n\n"
    "ctype: The C data type should be requested.  Set this to SQL_CHAR for\n"
    "  single-byte encodings like UTF-8 and to SQL_WCHAR for two-byte encodings\n"
    "  like UTF-16.";


static PyObject* Connection_setdecoding(PyObject* self, PyObject* args, PyObject* kwargs)
{
    Connection* cnxn = (Connection*)self;

    int sqltype;
    char* encoding = 0;
    int ctype = 0;

    static char *kwlist[] = {"sqltype", "encoding", "ctype", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i|si", kwlist, &sqltype, &encoding, &ctype))
        return 0;

    if (sqltype != SQL_WCHAR && sqltype != SQL_CHAR && sqltype != SQL_WMETADATA)
        return PyErr_Format(PyExc_ValueError, "Invalid sqltype %d.  Must be SQL_CHAR or SQL_WCHAR or SQL_WMETADATA", sqltype);

    TextEnc& enc = (sqltype == SQL_CHAR) ? cnxn->sqlchar_enc :
        ((sqltype == SQL_WMETADATA) ? cnxn->metadata_enc : cnxn->sqlwchar_enc);

    if (!SetTextEncCommon(enc, encoding, ctype))
        return 0;

    Py_RETURN_NONE;
}


// ColumnInfo struct for BCP binding
struct ColumnInfo {
    SQLSMALLINT sql_type;
    SQLULEN col_size;
    SQLLEN term_len;
    void* buffer;
    SQLLEN* indicator;
    PyObject** converted; // Store converted Python objects (batch_size rows)
    PyTypeObject* py_type; // Expected Python type for validation
};

// Helper function to clean up columns
static void cleanup_columns(Connection* self)
{
    if (self->columns) {
        for (Py_ssize_t i = 0; i < self->num_cols; i++) {
            for (Py_ssize_t r = 0; r < self->batch_size; r++)
                Py_CLEAR(self->columns[i].converted[r]);
            free(self->columns[i].buffer);
            free(self->columns[i].indicator);
            free(self->columns[i].converted);
            Py_CLEAR(self->columns[i].py_type);
        }
        free(self->columns);
        self->columns = 0;
        self->num_cols = 0;
        self->batch_size = 0;
    }
}

// Helper function to load BCP function pointers
static int load_bcp_functions(Connection* self)
{
    HMODULE hmod = GetModuleHandle(NULL); // Assume ODBC driver is already loaded
    self->bcp_init = (bcp_init_t)GetProcAddress(hmod, "bcp_init");
    self->bcp_bind = (bcp_bind_t)GetProcAddress(hmod, "bcp_bind");
    self->bcp_sendrow = (bcp_sendrow_t)GetProcAddress(hmod, "bcp_sendrow");
    self->bcp_batch = (bcp_batch_t)GetProcAddress(hmod, "bcp_batch");
    self->bcp_done = (bcp_done_t)GetProcAddress(hmod, "bcp_done");

    return self->bcp_init && self->bcp_bind && self->bcp_sendrow && self->bcp_batch && self->bcp_done;
}


static PyObject* Connection_bcp_init(PyObject* self, PyObject* args, PyObject* kwargs) {
    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    const char* table = 0;
    const char* file = 0;
    int direction = DB_IN;
    int hint = 0;

    if (!PyArg_ParseTuple(args, "s|sii", &table, &file, &direction, &hint))
        return 0;

    cleanup_columns(self);

    int ret = self->bcp_init(cnxn->hdbc,
                           (SQLWCHAR*)TextBuffer_FromUTF8(table),
                           file ? (SQLWCHAR*)TextBuffer_FromUTF8(file) : 0,
                           direction,
                           hint);

    if (ret != SUCCEED)
        return RaiseErrorFromHandle(cnxn, "bcp_init", cnxn->hdbc, 0);

    Py_RETURN_NONE;
}

// Bind columns for BCP
static PyObject* Connection_bcp_bind(Connection* self, PyObject* args)
{
    PyObject* sample_row = 0;
    Py_ssize_t batch_size = 1000; // Default batch size

    if (!PyArg_ParseTuple(args, "O|n", &sample_row, &batch_size))
        return 0;

    if (!self->bcp_initialized) {
        PyErr_SetString(PyExc_ValueError, "BCP not initialized. Call bcp_init first.");
        return 0;
    }

    if (!PyTuple_Check(sample_row)) {
        PyErr_SetString(PyExc_TypeError, "Sample row must be a tuple");
        return 0;
    }

    if (batch_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "Batch size must be positive");
        return 0;
    }

    Py_ssize_t num_cols = PyTuple_Size(sample_row);
    if (num_cols == 0) {
        PyErr_SetString(PyExc_ValueError, "Empty sample row");
        return 0;
    }

    cleanup_columns(self);

    self->columns = (ColumnInfo*)calloc(num_cols, sizeof(ColumnInfo));
    if (!self->columns) {
        PyErr_SetString(PyExc_MemoryError, "Failed to allocate column info");
        return 0;
    }
    self->num_cols = num_cols;
    self->batch_size = batch_size;

    for (Py_ssize_t col = 0; col < num_cols; col++) {
        PyObject* value = PyTuple_GetItem(sample_row, col);
        SQLSMALLINT decimal_digits;
        SQLSMALLINT sql_c_type;
        self->columns[col].converted = (PyObject**)calloc(batch_size, sizeof(PyObject*));
        if (!self->columns[col].converted) {
            cleanup_columns(self);
            PyErr_SetString(PyExc_MemoryError, "Failed to allocate converted array");
            return 0;
        }

        self->columns[col].sql_type = DetectCType(value, &self->columns[col].col_size, &decimal_digits, &sql_c_type, &self->columns[col].converted[0]);
        if (self->columns[col].sql_type == SQL_UNKNOWN_TYPE) {
            cleanup_columns(self);
            PyErr_SetString(PyExc_ValueError, "Unsupported column type");
            return 0;
        }

        self->columns[col].py_type = PyObject_Type(value);
        if (!self->columns[col].py_type) {
            cleanup_columns(self);
            return 0;
        }
        Py_INCREF(self->columns[col].py_type);

        self->columns[col].term_len = (self->columns[col].sql_type == SQLCHAR) ? SQL_NTS : 0;
        self->columns[col].indicator = (SQLLEN*)calloc(batch_size, sizeof(SQLLEN));
        if (!self->columns[col].indicator) {
            cleanup_columns(self);
            PyErr_SetString(PyExc_MemoryError, "Failed to allocate indicator array");
            return 0;
        }

        switch (self->columns[col].sql_type) {
            case SQLCHAR:
                self->columns[col].buffer = calloc(batch_size, self->columns[col].col_size + 1);
                break;
            case SQLINT:
                self->columns[col].buffer = calloc(batch_size, sizeof(SQLINTEGER));
                break;
            case SQLFLT8:
                self->columns[col].buffer = calloc(batch_size, sizeof(SQLDOUBLE));
                break;
            case SQLDATETIME:
                self->columns[col].buffer = calloc(batch_size, sizeof(SQL_TIMESTAMP_STRUCT));
                break;
            default:
                cleanup_columns(self);
                PyErr_SetString(PyExc_ValueError, "Unsupported SQL type in buffer allocation");
                return 0;
        }

        if (!self->columns[col].buffer) {
            cleanup_columns(self);
            PyErr_SetString(PyExc_MemoryError, "Failed to allocate column buffer");
            return 0;
        }

        RETCODE rc = self->bcp_bind(self->hdbc, self->columns[col].buffer, 0, self->columns[col].term_len,
                                    self->columns[col].indicator, sizeof(SQLLEN), self->columns[col].sql_type, col + 1);
        if (!SQL_SUCCEEDED(rc)) {
            cleanup_columns(self);
            RaiseErrorFromHandle(self, "bcp_bind", 0, self->hdbc, 0);
            return 0;
        }
    }

    for (Py_ssize_t col = 0; col < num_cols; col++) {
        Py_CLEAR(self->columns[col].converted[0]);
    }

    Py_RETURN_NONE;
}

// Send a single row or chunk of rows for BCP
static PyObject* Connection_bcp_sendrow(Connection* self, PyObject* args)
{
    PyObject* data = 0;

    if (!PyArg_ParseTuple(args, "O", &data))
        return 0;

    if (!self->bcp_initialized) {
        PyErr_SetString(PyExc_ValueError, "BCP not initialized. Call bcp_init first.");
        return 0;
    }

    if (!self->columns || self->num_cols == 0) {
        PyErr_SetString(PyExc_ValueError, "Columns not bound. Call bcp_bind first.");
        return 0;
    }

    PyObject* rows = NULL;
    Py_ssize_t num_rows;

    if (PyTuple_Check(data)) {
        rows = PyList_New(1);
        if (!rows) return 0;
        PyList_SET_ITEM(rows, 0, data);
        Py_INCREF(data);
        num_rows = 1;
    } else if (PyList_Check(data)) {
        rows = data;
        Py_INCREF(rows);
        num_rows = PyList_Size(data);
    } else {
        PyErr_SetString(PyExc_TypeError, "Data must be a tuple or list");
        return 0;
    }

    if (num_rows == 0) {
        Py_DECREF(rows);
        PyErr_SetString(PyExc_ValueError, "Empty data");
        return 0;
    }

    if (num_rows > self->batch_size) {
        Py_DECREF(rows);
        PyErr_Format(PyExc_ValueError, "Number of rows (%zd) exceeds batch size (%zd)", num_rows, self->batch_size);
        return 0;
    }

    for (Py_ssize_t row = 0; row < num_rows; row++) {
        PyObject* row_tuple = PyList_GetItem(rows, row);
        if (!PyTuple_Check(row_tuple) || PyTuple_Size(row_tuple) != self->num_cols) {
            for (Py_ssize_t col = 0; col < self->num_cols; col++) {
                for (Py_ssize_t r = 0; r < row; r++)
                    Py_CLEAR(self->columns[col].converted[r]);
            }
            Py_DECREF(rows);
            PyErr_SetString(PyExc_ValueError, "Invalid row data");
            return 0;
        }

        for (Py_ssize_t col = 0; col < self->num_cols; col++) {
            PyObject* value = PyTuple_GetItem(row_tuple, col);
            void* buffer = self->columns[col].buffer;
            SQLLEN* indicator = self->columns[col].indicator;

            if (value == Py_None) {
                indicator[row] = SQL_NULL_DATA;
                Py_CLEAR(self->columns[col].converted[row]);
                continue;
            }

            if (!PyObject_IsInstance(value, (PyObject*)self->columns[col].py_type)) {
                for (Py_ssize_t c = 0; c < self->num_cols; c++) {
                    for (Py_ssize_t r = 0; r <= row; r++)
                        Py_CLEAR(self->columns[c].converted[r]);
                }
                Py_DECREF(rows);
                PyErr_SetString(PyExc_TypeError, "Inconsistent column type in row data");
                return 0;
            }

            self->columns[col].converted[row] = 0;
            SQLULEN col_size;
            SQLSMALLINT decimal_digits;
            SQLSMALLINT sql_c_type;
            SQLSMALLINT sql_type = DetectCType(value, &col_size, &decimal_digits, &sql_c_type, &self->columns[col].converted[row]);
            if (sql_type != self->columns[col].sql_type) {
                for (Py_ssize_t c = 0; c < self->num_cols; c++) {
                    for (Py_ssize_t r = 0; r <= row; r++)
                        Py_CLEAR(self->columns[c].converted[r]);
                }
                Py_DECREF(rows);
                PyErr_SetString(PyExc_TypeError, "Inconsistent SQL type in row data");
                return 0;
            }

            switch (self->columns[col].sql_type) {
                case SQLCHAR: {
                    char* str_value = PyUnicode_AsUTF8(self->columns[col].converted[row] ? self->columns[col].converted[row] : value);
                    if (!str_value) {
                        for (Py_ssize_t c = 0; c < self->num_cols; c++) {
                            for (Py_ssize_t r = 0; r <= row; r++)
                                Py_CLEAR(self->columns[c].converted[r]);
                        }
                        Py_DECREF(rows);
                        return 0;
                    }
                    strncpy((char*)buffer + row * (self->columns[col].col_size + 1), str_value, self->columns[col].col_size);
                    indicator[row] = SQL_NTS;
                    break;
                }
                case SQLINT: {
                    ((SQLINTEGER*)buffer)[row] = (SQLINTEGER)PyLong_AsLong(value);
                    indicator[row] = sizeof(SQLINTEGER);
                    break;
                }
                case SQLFLT8: {
                    ((SQLDOUBLE*)buffer)[row] = PyFloat_AsDouble(value);
                    indicator[row] = sizeof(SQLDOUBLE);
                    break;
                }
                case SQLDATETIME: {
                    SQL_TIMESTAMP_STRUCT* ts = &((SQL_TIMESTAMP_STRUCT*)buffer)[row];
                    ts->year = PyDateTime_GET_YEAR(value);
                    ts->month = PyDateTime_GET_MONTH(value);
                    ts->day = PyDateTime_GET_DAY(value);
                    ts->hour = PyDateTime_DATE_GET_HOUR(value);
                    ts->minute = PyDateTime_DATE_GET_MINUTE(value);
                    ts->second = PyDateTime_DATE_GET_SECOND(value);
                    ts->fraction = PyDateTime_DATE_GET_MICROSECOND(value) * 1000;
                    indicator[row] = sizeof(SQL_TIMESTAMP_STRUCT);
                    break;
                }
            }
        }

        RETCODE rc = self->bcp_sendrow(self->hdbc);
        if (!SQL_SUCCEEDED(rc)) {
            for (Py_ssize_t col = 0; col < self->num_cols; col++) {
                for (Py_ssize_t r = 0; r <= row; r++)
                    Py_CLEAR(self->columns[col].converted[r]);
            }
            Py_DECREF(rows);
            RaiseErrorFromHandle(self, "bcp_sendrow", 0, self->hdbc, 0);
            return 0;
        }
    }

    for (Py_ssize_t col = 0; col < self->num_cols; col++) {
        for (Py_ssize_t r = 0; r < num_rows; r++)
            Py_CLEAR(self->columns[col].converted[r]);
    }

    Py_DECREF(rows);
    Py_RETURN_NONE;
}

// Commit a batch of rows
static PyObject* Connection_bcp_batch(Connection* self)
{
    if (!self->bcp_initialized) {
        PyErr_SetString(PyExc_ValueError, "BCP not initialized");
        return 0;
    }

    DBINT rows_committed = self->bcp_batch(self->hdbc);
    if (rows_committed == -1) {
        RaiseErrorFromHandle(self, "bcp_batch", 0, self->hdbc, 0);
        return 0;
    }

    return PyLong_FromLong((long)rows_committed);
}

// Finalize BCP operation
static PyObject* Connection_bcp_done(Connection* self)
{
    if (!self->bcp_initialized) {
        PyErr_SetString(PyExc_ValueError, "BCP not initialized");
        return 0;
    }

    cleanup_columns(self);

    DBINT rows_processed = self->bcp_done(self->hdbc);
    if (rows_processed == -1) {
        RaiseErrorFromHandle(self, "bcp_done", 0, self->hdbc, 0);
        return 0;
    }

    self->bcp_initialized = FALSE;
    return PyLong_FromLong((long)rows_processed);
}

static char enter_doc[] = "__enter__() -> self.";
static PyObject* Connection_enter(PyObject* self, PyObject* args)
{
    UNUSED(args);
    Py_INCREF(self);
    return self;
}

static char exit_doc[] = "__exit__(*excinfo) -> None.  Commits the connection if necessary.";
static PyObject* Connection_exit(PyObject* self, PyObject* args)
{
    Connection* cnxn = (Connection*)self;

    // If an error has occurred, `args` will be a tuple of 3 values.  Otherwise it will be a tuple of 3 `None`s.
    assert(PyTuple_Check(args));

    if (cnxn->nAutoCommit == SQL_AUTOCOMMIT_OFF)
    {
        SQLSMALLINT CompletionType = (PyTuple_GetItem(args, 0) == Py_None) ? SQL_COMMIT : SQL_ROLLBACK;
        SQLRETURN ret;
        Py_BEGIN_ALLOW_THREADS
        ret = SQLEndTran(SQL_HANDLE_DBC, cnxn->hdbc, CompletionType);
        Py_END_ALLOW_THREADS

        if (!SQL_SUCCEEDED(ret))
        {
            const char* szFunc = (CompletionType == SQL_COMMIT) ? "SQLEndTran(SQL_COMMIT)" : "SQLEndTran(SQL_ROLLBACK)";
            return RaiseErrorFromHandle(cnxn, szFunc, cnxn->hdbc, SQL_NULL_HANDLE);
        }
    }

    Py_RETURN_NONE;
}


static struct PyMethodDef Connection_methods[] =
{
    { "cursor",                  Connection_cursor,          METH_NOARGS,  cursor_doc     },
    { "close",                   Connection_close,           METH_NOARGS,  close_doc      },
    { "execute",                 Connection_execute,         METH_VARARGS, execute_doc    },
    { "commit",                  Connection_commit,          METH_NOARGS,  commit_doc     },
    { "rollback",                Connection_rollback,        METH_NOARGS,  rollback_doc   },
    { "getinfo",                 Connection_getinfo,         METH_VARARGS, getinfo_doc    },
    { "add_output_converter",    Connection_conv_add,        METH_VARARGS, conv_add_doc   },
    { "remove_output_converter", Connection_conv_remove,     METH_VARARGS, conv_remove_doc },
    { "get_output_converter",    Connection_conv_get,        METH_VARARGS, conv_get_doc },
    { "clear_output_converters", Connection_conv_clear,      METH_NOARGS,  conv_clear_doc },
    { "setdecoding",             (PyCFunction)Connection_setdecoding,     METH_VARARGS|METH_KEYWORDS, setdecoding_doc },
    { "setencoding",             (PyCFunction)Connection_setencoding,     METH_VARARGS|METH_KEYWORDS, 0 },
    { "set_attr",                Connection_set_attr,        METH_VARARGS, set_attr_doc   },
    { "__enter__",               Connection_enter,           METH_NOARGS,  enter_doc      },
    { "__exit__",                Connection_exit,            METH_VARARGS, exit_doc       },
    {"bcp_init", (PyCFunction)Connection_bcp_init, METH_VARARGS, "Initialize BCP operation for a table"},
    {"bcp_bind", (PyCFunction)Connection_bcp_bind, METH_VARARGS, "Bind columns for bulk copy with optional batch size"},
    {"bcp_sendrow", (PyCFunction)Connection_bcp_sendrow, METH_VARARGS, "Send a single row or chunk of rows for bulk copy"},
    {"bcp_batch", (PyCFunction)Connection_bcp_batch, METH_NOARGS, "Commit a batch of rows and return rows committed"},
    {"bcp_done", (PyCFunction)Connection_bcp_done, METH_NOARGS, "Finalize BCP operation and return rows processed"},

    { 0, 0, 0, 0 }
};

static PyGetSetDef Connection_getseters[] = {
    { "closed", (getter)Connection_getclosed, 0,
      "Returns True if the connection is closed; False otherwise.", 0},
    { "searchescape", (getter)Connection_getsearchescape, 0,
        "The ODBC search pattern escape character, as returned by\n"
        "SQLGetInfo(SQL_SEARCH_PATTERN_ESCAPE).  These are driver specific.", 0 },
    { "autocommit", Connection_getautocommit, Connection_setautocommit,
      "Returns True if the connection is in autocommit mode; False otherwise.", 0 },
    { "timeout", Connection_gettimeout, Connection_settimeout,
      "The timeout in seconds, zero means no timeout.", 0 },
    { "maxwrite", Connection_getmaxwrite, Connection_setmaxwrite, "The maximum bytes to write before using SQLPutData.", 0 },
    { 0 }
};

PyTypeObject ConnectionType =
{
    PyVarObject_HEAD_INIT(0, 0)
    "pyodbc.Connection",        // tp_name
    sizeof(Connection),         // tp_basicsize
    0,                          // tp_itemsize
    Connection_dealloc,         // destructor tp_dealloc
    0,                          // tp_print
    0,                          // tp_getattr
    0,                          // tp_setattr
    0,                          // tp_compare
    0,                          // tp_repr
    0,                          // tp_as_number
    0,                          // tp_as_sequence
    0,                          // tp_as_mapping
    0,                          // tp_hash
    0,                          // tp_call
    0,                          // tp_str
    0,                          // tp_getattro
    0,                          // tp_setattro
    0,                          // tp_as_buffer
    Py_TPFLAGS_DEFAULT,         // tp_flags
    connection_doc,             // tp_doc
    0,                          // tp_traverse
    0,                          // tp_clear
    0,                          // tp_richcompare
    0,                          // tp_weaklistoffset
    0,                          // tp_iter
    0,                          // tp_iternext
    Connection_methods,         // tp_methods
    0,                          // tp_members
    Connection_getseters,       // tp_getset
    0,                          // tp_base
    0,                          // tp_dict
    0,                          // tp_descr_get
    0,                          // tp_descr_set
    0,                          // tp_dictoffset
    0,                          // tp_init
    0,                          // tp_alloc
    0,                          // tp_new
    0,                          // tp_free
    0,                          // tp_is_gc
    0,                          // tp_bases
    0,                          // tp_mro
    0,                          // tp_cache
    0,                          // tp_subclasses
    0,                          // tp_weaklist
};
