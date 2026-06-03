// The SQL Server bulk copy (BCP) fast path for Cursor.executemany.
//
// When cursor.fast_executemany and cursor.use_bcp_fast are set, an INSERT of a single
// parameterized VALUES row is carried out with the BCP API instead of an array of parameter
// sets.  The path is an optimisation only: it must store exactly what the plain fast path would,
// so anything it cannot reproduce faithfully is detected before bcp_init and left to that path.
// Once rows are streaming, a failure rolls back everything this call sent and raises.

#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "pyodbcmodule.h"
#include "connection.h"
#include "cursor.h"
#include "errors.h"
#include "bcp_support.h"
#include <datetime.h>

void BcpExecuteMany_init()
{
    PyDateTime_IMPORT;
}

/** The classes of Python value a column can hold, as bits so a column records all it has seen. */
enum : unsigned
{
    VAL_NONE     = 1u << 0,
    VAL_BOOL     = 1u << 1,
    VAL_INT      = 1u << 2,
    VAL_FLOAT    = 1u << 3,
    VAL_STR      = 1u << 4,
    VAL_BYTES    = 1u << 5,
    VAL_DECIMAL  = 1u << 6,
    VAL_DATE     = 1u << 7,
    VAL_DATETIME = 1u << 8,
    VAL_TIME     = 1u << 9,
    VAL_UUID     = 1u << 10,
};

/** Destination column categories, by how values must be prepared for them. */
enum Destination
{
    DEST_UNSUPPORTED,
    DEST_CHAR,
    DEST_XML,
    DEST_BINARY,
    DEST_INTEGER,
    DEST_BIT,
    DEST_FLOAT,
    DEST_DECIMAL,
    DEST_MONEY,
    DEST_DATE,
    DEST_TIME,
    DEST_DATETIME2,
    DEST_DATETIMEOFFSET,
    DEST_DATETIME,
    DEST_SMALLDATETIME,
    DEST_GUID,
};

/**
 * One bound column: where it goes, what the rows hold for it, and its capacity in characters
 * (text) or bytes (binary), or -1 where no limit applies.
 */
struct ColumnPlan
{
    const BcpTargetColumn* target;
    int ordinal;
    int destination;
    long capacity;
    unsigned classes;
    PyTypeObject* lastType;
    unsigned lastClass;
};

/**
 * How the load runs inside a transaction: in autocommit mode autocommit is suspended so the
 * load is its own transaction; in manual-commit mode a savepoint marks where it started.
 */
struct LoadTransaction
{
    bool autocommitSuspended;
    bool savepoint;
};

/**
 * Everything a load allocates, freed when it goes out of scope.
 *
 * `cols` holds the ncols columns the rows' values go to, then nfill filler columns: the table
 * columns after the last of those, which BCP requires bound and which are always NULL so the
 * server fills them in.  keepNulls says whether BCPKEEPNULLS is set.
 */
struct BcpLoad
{
    Connection* cnxn;
    BcpInsert insert;
    bool insertParsed;
    BcpTarget target;
    ColumnPlan* plans;
    BcpCol* cols;
    int ncols;
    int nfill;
    bool keepNulls;
    Object rows;
    Object decimalFormat;
    Object normalizeName;

    explicit BcpLoad(Connection* c)
        : cnxn(c), insertParsed(false), plans(0), cols(0), ncols(0), nfill(0), keepNulls(true)
    {
        target.ncols = 0;
        target.cols = 0;
    }

    BcpLoad(const BcpLoad&) = delete;
    BcpLoad& operator=(const BcpLoad&) = delete;

    ~BcpLoad()
    {
        if (insertParsed)
            BcpFreeInsert(&insert);
        BcpFreeTarget(&target);
        PyMem_Free(plans);
        BcpFreeColumns(cols, ncols + nfill);
    }
};

/**
 * The category of a destination column, or DEST_UNSUPPORTED for one the path leaves to the
 * plain path.
 */
static int destination_of(const BcpTargetColumn& col)
{
    switch (col.systemType) {
    case 35: case 99: case 167: case 175: case 231: case 239:
        return DEST_CHAR;
    case 241:
        return DEST_XML;
    case 34: case 165: case 173:
        return DEST_BINARY;
    case 48: case 52: case 56: case 127:
        return DEST_INTEGER;
    case 104:
        return DEST_BIT;
    case 59: case 62:
        return DEST_FLOAT;
    case 106: case 108:
        return DEST_DECIMAL;
    case 60: case 122:
        return DEST_MONEY;
    case 40:
        return DEST_DATE;
    case 41:
        return DEST_TIME;
    case 42:
        return DEST_DATETIME2;
    case 43:
        return DEST_DATETIMEOFFSET;
    case 61:
        return DEST_DATETIME;
    case 58:
        return DEST_SMALLDATETIME;
    case 36:
        return DEST_GUID;
    }
    return DEST_UNSUPPORTED;
}

/**
 * The length limit the plain path enforces before sending, in UTF-16 units for char, varchar,
 * nchar and nvarchar and in bytes for binary and varbinary; -1 for (max), xml and the legacy
 * text, ntext and image types, which have none that matters.
 */
static long capacity_of(const BcpTargetColumn& col)
{
    if (col.maxLength == -1)
        return -1;
    switch (col.systemType) {
    case 167: case 175: case 165: case 173:
        return col.maxLength;
    case 231: case 239:
        return col.maxLength / 2;
    }
    return -1;
}

static bool names_equal(const SQLWCHAR* a, const SQLWCHAR* b, bool foldAsciiCase)
{
    for (; *a && *b; ++a, ++b) {
        SQLWCHAR ca = *a, cb = *b;
        if (foldAsciiCase) {
            if (ca >= 'A' && ca <= 'Z') ca = (SQLWCHAR)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (SQLWCHAR)(cb + 32);
        }
        if (ca != cb)
            return false;
    }
    return *a == 0 && *b == 0;
}

/**
 * Finds the target column a listed name refers to: an exact match, else, in a database that
 * compares identifiers ignoring case, the only match that differs in ASCII case.  A name that
 * matches only by folding other letters is left unmatched, so the plain path resolves it.
 * Returns -1 if there is none, -2 on allocation failure.
 */
static int find_target_column(const BcpTarget& target, const char* name)
{
    SQLWCHAR* wname = BcpUtf8ToSqlWChar(name);
    if (!wname)
        return -2;
    int exact = -1, folded = -1, nfolded = 0;
    for (int i = 0; i < target.ncols && exact < 0; ++i) {
        if (names_equal(target.cols[i].name, wname, false))
            exact = i;
        else if (target.caseInsensitive && names_equal(target.cols[i].name, wname, true)) {
            folded = i;
            ++nfolded;
        }
    }
    PyMem_Free(wname);
    if (exact >= 0)
        return exact;
    return nfolded == 1 ? folded : -1;
}

static bool is_bindable(const BcpTargetColumn& col)
{
    return !col.identity && !col.computed && destination_of(col) != DEST_UNSUPPORTED;
}

/**
 * Maps the INSERT's values to target columns as the INSERT itself would.
 *
 * A column list names its columns; the unlisted ones are left for the server to fill in
 * (identity, computed, DEFAULT or NULL) exactly as for the INSERT.  Without a list the values
 * go to every column except identity and computed ones, which is how T-SQL reads a positional
 * INSERT.  Anything the INSERT would reject is left to the plain path to report.
 *
 * Unbound columns before the last bound one need nothing, but BCP requires the columns after it
 * bound, so those become filler columns bound to NULL, which the server fills in the same way
 * (see choose_keep_nulls for columns with a DEFAULT).
 *
 * @return 1 if mapped, 0 to fall back, -1 on allocation failure (Python exception set).
 */
static int map_columns(BcpLoad& load)
{
    const BcpInsert& ins = load.insert;
    const BcpTarget& target = load.target;
    load.plans = (ColumnPlan*)PyMem_Calloc((size_t)ins.nmarkers, sizeof(ColumnPlan));
    if (!load.plans) {
        PyErr_NoMemory();
        return -1;
    }

    int n = 0;
    if (ins.nnames) {
        if (ins.nnames != ins.nmarkers)
            return 0;
        for (int i = 0; i < ins.nnames; ++i) {
            int t = find_target_column(target, ins.names[i]);
            if (t == -2)
                return -1;
            if (t < 0 || !is_bindable(target.cols[t]))
                return 0;
            for (int j = 0; j < n; ++j)
                if (load.plans[j].ordinal == t + 1)
                    return 0;
            load.plans[n].target = &target.cols[t];
            load.plans[n++].ordinal = t + 1;
        }
    } else {
        for (int t = 0; t < target.ncols; ++t) {
            const BcpTargetColumn& col = target.cols[t];
            if (col.identity || col.computed)
                continue;
            if (n == ins.nmarkers || destination_of(col) == DEST_UNSUPPORTED)
                return 0;
            load.plans[n].target = &col;
            load.plans[n++].ordinal = t + 1;
        }
        if (n != ins.nmarkers)
            return 0;
    }
    int last = 0;
    for (int i = 0; i < n; ++i) {
        load.plans[i].destination = destination_of(*load.plans[i].target);
        load.plans[i].capacity = capacity_of(*load.plans[i].target);
        if (load.plans[i].ordinal > last)
            last = load.plans[i].ordinal;
    }
    load.ncols = n;
    load.nfill = target.ncols - last;
    return 1;
}

/**
 * Decides whether to set BCPKEEPNULLS.  With it, a NULL bound to a column with a DEFAULT is
 * stored as NULL, as an INSERT stores an explicit NULL; without it BCP stores the default, as
 * the INSERT does for a column it leaves out.  It only affects columns with a default, so it is
 * cleared when a filler column has one, which is only safe if no value column with a default
 * receives a NULL.
 *
 * @return false to fall back.
 */
static bool choose_keep_nulls(BcpLoad& load)
{
    bool fillerDefault = false;
    for (int t = load.target.ncols - load.nfill; t < load.target.ncols; ++t)
        fillerDefault = fillerDefault || load.target.cols[t].hasDefault;
    if (!fillerDefault)
        return true;
    for (int i = 0; i < load.ncols; ++i)
        if (load.plans[i].target->hasDefault && (load.plans[i].classes & VAL_NONE))
            return false;
    load.keepNulls = false;
    return true;
}

static unsigned class_of(PyObject* cell)
{
    if (cell == Py_None)
        return VAL_NONE;
    if (PyBool_Check(cell))
        return VAL_BOOL;
    if (PyLong_Check(cell)) {
        int overflow = 0;
        PyLong_AsLongLongAndOverflow(cell, &overflow);
        return overflow ? 0 : VAL_INT;
    }
    if (PyFloat_Check(cell))
        return VAL_FLOAT;
    if (PyUnicode_Check(cell))
        return VAL_STR;
    if (PyBytes_Check(cell) || PyByteArray_Check(cell))
        return VAL_BYTES;
    if (PyDateTime_Check(cell))
        return VAL_DATETIME;
    if (PyDate_Check(cell))
        return VAL_DATE;
    if (PyTime_Check(cell))
        return VAL_TIME;
    PyObject* cls = 0;
    if (IsInstanceForThread(cell, "decimal", "Decimal", &cls) && cls) {
        Py_DECREF(cls);
        return VAL_DECIMAL;
    }
    PyErr_Clear();
    if (IsInstanceForThread(cell, "uuid", "UUID", &cls) && cls) {
        Py_DECREF(cls);
        return VAL_UUID;
    }
    PyErr_Clear();
    return 0;
}

/** Classifies a cell, reusing the column's last class when the type repeats (ints are range-checked each time). */
static unsigned classify(ColumnPlan& plan, PyObject* cell)
{
    if (Py_TYPE(cell) == plan.lastType && plan.lastClass != VAL_INT)
        return plan.lastClass;
    plan.lastType = Py_TYPE(cell);
    plan.lastClass = class_of(cell);
    return plan.lastClass;
}

static bool is_row(PyObject* row, int ncols)
{
    return (PyTuple_Check(row) || PyList_Check(row)) && PySequence_Fast_GET_SIZE(row) == ncols;
}

/** The length of a str in UTF-16 code units, as the plain path measures it. */
static Py_ssize_t utf16_length(PyObject* s)
{
    const Py_ssize_t n = PyUnicode_GET_LENGTH(s);
    if (PyUnicode_KIND(s) != PyUnicode_4BYTE_KIND)
        return n;
    const Py_UCS4* data = PyUnicode_4BYTE_DATA(s);
    Py_ssize_t units = n;
    for (Py_ssize_t i = 0; i < n; ++i)
        units += data[i] > 0xFFFF;
    return units;
}

/** The number of characters an int (already known to fit 64 bits) takes as decimal text. */
static long int_text_length(PyObject* cell)
{
    long long v = PyLong_AsLongLong(cell);
    long n = v < 0 ? 2 : 1;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    while (u >= 10) {
        u /= 10;
        ++n;
    }
    return n;
}

/**
 * Returns false where a value's length means the plain path would not store what BCP would:
 * text or bytes longer than the column, which the plain path rejects but the driver quietly
 * trims of trailing spaces or zero bytes, and a number wider as text than its text column,
 * which the plain path truncates but BCP rejects.  A double can take 24 characters as text.
 */
static bool fits_capacity(const ColumnPlan& plan, PyObject* cell, unsigned cls)
{
    switch (cls) {
    case VAL_STR:
        return plan.destination != DEST_CHAR || utf16_length(cell) <= plan.capacity;
    case VAL_BYTES:
        return plan.destination != DEST_BINARY ||
               (PyBytes_Check(cell) ? PyBytes_GET_SIZE(cell) : PyByteArray_GET_SIZE(cell)) <= plan.capacity;
    case VAL_INT:
        return plan.destination != DEST_CHAR || int_text_length(cell) <= plan.capacity;
    case VAL_FLOAT:
        return plan.destination != DEST_CHAR || plan.capacity >= 24;
    }
    return true;
}

static int hex_digit(Py_UCS1 c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/**
 * Parses a str in the one form the plain path stores in a uniqueidentifier column,
 * xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx in either case, into the column's byte order (that of
 * uuid.UUID.bytes_le).  The plain path rejects every other form (braces, no hyphens, padding),
 * so those are left to it.  `guid` may be null to only check the form.
 */
static bool parse_guid_text(PyObject* s, unsigned char* guid)
{
    static const int byteOrder[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    if (PyUnicode_KIND(s) != PyUnicode_1BYTE_KIND || PyUnicode_GET_LENGTH(s) != 36)
        return false;
    const Py_UCS1* text = PyUnicode_1BYTE_DATA(s);
    unsigned char bytes[16];
    int n = 0;
    for (int i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i++] != '-')
                return false;
            continue;
        }
        const int high = hex_digit(text[i]), low = hex_digit(text[i + 1]);
        if (high < 0 || low < 0)
            return false;
        bytes[n++] = (unsigned char)(high << 4 | low);
        i += 2;
    }
    if (guid)
        for (int k = 0; k < 16; ++k)
            guid[k] = bytes[byteOrder[k]];
    return true;
}

static bool is_two_digits_below(const Py_UCS1* text, int limit)
{
    return text[0] >= '0' && text[0] <= '9' && text[1] >= '0' && text[1] <= '9' &&
           (text[0] - '0') * 10 + (text[1] - '0') < limit;
}

/**
 * Whether a str is time text the plain path stores exactly in a time column of the given scale
 * and the driver's BCP conversion accepts as is: HH:MM:SS with an optional fraction of no more
 * digits than the scale, the form str(datetime.time) takes and SQLAlchemy binds.  The plain path
 * rejects a longer fraction, which does not fit the column's size, and out-of-range fields.
 */
static bool is_time_text(PyObject* s, int scale)
{
    if (PyUnicode_KIND(s) != PyUnicode_1BYTE_KIND)
        return false;
    const Py_ssize_t n = PyUnicode_GET_LENGTH(s);
    const Py_UCS1* text = PyUnicode_1BYTE_DATA(s);
    if (n < 8 || text[2] != ':' || text[5] != ':' || !is_two_digits_below(text, 24) ||
        !is_two_digits_below(text + 3, 60) || !is_two_digits_below(text + 6, 60))
        return false;
    if (n == 8)
        return true;
    if (text[8] != '.' || n == 9 || n - 9 > scale)
        return false;
    for (Py_ssize_t i = 9; i < n; ++i)
        if (text[i] < '0' || text[i] > '9')
            return false;
    return true;
}

/**
 * Whether the plain path stores a datetime in a date column: when its hours, minutes and seconds
 * are zero, ignoring the microseconds and tzinfo.  It rejects any other time of day, and SQLAlchemy
 * binds a date for such a column as a datetime at midnight.
 */
static bool is_storable_as_date(PyObject* cell)
{
    return PyDateTime_DATE_GET_HOUR(cell) == 0 && PyDateTime_DATE_GET_MINUTE(cell) == 0 &&
           PyDateTime_DATE_GET_SECOND(cell) == 0;
}

/**
 * Returns false for a value the plain path rejects in its column, or would store differently,
 * because of its form rather than its type: a str in a uniqueidentifier column that is not a
 * canonical GUID, a str in a time column that is not time text (see is_time_text), or a datetime
 * with a time of day in a date column.
 */
static bool has_plain_path_form(const ColumnPlan& plan, PyObject* cell, unsigned cls)
{
    if (plan.destination == DEST_GUID && cls == VAL_STR)
        return parse_guid_text(cell, 0);
    if (plan.destination == DEST_TIME && cls == VAL_STR)
        return is_time_text(cell, plan.target->scale);
    if (plan.destination == DEST_DATE && cls == VAL_DATETIME)
        return is_storable_as_date(cell);
    return true;
}

/**
 * Scans every row once before anything is sent, recording the classes of value in each column.
 *
 * @return false if a row is not a tuple or list of the right length, or holds a value BCP
 *         cannot send as the plain path would (an int beyond 64 bits, an unsupported type, a
 *         value that does not fit its column, see fits_capacity, or one whose form the plain
 *         path rejects, see has_plain_path_form).
 */
static bool scan_rows(BcpLoad& load)
{
    const Py_ssize_t nrows = PyTuple_GET_SIZE(load.rows.Get());
    for (Py_ssize_t r = 0; r < nrows; ++r) {
        PyObject* row = PyTuple_GET_ITEM(load.rows.Get(), r);
        if (!is_row(row, load.ncols))
            return false;
        PyObject** cells = PySequence_Fast_ITEMS(row);
        for (int i = 0; i < load.ncols; ++i) {
            ColumnPlan& plan = load.plans[i];
            unsigned cls = classify(plan, cells[i]);
            if (!cls || (plan.capacity >= 0 && !fits_capacity(plan, cells[i], cls)) ||
                !has_plain_path_form(plan, cells[i], cls))
                return false;
            plan.classes |= cls;
        }
    }
    return true;
}

/**
 * Picks the host type for a column from its destination and the classes of value it holds.
 *
 * Only pairs whose driver conversion stores what the plain fast path stores are allowed (see
 * test_bcp_matches_plain_path); text goes as UTF-16 (SQLNCHAR) because the driver reads
 * SQLCHARACTER data in the client's ANSI code page.  SQLCHARACTER is used only for ASCII text
 * the path formats itself, Decimals and dates and times shaped to the destination's precision,
 * and for time text it has checked (see is_time_text).
 *
 * @return The host type, or 0 to fall back.
 */
static int choose_host_type(int destination, unsigned classes)
{
    const unsigned v = classes & ~VAL_NONE;
    if (!v)
        return SQLCHARACTER;
    const bool onlyIntegers = !(v & ~(VAL_INT | VAL_BOOL));

    switch (destination) {
    case DEST_CHAR:
        if (v == VAL_STR || v == (VAL_STR | VAL_INT))
            return SQLNCHAR;
        if (v == VAL_INT)
            return SQLINT8;
        if (v == VAL_BOOL)
            return SQLBIT;
        if (v == VAL_FLOAT)
            return SQLFLT8;
        return 0;
    case DEST_XML:
        return v == VAL_STR ? SQLNCHAR : 0;
    case DEST_BINARY:
        return v == VAL_BYTES ? SQLBINARY : 0;
    case DEST_INTEGER:
        if (onlyIntegers)
            return SQLINT8;
        if (v == VAL_FLOAT)
            return SQLFLT8;
        if (!(v & ~(VAL_DECIMAL | VAL_INT)))
            return SQLCHARACTER;
        return 0;
    case DEST_BIT:
        if (onlyIntegers)
            return SQLINT8;
        return v == VAL_FLOAT ? SQLFLT8 : 0;
    case DEST_FLOAT:
        if (onlyIntegers)
            return SQLINT8;
        return !(v & ~(VAL_FLOAT | VAL_INT | VAL_BOOL)) ? SQLFLT8 : 0;
    case DEST_DECIMAL:
        if (v == VAL_INT)
            return SQLINT8;
        if (v == VAL_FLOAT)
            return SQLFLT8;
        return !(v & ~(VAL_DECIMAL | VAL_INT)) ? SQLCHARACTER : 0;
    case DEST_MONEY:
        if (v == VAL_INT)
            return SQLINT8;
        return !(v & ~(VAL_DECIMAL | VAL_INT)) ? SQLCHARACTER : 0;
    case DEST_TIME:
        return !(v & ~(VAL_TIME | VAL_DATETIME | VAL_STR)) ? SQLCHARACTER : 0;
    case DEST_DATE:
    case DEST_DATETIME2:
    case DEST_DATETIMEOFFSET:
    case DEST_DATETIME:
    case DEST_SMALLDATETIME:
        return !(v & ~(VAL_DATE | VAL_DATETIME)) ? SQLCHARACTER : 0;
    case DEST_GUID:
        return !(v & ~(VAL_UUID | VAL_STR)) ? SQLUNIQUEID : 0;
    }
    return 0;
}

/**
 * Sets up a column per plan, then the NULL filler columns.  Variable-length columns take a
 * 4-byte length prefix, except (max) columns, which need an 8-byte prefix and, because the
 * driver reads a zero-length 8-byte prefix as 64KB, every length passed to bcp_collen as well.
 *
 * @return 1 if every column has a host type, 0 to fall back, -1 on allocation failure.
 */
static int plan_columns(BcpLoad& load)
{
    load.cols = (BcpCol*)PyMem_Calloc((size_t)(load.ncols + load.nfill), sizeof(BcpCol));
    if (!load.cols) {
        PyErr_NoMemory();
        return -1;
    }
    for (int f = 0; f < load.nfill; ++f) {
        BcpCol* c = &load.cols[load.ncols + f];
        c->ordinal = load.target.ncols - load.nfill + f + 1;
        c->hostType = SQLCHARACTER;
        c->prefix = load.target.cols[c->ordinal - 1].maxLength == -1 ? 8 : 4;
        if (!BcpAllocColumn(c))
            return -1;
        write_le(c->scratch, ~0ULL, c->prefix);
    }
    for (int i = 0; i < load.ncols; ++i) {
        const ColumnPlan& plan = load.plans[i];
        BcpCol* c = &load.cols[i];
        c->ordinal = plan.ordinal;
        c->hostType = choose_host_type(plan.destination, plan.classes);
        if (!c->hostType)
            return 0;
        const bool fixed = c->hostType == SQLBIT || c->hostType == SQLINT8 || c->hostType == SQLFLT8 ||
                           c->hostType == SQLUNIQUEID;
        const bool unlimited = plan.target->maxLength == -1;
        c->prefix = fixed ? 0 : (unlimited ? 8 : 4);
        c->setsCollen = !fixed && unlimited;
        if (!BcpAllocColumn(c))
            return -1;
    }
    return 1;
}

/*=======================================================================================*/
// Dates and times, shaped as the plain fast path stores them
/*=======================================================================================*/

/** A wall-clock date and time; any tzinfo is ignored, as the plain fast path ignores it. */
struct CivilTime
{
    int year, month, day, hour, minute, second, micro;
};

static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, CivilTime& t)
{
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    t.day = (int)(doy - (153 * mp + 2) / 5 + 1);
    t.month = (int)(mp < 10 ? mp + 3 : mp - 9);
    t.year = (int)((long long)yoe + era * 400 + (t.month <= 2));
}

/** Adds up to a day's worth of seconds, carrying into the date. */
static void add_seconds(CivilTime& t, int seconds)
{
    int sod = t.hour * 3600 + t.minute * 60 + t.second + seconds;
    if (sod >= 86400) {
        civil_from_days(days_from_civil(t.year, (unsigned)t.month, (unsigned)t.day) + sod / 86400, t);
        sod %= 86400;
    }
    t.hour = sod / 3600;
    t.minute = (sod / 60) % 60;
    t.second = sod % 60;
}

static CivilTime civil_of(PyObject* cell, unsigned cls)
{
    CivilTime t = {1900, 1, 1, 0, 0, 0, 0};
    if (cls != VAL_TIME) {
        t.year = PyDateTime_GET_YEAR(cell);
        t.month = PyDateTime_GET_MONTH(cell);
        t.day = PyDateTime_GET_DAY(cell);
    }
    if (cls == VAL_DATETIME) {
        t.hour = PyDateTime_DATE_GET_HOUR(cell);
        t.minute = PyDateTime_DATE_GET_MINUTE(cell);
        t.second = PyDateTime_DATE_GET_SECOND(cell);
        t.micro = PyDateTime_DATE_GET_MICROSECOND(cell);
    } else if (cls == VAL_TIME) {
        t.hour = PyDateTime_TIME_GET_HOUR(cell);
        t.minute = PyDateTime_TIME_GET_MINUTE(cell);
        t.second = PyDateTime_TIME_GET_SECOND(cell);
        t.micro = PyDateTime_TIME_GET_MICROSECOND(cell);
    }
    return t;
}

/** Appends `digits` digits of the seconds fraction (at 100ns resolution), truncated. */
static int format_fraction(char* buf, int micro, int digits)
{
    if (digits <= 0)
        return 0;
    char ticks[16];
    snprintf(ticks, sizeof(ticks), "%07d", micro * 10);
    buf[0] = '.';
    memcpy(buf + 1, ticks, (size_t)(digits > 7 ? 7 : digits));
    return 1 + (digits > 7 ? 7 : digits);
}

/**
 * Appends the UTC offset a datetimeoffset value keeps, as a user would expect: an aware value's
 * own offset, and +00:00 for a naive value or a date, which is SQL Server's rule for a value
 * without one.  (The plain fast path differs here: the driver drops the tzinfo and applies the
 * client machine's local offset for the date.)
 *
 * @return The characters written, or -1 with ValueError set for an offset SQL Server cannot
 *         hold: not whole minutes, or beyond 14 hours.
 */
static int format_offset(char* buf, size_t cap, PyObject* cell, unsigned cls)
{
    long minutes = 0;
    if (cls == VAL_DATETIME && PyDateTime_DATE_GET_TZINFO(cell) != Py_None) {
        Object delta(PyObject_CallMethod(cell, "utcoffset", NULL));
        if (!delta.IsValid())
            return -1;
        if (delta.Get() != Py_None) {
            const long seconds = PyDateTime_DELTA_GET_DAYS(delta.Get()) * 86400L + PyDateTime_DELTA_GET_SECONDS(delta.Get());
            if (PyDateTime_DELTA_GET_MICROSECONDS(delta.Get()) != 0 || seconds % 60 != 0 || labs(seconds) > 14 * 3600) {
                PyErr_SetString(PyExc_ValueError, "datetimeoffset offsets must be whole minutes within 14 hours");
                return -1;
            }
            minutes = seconds / 60;
        }
    }
    return snprintf(buf, cap, " %c%02ld:%02ld", minutes < 0 ? '-' : '+', labs(minutes) / 60, labs(minutes) % 60);
}

/**
 * Formats a date, datetime or time value for a temporal destination, applying the rounding the
 * plain fast path gets: datetime2, datetimeoffset and time keep the destination's digits of the
 * fraction, truncated; datetime truncates to milliseconds then rounds to its 1/300 s tick;
 * smalldatetime truncates to seconds then rounds to the minute; a datetime that would round past
 * its maximum stores the maximum, as the server clamps it.  The driver's own conversion
 * rejects excess digits, and does not carry datetime rounding past midnight, hence doing it here.
 * A time value keeps its full fraction so an unrepresentable one fails as it does on that path.
 * A date column takes a datetime's date (see is_storable_as_date), and datetimeoffset values
 * carry the offset from format_offset.
 *
 * @return The characters written, or -1 with a Python exception set.
 */
static int format_temporal(char* buf, size_t cap, PyObject* cell, unsigned cls, const ColumnPlan& plan)
{
    CivilTime t = civil_of(cell, cls);
    const int scale = plan.target->scale;

    if (plan.destination == DEST_DATETIMEOFFSET) {
        int n = snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
        n += format_fraction(buf + n, t.micro, scale);
        const int offset = format_offset(buf + n, cap - (size_t)n, cell, cls);
        return offset < 0 ? -1 : n + offset;
    }

    if (plan.destination == DEST_TIME) {
        int n = snprintf(buf, cap, "%02d:%02d:%02d", t.hour, t.minute, t.second);
        if (cls == VAL_DATETIME)
            return n + format_fraction(buf + n, t.micro, scale);
        if (t.micro) {
            n += snprintf(buf + n, cap - (size_t)n, ".%06d", t.micro);
            while (buf[n - 1] == '0')
                --n;
        }
        return n;
    }
    if (cls == VAL_DATE || plan.destination == DEST_DATE)
        return snprintf(buf, cap, "%04d-%02d-%02d", t.year, t.month, t.day);

    if (plan.destination == DEST_DATETIME) {
        int ticks = ((t.micro / 1000) * 3 + 5) / 10;
        if (ticks == 300) {
            add_seconds(t, 1);
            ticks = 0;
        }
        if (t.year > 9999)
            return snprintf(buf, cap, "9999-12-31 23:59:59.997");
        return snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                        t.year, t.month, t.day, t.hour, t.minute, t.second, (ticks * 10 + 1) / 3);
    }
    if (plan.destination == DEST_SMALLDATETIME) {
        if (t.second >= 30)
            add_seconds(t, 60 - t.second);
        return snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:00", t.year, t.month, t.day, t.hour, t.minute);
    }
    int n = snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return n + format_fraction(buf + n, t.micro, scale);
}

/*=======================================================================================*/
// Filling columns
/*=======================================================================================*/

static bool unexpected_value(PyObject* cell)
{
    PyErr_Format(PyExc_TypeError, "unexpected %.100s value in a BCP column; the row changed while it was being sent",
                 Py_TYPE(cell)->tp_name);
    return false;
}

/** ASCII text the path formats itself, for a SQLCHARACTER or SQLNCHAR column. */
static bool put_ascii(Connection* cnxn, BcpCol* c, const char* text, Py_ssize_t n)
{
    if (c->hostType == SQLNCHAR)
        return BcpPutWidened(cnxn, c, (const unsigned char*)text, n);
    return BcpPutBytes(cnxn, c, text, n);
}

static bool put_int_text(Connection* cnxn, BcpCol* c, PyObject* cell)
{
    long long v = PyLong_AsLongLong(cell);
    if (v == -1 && PyErr_Occurred())
        return false;
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%lld", v);
    return put_ascii(cnxn, c, buf, n);
}

/**
 * Python code run while filling a row (a Decimal's normalize, a tzinfo's utcoffset) can close
 * the connection, after which nothing may touch its handle or BCP state.
 */
static bool connection_still_open(Connection* cnxn)
{
    if (cnxn->hdbc && cnxn->bcp)
        return true;
    PyErr_SetString(ProgrammingError, "The connection was closed while BCP was sending rows.");
    return false;
}

/**
 * A Decimal as exact text, normalized first in the thread's decimal context as the plain path
 * normalizes it: that rounds a value with more significant digits than the context's precision
 * (28 by default) before the column's scale applies.
 */
static bool put_decimal_text(BcpLoad& load, BcpCol* c, PyObject* cell)
{
    Object normalized(PyObject_CallMethodNoArgs(cell, load.normalizeName));
    if (!normalized.IsValid() && PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        return unexpected_value(cell);
    }
    if (!normalized.IsValid() || !connection_still_open(load.cnxn))
        return false;
    Object text(PyObject_Format(normalized, load.decimalFormat));
    if (!text.IsValid() || !connection_still_open(load.cnxn))
        return false;
    Py_ssize_t n = 0;
    const char* p = PyUnicode_AsUTF8AndSize(text, &n);
    return p && put_ascii(load.cnxn, c, p, n);
}

/**
 * A uuid.UUID or canonical GUID str for a SQLUNIQUEID column, as the 16 bytes the plain path
 * sends: a UUID's bytes_le, which can run Python code in a subclass.  A bytes_le that is not 16
 * bytes raises, where the plain path would copy 16 bytes from whatever it was given.
 */
static bool put_guid(BcpLoad& load, BcpCol* c, PyObject* cell)
{
    unsigned char guid[16];
    if (PyUnicode_Check(cell))
        return parse_guid_text(cell, guid) ? BcpPutFixed(load.cnxn, c, guid) : unexpected_value(cell);
    Object bytesLe(PyObject_GetAttrString(cell, "bytes_le"));
    if (!bytesLe.IsValid() && PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        return unexpected_value(cell);
    }
    if (!bytesLe.IsValid() || !connection_still_open(load.cnxn))
        return false;
    if (!PyBytes_Check(bytesLe.Get()) || PyBytes_GET_SIZE(bytesLe.Get()) != 16) {
        PyErr_SetString(PyExc_TypeError, "UUID.bytes_le must be 16 bytes");
        return false;
    }
    return BcpPutFixed(load.cnxn, c, PyBytes_AS_STRING(bytesLe.Get()));
}

/** Whether a value of class `cls` would still have given the column its host type. */
static bool suits_host_type(const ColumnPlan& plan, const BcpCol* c, unsigned cls)
{
    return choose_host_type(plan.destination, plan.classes | cls) == c->hostType;
}

/**
 * A value for a SQLCHARACTER column, as text the path formats or has checked.  A row can change
 * while it is being sent, so each value must still suit the column's host type and have a form
 * the plain path stores, or the call raises.
 */
static bool fill_text(BcpLoad& load, const ColumnPlan& plan, BcpCol* c, PyObject* cell)
{
    if (PyBool_Check(cell))
        return unexpected_value(cell);
    if (PyLong_Check(cell))
        return suits_host_type(plan, c, VAL_INT) ? put_int_text(load.cnxn, c, cell) : unexpected_value(cell);
    if (PyUnicode_Check(cell)) {
        if (plan.destination != DEST_TIME || !is_time_text(cell, plan.target->scale))
            return unexpected_value(cell);
        return put_ascii(load.cnxn, c, (const char*)PyUnicode_1BYTE_DATA(cell), PyUnicode_GET_LENGTH(cell));
    }
    if (PyDateTime_Check(cell) || PyDate_Check(cell) || PyTime_Check(cell)) {
        const unsigned cls = PyDateTime_Check(cell) ? VAL_DATETIME : (PyDate_Check(cell) ? VAL_DATE : VAL_TIME);
        if (!suits_host_type(plan, c, cls) || !has_plain_path_form(plan, cell, cls))
            return unexpected_value(cell);
        char buf[48];
        int n = format_temporal(buf, sizeof(buf), cell, cls, plan);
        return n >= 0 && connection_still_open(load.cnxn) && put_ascii(load.cnxn, c, buf, n);
    }
    return put_decimal_text(load, c, cell);
}

/**
 * A str for a SQLNCHAR column.  For a zero-length value the driver stores the previous row's
 * value in a char(n) column, which pads with spaces anyway, so an empty string goes there as one
 * space, which stores the same.
 */
static bool put_str(Connection* cnxn, const ColumnPlan& plan, BcpCol* c, PyObject* s)
{
    if (PyUnicode_GET_LENGTH(s) == 0 && plan.target->systemType == 175)
        return BcpPutWidened(cnxn, c, (const unsigned char*)" ", 1);
    return BcpPutWideStr(cnxn, c, s);
}

static bool fill_cell(BcpLoad& load, const ColumnPlan& plan, BcpCol* c, PyObject* cell)
{
    Connection* cnxn = load.cnxn;
    if (cell == Py_None)
        return BcpPutNull(cnxn, c);

    switch (c->hostType) {
    case SQLBIT: {
        if (!PyBool_Check(cell))
            return unexpected_value(cell);
        unsigned char b = cell == Py_True;
        return BcpPutFixed(cnxn, c, &b);
    }
    case SQLINT8: {
        long long v;
        if (PyBool_Check(cell))
            v = cell == Py_True;
        else if (PyLong_Check(cell)) {
            v = PyLong_AsLongLong(cell);
            if (v == -1 && PyErr_Occurred())
                return false;
        }
        else
            return unexpected_value(cell);
        return BcpPutFixed(cnxn, c, &v);
    }
    case SQLFLT8: {
        double d;
        if (PyBool_Check(cell))
            d = cell == Py_True;
        else if (PyFloat_Check(cell))
            d = PyFloat_AS_DOUBLE(cell);
        else if (PyLong_Check(cell)) {
            d = PyLong_AsDouble(cell);
            if (d == -1.0 && PyErr_Occurred())
                return false;
        }
        else
            return unexpected_value(cell);
        return BcpPutFixed(cnxn, c, &d);
    }
    case SQLBINARY:
        if (PyBytes_Check(cell))
            return BcpPutBytes(cnxn, c, PyBytes_AS_STRING(cell), PyBytes_GET_SIZE(cell));
        if (PyByteArray_Check(cell))
            return BcpPutBytes(cnxn, c, PyByteArray_AS_STRING(cell), PyByteArray_GET_SIZE(cell));
        return unexpected_value(cell);
    case SQLNCHAR:
        if (PyUnicode_Check(cell))
            return put_str(cnxn, plan, c, cell);
        if (PyLong_Check(cell) && !PyBool_Check(cell))
            return put_int_text(cnxn, c, cell);
        return unexpected_value(cell);
    case SQLUNIQUEID:
        return put_guid(load, c, cell);
    }
    return fill_text(load, plan, c, cell);
}

/*=======================================================================================*/
// The transaction around a load
/*=======================================================================================*/

static bool execute_direct(HDBC hdbc, const char16_t* sql)
{
    HSTMT hstmt = SQL_NULL_HANDLE;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt)))
        return false;
    SQLRETURN rc;
    Py_BEGIN_ALLOW_THREADS
    rc = SQLExecDirectW(hstmt, (SQLWCHAR*)sql, SQL_NTS);
    Py_END_ALLOW_THREADS
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return SQL_SUCCEEDED(rc);
}

/**
 * Starts the load's transaction.  In manual-commit mode the savepoint was already set by the
 * catalog query in prepare_load, saving a round trip.
 */
static bool begin_load_transaction(Connection* cnxn, LoadTransaction& txn)
{
    if (cnxn->nAutoCommit == SQL_AUTOCOMMIT_ON) {
        txn.autocommitSuspended = BcpSetAutocommit(cnxn->hdbc, false);
        return txn.autocommitSuspended;
    }
    txn.savepoint = true;
    return true;
}

/** Commits a completed batch in autocommit mode; in manual-commit mode it stays in the caller's transaction. */
static bool commit_load_batch(Connection* cnxn, const LoadTransaction& txn)
{
    return !txn.autocommitSuspended || BcpEndTransaction(cnxn->hdbc, true);
}

static bool finish_load_transaction(Connection* cnxn, LoadTransaction& txn)
{
    if (!txn.autocommitSuspended)
        return true;
    if (!BcpEndTransaction(cnxn->hdbc, true))
        return false;
    txn.autocommitSuspended = false;
    BcpSetAutocommit(cnxn->hdbc, true);
    return true;
}

/**
 * Undoes what the load sent and restores autocommit.  A savepoint that no longer exists (the
 * error aborted the whole transaction, as with XACT_ABORT ON) leaves nothing more to undo.
 */
static void rollback_load_transaction(Connection* cnxn, LoadTransaction& txn)
{
    if (txn.autocommitSuspended) {
        BcpEndTransaction(cnxn->hdbc, false);
        BcpSetAutocommit(cnxn->hdbc, true);
        txn.autocommitSuspended = false;
    }
    if (txn.savepoint) {
        execute_direct(cnxn->hdbc, u"IF XACT_STATE() = 1 ROLLBACK TRANSACTION pyodbc_bcp");
        txn.savepoint = false;
    }
}

/*=======================================================================================*/
// The load
/*=======================================================================================*/

/**
 * Reports a failure after rows may have been sent: raises (from the driver diagnostics unless a
 * Python exception is already set), closes the bulk copy and rolls back everything it sent.
 * The diagnostics are read before bcp_done, which clears them.
 */
static bool abandon_load(BcpLoad& load, LoadTransaction& txn, const char* function)
{
    Connection* cnxn = load.cnxn;
    if (!PyErr_Occurred())
        RaiseErrorFromHandle(cnxn, function, cnxn->hdbc, SQL_NULL_HANDLE);
    if (!cnxn->hdbc || !cnxn->bcp)
        return true;
    Py_BEGIN_ALLOW_THREADS
    cnxn->bcp->procs.bcp_done(cnxn->hdbc);
    Py_END_ALLOW_THREADS
    rollback_load_transaction(cnxn, txn);
    return true;
}

/**
 * Starts the bulk copy and binds the columns.  Nothing has been sent yet, so on failure the
 * session is closed, the transaction undone and the call falls back to the plain path.
 */
static bool start_bulk_copy(BcpLoad& load, Cursor* cur, LoadTransaction& txn)
{
    Connection* cnxn = load.cnxn;
    const BcpProcs procs = cnxn->bcp->procs;
    SQLWCHAR* name = BcpQuotedTableName(load.insert);
    if (!name)
        return false;
    SQLRETURN rc;
    Py_BEGIN_ALLOW_THREADS
    rc = procs.bcp_initW(cnxn->hdbc, name, nullptr, nullptr, DB_IN);
    Py_END_ALLOW_THREADS
    PyMem_Free(name);
    if (rc != SUCCEED)
        return false;

    bool ok = (!load.keepNulls || procs.bcp_control(cnxn->hdbc, BCPKEEPNULLS, (void*)(uintptr_t)1) == SUCCEED)
           && procs.bcp_control(cnxn->hdbc, BCPHINTS,
                                (void*)(cur->bcp_tablock ? "CHECK_CONSTRAINTS,FIRE_TRIGGERS,TABLOCK"
                                                         : "CHECK_CONSTRAINTS,FIRE_TRIGGERS")) == SUCCEED;
    for (int i = 0; ok && i < load.ncols + load.nfill; ++i)
        ok = BcpBindColumn(cnxn, &load.cols[i]);
    if (!ok) {
        Py_BEGIN_ALLOW_THREADS
        procs.bcp_done(cnxn->hdbc);
        Py_END_ALLOW_THREADS
    }
    return ok;
}

/**
 * Sends every row, batching every cur->bcp_batch_rows rows, and finishes the bulk copy.
 *
 * @return true on success with *copied set; false with an error raised and the load undone.
 */
static bool send_rows(BcpLoad& load, Cursor* cur, LoadTransaction& txn, long long* copied)
{
    Connection* cnxn = load.cnxn;
    const BcpProcs procs = cnxn->bcp->procs;
    const Py_ssize_t nrows = PyTuple_GET_SIZE(load.rows.Get());
    const long batchRows = cur->bcp_batch_rows;
    long unbatched = 0;
    SQLRETURN rc;
    DBINT count;

    *copied = 0;
    for (Py_ssize_t r = 0; r < nrows; ++r) {
        PyObject* row = PyTuple_GET_ITEM(load.rows.Get(), r);
        for (int i = 0; i < load.ncols; ++i) {
            if (!is_row(row, load.ncols)) {
                PyErr_SetString(PyExc_TypeError, "a row changed length while it was being sent by BCP");
                return !abandon_load(load, txn, "bcp_sendrow");
            }
            Object cell(PySequence_Fast_GET_ITEM(row, i));
            Py_INCREF(cell.Get());
            if (!fill_cell(load, load.plans[i], &load.cols[i], cell))
                return !abandon_load(load, txn, "bcp_collen");
        }
        Py_BEGIN_ALLOW_THREADS
        rc = procs.bcp_sendrow(cnxn->hdbc);
        Py_END_ALLOW_THREADS
        if (rc != SUCCEED)
            return !abandon_load(load, txn, "bcp_sendrow");

        if (batchRows > 0 && ++unbatched == batchRows) {
            Py_BEGIN_ALLOW_THREADS
            count = procs.bcp_batch(cnxn->hdbc);
            Py_END_ALLOW_THREADS
            if (count < 0)
                return !abandon_load(load, txn, "bcp_batch");
            if (!commit_load_batch(cnxn, txn))
                return !abandon_load(load, txn, "SQLEndTran");
            *copied += count;
            unbatched = 0;
        }
    }

    Py_BEGIN_ALLOW_THREADS
    count = procs.bcp_done(cnxn->hdbc);
    Py_END_ALLOW_THREADS
    if (count < 0) {
        RaiseErrorFromHandle(cnxn, "bcp_done", cnxn->hdbc, SQL_NULL_HANDLE);
        rollback_load_transaction(cnxn, txn);
        return false;
    }
    *copied += count;
    if (*copied != nrows) {
        PyErr_Format(PyExc_RuntimeError, "BCP copied %lld of %zd rows", *copied, nrows);
        rollback_load_transaction(cnxn, txn);
        return false;
    }
    if (!finish_load_transaction(cnxn, txn)) {
        RaiseErrorFromHandle(cnxn, "SQLEndTran", cnxn->hdbc, SQL_NULL_HANDLE);
        rollback_load_transaction(cnxn, txn);
        return false;
    }
    return true;
}

/**
 * Prepares a load: parses the INSERT, loads the destination columns, maps and scans the rows
 * and picks host types, all before anything is sent.  A table with an enabled INSERT trigger
 * is left to the plain path: a trigger that rolls back a bulk copy run inside a transaction
 * leaves the driver unable to use the connection again.
 *
 * @return 1 to go ahead, 0 to fall back to the plain path, -1 on error (Python exception set).
 */
static int prepare_load(BcpLoad& load, PyObject* pSql, PyObject* param_seq)
{
    Connection* cnxn = load.cnxn;

    Py_ssize_t sqlLen = 0;
    const char* sql = PyUnicode_AsUTF8AndSize(pSql, &sqlLen);
    if (!sql) {
        PyErr_Clear();
        return 0;
    }
    if ((Py_ssize_t)strlen(sql) != sqlLen)
        return 0;
    int r = BcpParseInsert(sql, &load.insert);
    if (r <= 0)
        return r;
    load.insertParsed = true;

    if (!BcpEnsureLoaded(cnxn))
        return PyErr_Occurred() ? -1 : 0;
    if (cnxn->bcp->sessionOpen || !BcpIsEnabled(cnxn->hdbc))
        return 0;

    if ((r = BcpLoadTarget(cnxn, load.insert, cnxn->nAutoCommit == SQL_AUTOCOMMIT_OFF, &load.target)) <= 0)
        return r;
    if (load.target.hasInsertTrigger)
        return 0;
    if ((r = map_columns(load)) <= 0)
        return r;

    load.rows = PySequence_Tuple(param_seq);
    if (!load.rows.IsValid())
        return -1;
    if (!scan_rows(load) || !choose_keep_nulls(load))
        return 0;
    if ((r = plan_columns(load)) <= 0)
        return r;

    load.decimalFormat = PyUnicode_FromString("f");
    load.normalizeName = PyUnicode_InternFromString("normalize");
    return load.decimalFormat.IsValid() && load.normalizeName.IsValid() ? 1 : -1;
}

bool ExecuteMulti_BCP(Cursor* cur, PyObject* pSql, PyObject* param_seq)
{
    if (!cur->use_bcp_fast || !PyUnicode_Check(pSql) || cur->inputsizes)
        return false;

    BcpLoad load(cur->cnxn);
    int prepared = prepare_load(load, pSql, param_seq);
    if (prepared <= 0)
        return prepared < 0;

    LoadTransaction txn = {false, false};
    if (!begin_load_transaction(load.cnxn, txn))
        return false;
    if (!start_bulk_copy(load, cur, txn)) {
        rollback_load_transaction(load.cnxn, txn);
        return PyErr_Occurred() != 0;
    }

    long long copied = 0;
    if (send_rows(load, cur, txn, &copied))
        cur->rowcount = (int)copied;
    return true;
}
