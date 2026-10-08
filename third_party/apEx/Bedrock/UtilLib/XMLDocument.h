#pragma once

#include "../BaseLib/BaseLib.h"
#include "XMLNode.h"

// The COM VARIANT/BSTR types are gone; these standalone stand-ins keep the two
// remaining signatures that referenced them source compatible.  VARIANT is only
// used as a by-value parameter that the implementation ignores, and BSTR is a
// plain string pointer - neither appears anywhere else in the tree.
#ifndef _APEX_XML_TYPES_DEFINED
#define _APEX_XML_TYPES_DEFINED
struct tagVARIANT {};
typedef struct tagVARIANT VARIANT;
typedef wchar_t * BSTR;
#endif

class CXMLDocument
{
  CXMLDocImpl * pDoc;

  // CXMLNode needs the allocation pool / intern table of the document.
  friend class CXMLNode;

  CXMLDocImpl *RawDoc() { return pDoc; }

  // Allocator of the backing document (a rapidxml::xml_document<char>*).
  void *RawPool() { return pDoc ? pDoc->RawDoc() : NULL; }

public:
  CXMLDocument( void );
  ~CXMLDocument( void );

  TBOOL Allocate();
  TBOOL Cleanup();

  TBOOL LoadFromFile( TCHAR * );
  TBOOL LoadFromString( CString );
  TBOOL SaveToFile( TCHAR * );
  CString SaveToString();
  CXMLNode GetDocumentNode();

  CXMLNode CreateNode( VARIANT type, BSTR Name, TS32 Level );
};
