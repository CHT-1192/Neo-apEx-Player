#pragma once
// TCHAR maps to char in this port (the original MSVC projects build without
// UNICODE as well), so the _t* helpers are thin wrappers over the byte CRT.
#include "Windows.h"

#include <ctype.h>

#define _tmain main
#define _tWinMain(...) main(__VA_ARGS__)

#define _tcslen strlen
#define _tcscpy strcpy
#define _tcsncpy strncpy
#define _tcscat strcat
#define _tcsncat strncat
#define _tcscmp strcmp
#define _tcsncmp strncmp
#define _tcsicmp strcasecmp
#define _tcsnicmp strncasecmp
#define _tcsstr strstr
#define _tcsrchr strrchr
#define _tcsdup strdup
#define _tcsrev strrev_compat
#define _stprintf sprintf
#define _sntprintf snprintf
#define _tprintf printf
#define _ttoi atoi
#define _ttol atol
#define _tstof atof
#define _totlower tolower
#define _totupper toupper
#define _istascii isascii_macro
#define _istalpha isalpha
#define _istdigit isdigit
#define _istspace isspace

inline char *strrev_compat(char *s)
{
  size_t n = strlen(s);
  for (size_t i = 0; i < n / 2; i++)
  {
    char t = s[i];
    s[i] = s[n - 1 - i];
    s[n - 1 - i] = t;
  }
  return s;
}
#define isascii_macro(c) ((c) >= 0 && (c) < 128)

#ifdef UNICODE
#error "This port builds the engine in ANSI mode (TCHAR == char)."
#endif

#define _istalnum isalnum
#define _istalpha isalpha
#define _istdigit isdigit
#define _istspace isspace
#define _istpunct ispunct
#define _istupper isupper
#define _istlower islower
#define _istxdigit isxdigit
#define _istcntrl iscntrl
#define _istprint isprint
#define _istgraph isgraph
#define _tcsftime strftime
#define _tcsncpy_s strncpy_s
#define _tcscpy_s strcpy_s
#define _tcscat_s strcat_s
#define _tstoi atoi
#define _tstol atol
#define _tstoi64 _atoi64
#define _tsprintf_s sprintf_s
