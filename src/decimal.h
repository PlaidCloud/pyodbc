#pragma once

bool InitializeDecimal();
PyObject* GetDecimalPoint();
bool SetDecimalPoint(PyObject* pNew);

PyObject* DecimalFromText(const TextEnc& enc, const byte* pb, Py_ssize_t cb);
PyObject* DecimalFromNumericStruct(const SQL_NUMERIC_STRUCT& num);
