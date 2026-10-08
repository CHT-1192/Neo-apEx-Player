#include "XMLNode.h"
#include "XMLDocument.h"

#include "RapidXML/rapidxml.hpp"

#include <string.h>

using RapidNode = rapidxml::xml_node<char>;
using RapidDoc = rapidxml::xml_document<char>;
using RapidAttr = rapidxml::xml_attribute<char>;

// ---------------------------------------------------------------------------
// helpers
//
// The parser runs without string terminators, so names and values are NOT zero
// terminated and must always be handled together with their size.
// ---------------------------------------------------------------------------

// MSVC's "copy as much as fits" sentinel, understood by the compat strncpy_s.
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

// Tokens from the parser are not zero terminated, so the length always has to
// be passed in.
static void CopyTruncated( TCHAR *Buffer, TS32 BufferSize, const char *Source, std::size_t Length )
{
  if ( BufferSize <= 0 ) return;
  if ( !Source ) { Buffer[ 0 ] = 0; return; }

  std::size_t Count = ( Length < (std::size_t)( BufferSize - 1 ) ) ? Length : (std::size_t)( BufferSize - 1 );
  if ( Count ) memcpy( Buffer, Source, Count );
  Buffer[ Count ] = 0;
}

static bool IsElement( const RapidNode *n )
{
  return n && n->type() == rapidxml::node_element;
}

// Element names are compared exactly like MSXML's wcscmp in the original
// implementation (case sensitive, byte comparison).
static bool NameIs( const RapidNode *n, const char *Name )
{
  if ( !n || !Name ) return false;
  return ApexXMLNameEquals( n->name(), n->name_size(), Name );
}

// A (not necessarily terminated) RapidXML token as a CString.
static CString TokenToString( const char *Text, std::size_t Size )
{
  CString s;
  if ( Text && Size )
    s = CString( Text, (TU32)Size );
  return s;
}

// MSXML's get_text() on an element returns the concatenation of every text node
// below it, in document order.
static void CollectText( const RapidNode *n, CString &Out )
{
  for ( const RapidNode *c = n->first_node(); c; c = c->next_sibling() )
  {
    if ( c->type() == rapidxml::node_data || c->type() == rapidxml::node_cdata )
      Out.Append( c->value(), (TU32)c->value_size() );
    else if ( c->type() == rapidxml::node_element )
      CollectText( c, Out );
  }
}

CXMLNode::CXMLNode()
{
  pNode = NULL;
  pDoc = NULL;
  nLevel = 0;
}

CXMLNode::CXMLNode( RapidNode * p, CXMLDocument * d, TS32 l )
{
  pNode = p;
  pDoc = d;
  nLevel = l;
}

CXMLNode::CXMLNode( const CXMLNode &Original )
{
  nLevel = Original.nLevel;
  pNode = Original.pNode;
  pDoc = Original.pDoc;
}

CXMLNode CXMLNode::operator=( const CXMLNode Original )
{
  nLevel = Original.nLevel;
  pNode = Original.pNode;
  pDoc = Original.pDoc;
  return *this;
}

CXMLNode::~CXMLNode()
{
}

TS32 CXMLNode::GetChildCount()
{
  if ( !pNode ) return 0;

  TS32 i = 0;
  for ( const RapidNode *c = pNode->first_node(); c; c = c->next_sibling() )
    i++;

  return i;
}

TS32 CXMLNode::GetChildCount( TCHAR * szNodeName )
{
  if ( !pNode ) return 0;

  TS32 i = 0;
  for ( const RapidNode *c = pNode->first_node(); c; c = c->next_sibling() )
    if ( IsElement( c ) && NameIs( c, szNodeName ) )
      i++;

  return i;
}

CString CXMLNode::GetNodeName()
{
  if ( !pNode ) return CString();
  return TokenToString( pNode->name(), pNode->name_size() );
}

CXMLNode CXMLNode::GetChild( TS32 n )
{
  if ( !pNode ) return CXMLNode();

  TS32 i = 0;
  for ( RapidNode *c = pNode->first_node(); c; c = c->next_sibling() )
  {
    if ( n == i )
      return CXMLNode( c, pDoc, nLevel + 1 );
    i++;
  }

  return CXMLNode();
}

CXMLNode CXMLNode::GetChild( TCHAR * szNodeName )
{
  if ( !pNode ) return CXMLNode();

  for ( RapidNode *c = pNode->first_node(); c; c = c->next_sibling() )
    if ( IsElement( c ) && NameIs( c, szNodeName ) )
      return CXMLNode( c, pDoc, nLevel + 1 );

  return CXMLNode();
}

CXMLNode CXMLNode::GetChild( TCHAR * szNodeName, TS32 n )
{
  if ( !pNode ) return CXMLNode();

  TS32 i = 0;
  for ( RapidNode *c = pNode->first_node(); c; c = c->next_sibling() )
  {
    if ( !IsElement( c ) || !NameIs( c, szNodeName ) ) continue;

    if ( n == i )
      return CXMLNode( c, pDoc, nLevel + 1 );
    i++;
  }

  return CXMLNode();
}

void CXMLNode::GetText( TCHAR * szBuffer, TS32 nBufferSize )
{
  CString s = GetText();
  CopyTruncated( szBuffer, nBufferSize, s.GetPointer(), (std::size_t)s.Length() );
}

CString CXMLNode::GetText()
{
  CString s;
  if ( pNode )
    CollectText( pNode, s );
  return s;
}

TBOOL CXMLNode::GetAttribute( TCHAR * szAttribute, TCHAR * szBuffer, TS32 nBufferSize )
{
  if ( !pNode ) return false;

  for ( const RapidAttr *a = pNode->first_attribute(); a; a = a->next_attribute() )
    if ( ApexXMLNameEquals( a->name(), a->name_size(), szAttribute ) )
    {
      CopyTruncated( szBuffer, nBufferSize, a->value(), a->value_size() );
      return true;
    }

  return false;
}

CString CXMLNode::GetAttribute( TCHAR * szAttribute )
{
  if ( !pNode ) return _T( "" );

  for ( const RapidAttr *a = pNode->first_attribute(); a; a = a->next_attribute() )
    if ( ApexXMLNameEquals( a->name(), a->name_size(), szAttribute ) )
      return TokenToString( a->value(), a->value_size() );

  return _T( "" );
}

CString CXMLNode::GetAttributeAsString( TCHAR * szAttribute )
{
  if ( !pNode ) return _T( "" );

  for ( const RapidAttr *a = pNode->first_attribute(); a; a = a->next_attribute() )
    if ( ApexXMLNameEquals( a->name(), a->name_size(), szAttribute ) )
      return TokenToString( a->value(), a->value_size() );

  return _T( "" );
}

TBOOL CXMLNode::HasAttribute( TCHAR * szAttribute )
{
  if ( !pNode ) return false;

  for ( const RapidAttr *a = pNode->first_attribute(); a; a = a->next_attribute() )
    if ( ApexXMLNameEquals( a->name(), a->name_size(), szAttribute ) )
      return true;

  return false;
}

TS32 CXMLNode::IsValid()
{
  return pNode != NULL;
}

void CXMLNode::GetAttributeAsInteger( TCHAR * szAttribute, TS32 * pnValue )
{
  *pnValue = 0;

  TCHAR s[ 32 ];
  ZeroMemory( s, sizeof( s ) );
  GetAttribute( szAttribute, s, 32 );
  if ( !s[ 0 ] ) return;

  sscanf( s, _T( "%d" ), pnValue );
}

void CXMLNode::GetAttributeAsFloat( TCHAR * szAttribute, TF32 * pfValue )
{
  *pfValue = 0;

  TCHAR s[ 32 ];
  ZeroMemory( s, sizeof( s ) );
  GetAttribute( szAttribute, s, 32 );
  if ( !s[ 0 ] ) return;

  sscanf( s, _T( "%g" ), pfValue );
}

//void CXMLNode::FlushNode()
//{
//  Removes every child of this node.
//}

CXMLNode CXMLNode::AddChild( TCHAR * szNodeName, TBOOL PostEnter )
{
  if ( !pNode || !pDoc ) return CXMLNode();

  CXMLDocImpl *Impl = pDoc->RawDoc();
  rapidxml::xml_document<char> *Doc = (rapidxml::xml_document<char>*)pDoc->RawPool();
  if ( !Impl || !Doc ) return CXMLNode();

  IGNOREFREEERRORS( true );

  // The original MSXML implementation appended a text node with the indentation
  // for the child, then the element itself.  "\r\n" plus up to 30 spaces, cut
  // off after nLevel * 2 + 2 characters.
  const char *Indent = "\r\n                              ";

  const char *StoredName = Impl->Intern( szNodeName );

  TS32 PreLen = nLevel * 2 + 2;
  if ( PreLen < 0 ) PreLen = 0;
  if ( PreLen > 32 ) PreLen = 32;

  // The new element goes at the very end; the indentation nodes then go in
  // front of it, so that the whitespace that used to be trailing ends up after
  // the closing tag exactly like the MSXML implementation left it.
  RapidNode *Element = Doc->allocate_node( rapidxml::node_element, StoredName, NULL, strlen( StoredName ) );
  pNode->append_node( Element );

  char *Pre = Impl->AllocateString( PreLen );
  memcpy( Pre, Indent, (size_t)PreLen );
  Pre[ PreLen ] = 0;

  {
    RapidNode *t = Doc->allocate_node( rapidxml::node_data );
    t->value( Pre, (std::size_t)PreLen );
    pNode->insert_node( Element, t );
  }

  if ( PostEnter )
  {
    TS32 PostLen = nLevel * 2;
    if ( PostLen < 0 ) PostLen = 0;
    if ( PostLen > 32 ) PostLen = 32;

    char *Post = Impl->AllocateString( PostLen );
    memcpy( Post, Indent, (size_t)PostLen );
    Post[ PostLen ] = 0;

    RapidNode *t = Doc->allocate_node( rapidxml::node_data );
    t->value( Post, (std::size_t)PostLen );
    pNode->append_node( t );
  }

  IGNOREFREEERRORS( false );

  if ( !PostEnter ) return CXMLNode();

  return CXMLNode( Element, pDoc, nLevel + 1 );
}

void CXMLNode::SetAttribute( TCHAR * szAttributeName, const TCHAR * szValue )
{
  if ( !pNode || !pDoc ) return;

  CXMLDocImpl *Impl = pDoc->RawDoc();
  rapidxml::xml_document<char> *Doc = (rapidxml::xml_document<char>*)pDoc->RawPool();
  if ( !Impl || !Doc ) return;

  IGNOREFREEERRORS( true );

  const char *Value = szValue ? szValue : "";
  TS32 ValueLen = (TS32)strlen( Value );

  char *Copy = Impl->AllocateString( ValueLen );
  memcpy( Copy, Value, (size_t)ValueLen );
  Copy[ ValueLen ] = 0;

  for ( RapidAttr *a = pNode->first_attribute(); a; a = a->next_attribute() )
    if ( ApexXMLNameEquals( a->name(), a->name_size(), szAttributeName ) )
    {
      a->value( Copy, (std::size_t)ValueLen );
      IGNOREFREEERRORS( false );
      return;
    }

  const char *StoredName = Impl->Intern( szAttributeName );
  RapidAttr *a = Doc->allocate_attribute( StoredName, Copy, strlen( StoredName ), (std::size_t)ValueLen );
  pNode->append_attribute( a );

  IGNOREFREEERRORS( false );
}

void CXMLNode::SetAttributeFromInteger( TCHAR * szAttributeName, TS32 nValue )
{
  TCHAR s[ 64 ];
  snprintf( s, 64, _T( "%d" ), nValue );
  SetAttribute( szAttributeName, s );
}

void CXMLNode::SetAttributeFromFloat( TCHAR * szAttributeName, TF32 nValue )
{
  TCHAR s[ 64 ];
  snprintf( s, 64, _T( "%g" ), nValue );
  SetAttribute( szAttributeName, s );
}

void CXMLNode::SetText( const TCHAR * sz )
{
  if ( !pNode || !pDoc ) return;

  CXMLDocImpl *Impl = pDoc->RawDoc();
  rapidxml::xml_document<char> *Doc = (rapidxml::xml_document<char>*)pDoc->RawPool();
  if ( !Impl || !Doc ) return;

  IGNOREFREEERRORS( true );

  // MSXML's put_text replaces every child of the element with one text node.
  pNode->remove_all_nodes();

  RapidNode *n = Doc->allocate_node( rapidxml::node_data );

  if ( sz && sz[ 0 ] )
  {
    TS32 Length = (TS32)strlen( sz );
    char *Copy = Impl->AllocateString( Length );
    memcpy( Copy, sz, (size_t)Length );
    Copy[ Length ] = 0;
    n->value( Copy, (std::size_t)Length );
  }

  pNode->append_node( n );

  IGNOREFREEERRORS( false );
}

void CXMLNode::SetText( CString &s )
{
  SetText( s.GetPointer() );
}

CString CXMLNode::SaveToString()
{
  if ( !pNode || !pDoc ) return CString();

  // Serialise exactly this node (MSXML's IXMLDOMNode::get_xml).
  std::string Buffer;
  ApexXMLWriteNode( Buffer, pNode );

  return CString( Buffer.c_str(), (TU32)Buffer.size() );
}

void CXMLNode::SetInt( TS32 Int )
{
  TCHAR s[ 64 ];
  snprintf( s, 64, _T( "%d" ), Int );
  SetText( s );
}

void CXMLNode::SetFloat( TF32 Float )
{
  TCHAR s[ 64 ];
  snprintf( s, 64, _T( "%g" ), Float );
  SetText( s );
}

TBOOL CXMLNode::GetValue( TS32 &Int )
{
  CString s = GetText();
  if ( !s.GetPointer() ) return false;
  return sscanf( s.GetPointer(), _T( "%d" ), &Int ) == 1;
}

TBOOL CXMLNode::GetValue( TBOOL &Int )
{
  TS32 x = 0;
  CString s = GetText();
  if ( !s.GetPointer() ) return false;

  TS32 r = sscanf( s.GetPointer(), _T( "%d" ), &x );
  if ( r == 1 )
    Int = x != 0;
  return r == 1;
}

TBOOL CXMLNode::GetValue( TF32 &Float )
{
  CString s = GetText();
  if ( !s.GetPointer() ) return false;
  return sscanf( s.GetPointer(), _T( "%g" ), &Float ) == 1;
}
