// compat/comdef.h
//
// Portable stand-in for the MSVC <comdef.h>.
//
// The engine only uses one thing out of comdef: _com_error, the tiny exception
// helper it constructs around a failing HRESULT so the log line can print
// something readable:
//
//     _com_error err(res);
//     LOG(LOG_ERROR, _T("... (%s)"), err.ErrorMessage());
//
// The real _com_error::ErrorMessage() returns a _bstr_t (a BSTR wrapper with an
// implicit conversion to const char*).  _bstr_t does not exist in this port and
// the call sites only ever feed the result to a printf-style %s, so
// ErrorMessage() hands back a plain const char* pointing at storage owned by the
// _com_error instance.  That keeps every call site compiling unchanged and is
// safe because the message buffer lives as long as the object.
//
// The _com_ptr_t / CComPtr smart pointers are intentionally NOT provided: no
// file in the reused subset needs them, and the engine manages its D3D objects
// with explicit AddRef/Release.
#pragma once

#include "Windows.h"

#include <cstdio>
#include <cstring>

class _com_error
{
public:
    _com_error()
        : m_hr(S_OK)
    {
        Format();
    }

    _com_error(HRESULT hr)
        : m_hr(hr)
    {
        Format();
    }

    _com_error(const _com_error &other)
        : m_hr(other.m_hr)
    {
        memcpy(m_message, other.m_message, sizeof(m_message));
    }

    _com_error &operator=(const _com_error &other)
    {
        if (this != &other)
        {
            m_hr = other.m_hr;
            memcpy(m_message, other.m_message, sizeof(m_message));
        }
        return *this;
    }

    ~_com_error() {}

    // The failing HRESULT.
    HRESULT Error() const { return m_hr; }

    // Human readable description, e.g. "E_INVALIDARG (0x80070057)".
    const char *ErrorMessage() const { return m_message; }

    // Alias for the SDK naming.
    const char *Description() const { return m_message; }

    // A WCHAR overload is deliberately absent: this port builds the engine in
    // ANSI mode (TCHAR == char), exactly like the original MSVC tool project.

private:
    void Format()
    {
        const char *name = NameForHresult(m_hr);
        if (name)
            snprintf(m_message, sizeof(m_message), "%s (0x%08lX)", name, (unsigned long)m_hr);
        else
            snprintf(m_message, sizeof(m_message), "HRESULT 0x%08lX", (unsigned long)m_hr);
        m_message[sizeof(m_message) - 1] = '\0';
    }

    static const char *NameForHresult(HRESULT hr)
    {
        switch ((unsigned long)hr)
        {
            case 0x00000000UL: return "S_OK";
            case 0x00000001UL: return "S_FALSE";
            case 0x80004001UL: return "E_NOTIMPL";
            case 0x80004002UL: return "E_NOINTERFACE";
            case 0x80004003UL: return "E_POINTER";
            case 0x80004004UL: return "E_ABORT";
            case 0x80004005UL: return "E_FAIL";
            case 0x80070005UL: return "E_ACCESSDENIED";
            case 0x8007000EUL: return "E_OUTOFMEMORY";
            case 0x80070057UL: return "E_INVALIDARG";
            case 0x8000FFFFUL: return "E_UNEXPECTED";
            case 0x80040154UL: return "REGDB_E_CLASSNOTREG";
            case 0x800401F0UL: return "CO_E_NOTINITIALIZED";
            case 0x887A0001UL: return "DXGI_ERROR_INVALID_CALL";
            case 0x887A0002UL: return "DXGI_ERROR_NOT_FOUND";
            case 0x887A0004UL: return "DXGI_ERROR_UNSUPPORTED";
            case 0x887A0005UL: return "DXGI_ERROR_DEVICE_REMOVED";
            case 0x887A0006UL: return "DXGI_ERROR_DEVICE_HUNG";
            case 0x887A0007UL: return "DXGI_ERROR_DEVICE_RESET";
            default: break;
        }
        // HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) and close neighbours.
        if ((unsigned long)hr == 0x80070000UL + 50UL) return "ERROR_NOT_SUPPORTED";
        if ((unsigned long)hr == 0x80070000UL + 87UL) return "ERROR_INVALID_PARAMETER";
        return nullptr;
    }

    HRESULT m_hr;
    char    m_message[96];
};
