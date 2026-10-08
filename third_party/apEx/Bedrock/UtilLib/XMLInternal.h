#pragma once

// Shared declaration for the RapidXML based backing store of CXMLDocument /
// CXMLNode.  See XMLDocument.cpp for the implementation.
//
// This header deliberately contains no MSXML/COM material so that every header
// which ends up including XMLNode.h (through phxResource.h / phxSplineExt.h /
// apxProject.h) stays portable.

#include "../BaseLib/BaseLib.h"

#include <string.h>
#include <string>
#include <unordered_set>

// Forward declaration only - rapidxml.hpp itself stays out of the public headers.
namespace rapidxml
{
  template<class Ch> class xml_node;
}

// The document node of RapidXML is the parent of every element (including the
// root element) and the owner of the character buffers that the nodes point at.
class CXMLDocImpl
{
public:
  CXMLDocImpl();
  ~CXMLDocImpl();

  CXMLDocImpl( const CXMLDocImpl & ) = delete;
  CXMLDocImpl &operator=( const CXMLDocImpl & ) = delete;

  void Clear();

  // Document without any element (used by AddChild on the document node).
  rapidxml::xml_node<char> *CreateRootElement( const char *Name );

  // The document node itself (parent of the root element).
  rapidxml::xml_node<char> *DocumentNode();

  // Parses Text (Length bytes, no terminator required) into this document.
  // Returns false and fills in the error fields when the text is not well formed.
  bool Parse( const char *Text, TU32 Length );

  // Character storage with a lifetime at least as long as this document, for the
  // names and values that RapidXML only keeps a pointer to.
  const char *Intern( const char *Str );
  char *AllocateString( TS32 Length );

  // Writes the document back out the way MSXML did: all formatting whitespace
  // is carried by the text nodes of the document, so nothing is added here.
  void WriteString( CString &Out ) const;
  void WriteFile( FILE *f ) const;

  // Allocation pool of the backing document (raw rapidxml::xml_document<char>).
  void *RawDoc() const { return DocPtr; }

  bool ParsedOk = false;
  TS32 ParseErrorLine = 0;
  TS32 ParseErrorColumn = 0;
  CString ParseErrorReason;

private:
  void *DocPtr;                       // rapidxml::xml_document<char> *
  char *TextBuffer;                   // parsed text; RapidXML points into this
  std::unordered_set<std::string> Strings;
};

// Serialises one RapidXML node (and its subtree).  Not part of the public API -
// shared between XMLDocument.cpp and XMLNode.cpp.
void ApexXMLWriteNode( std::string &Out, const rapidxml::xml_node<char> *n );

// Size aware name comparison.  The parser runs non destructively, so RapidXML
// does NOT zero terminate element/attribute names and values - strcmp() must
// never be used on them.
inline bool ApexXMLNameEquals( const char *Name, std::size_t NameSize, const char *Other )
{
  if ( !Name || !Other ) return false;
  return strncmp( Name, Other, NameSize ) == 0 && Other[ NameSize ] == 0;
}
