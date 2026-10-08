#include "XMLDocument.h"

#include "RapidXML/rapidxml.hpp"

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// RapidXML backing store
//
// The parser runs without string terminators so that the source buffer stays
// byte-for-byte intact.  Two reasons:
//   * whitespace-only text nodes between elements (which carry the formatting of
//     the tool's project files) are dropped by RapidXML, so they have to be
//     recovered from the source text;
//   * recovering them needs intact tag boundaries, and the default parser
//     overwrites the character after every name/value with a terminator.
// The price is that no token is zero terminated, so every name and value is read
// through its recorded size (see ApexXMLNameEquals).
// ---------------------------------------------------------------------------

using RapidNode = rapidxml::xml_node<char>;
using RapidDoc = rapidxml::xml_document<char>;
using RapidAttr = rapidxml::xml_attribute<char>;

// Defined below, declared in XMLInternal.h - also used by XMLNode.cpp.
void ApexXMLWriteNode( std::string &Out, const rapidxml::xml_node<char> *n );

namespace
{
  // ---- small character helpers -------------------------------------------

  inline bool IsWhitespace( char c )
  {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }

  bool IsPureWhitespace( const char *Begin, const char *End )
  {
    if ( Begin >= End ) return false;

    for ( const char *p = Begin; p < End; p++ )
      if ( !IsWhitespace( *p ) ) return false;

    return true;
  }

  // ---- source scanning ------------------------------------------------
  //
  // RapidXML drops whitespace-only text nodes between elements, and the tool's
  // project files carry their formatting in exactly those nodes.  They are
  // recovered from the source buffer, which the parser leaves untouched.

  bool IsNameStartChar( char c )
  {
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_' || c == ':';
  }

  // Whether an element with no children was written as '<x/>' or '<x></x>' is
  // not preserved by RapidXML, but MSXML preserved it and the project files use
  // both forms.  The distinction rides on the node's value pointer: element
  // values are never used by this code base, so the pointer can carry a marker.
  const char *SelfClosingMarker() { static const char Marker = 0; return &Marker; }
  const char *EmptyPairMarker()   { static const char Marker = 0; return &Marker; }

  inline bool IsSelfClosingNode( const RapidNode *n )
  {
    return n->value() == SelfClosingMarker();
  }

  inline void MarkEmptyPair( RapidNode *n )
  {
    n->value( EmptyPairMarker(), 0 );
  }

  // Finds the '>' that ends the start tag beginning at p.  Attribute values are
  // skipped so that a '>' inside one cannot end the tag early.
  const char *FindStartTagEnd( const char *p )
  {
    while ( *p )
    {
      if ( *p == '"' || *p == '\'' )
      {
        char Quote = *p++;
        while ( *p && *p != Quote ) p++;
        if ( !*p ) return p;
      }
      else if ( *p == '>' )
        return p;

      p++;
    }

    return p;
  }

  // Locates the closing tag of the element called Name and returns the position
  // of its '>'.
  const char *FindElementClose( const char *p, const char *Name, std::size_t NameSize )
  {
    while ( p && *p )
    {
      const char *Lt = strchr( p, '<' );
      if ( !Lt ) return NULL;

      if ( Lt[ 1 ] == '/' && strncmp( Lt + 2, Name, NameSize ) == 0 && Lt[ 2 + NameSize ] == '>' )
        return Lt + 2 + NameSize;

      if ( Lt[ 1 ] == '!' || Lt[ 1 ] == '?' )
      {
        const char *End = strchr( Lt, '>' );
        p = End ? End + 1 : NULL;
        continue;
      }

      p = Lt + 1;
    }

    return NULL;
  }

  // Appends the whitespace between Begin and End (used at the end of an
  // element's content and for the document tail), if it really is whitespace.
  void AppendWhitespace( RapidNode *Parent, RapidDoc *Doc, const char *Begin, const char *End )
  {
    if ( !Begin || !End || Begin >= End ) return;
    if ( !IsPureWhitespace( Begin, End ) ) return;

    RapidNode *t = Doc->allocate_node( rapidxml::node_data );
    t->value( Begin, (std::size_t)( End - Begin ) );
    Parent->append_node( t );
  }

  // Inserts the whitespace between Begin and End in front of Where, if it really
  // is whitespace.
  void InsertWhitespace( RapidNode *Parent, RapidDoc *Doc, RapidNode *Where, const char *Begin, const char *End )
  {
    if ( !Begin || !End || Begin >= End ) return;
    if ( !IsPureWhitespace( Begin, End ) ) return;

    RapidNode *t = Doc->allocate_node( rapidxml::node_data );
    t->value( Begin, (std::size_t)( End - Begin ) );
    Parent->insert_node( Where, t );
  }

  // Fills in the formatting whitespace of Element and returns the position just
  // past its closing '>' (or its self closing tag).
  //
  // The source ranges that belong to an element are
  //     [ContentStart, first child)   whitespace in front of the first child
  //     [child end, next child)       whitespace between two children
  //     [last child end, '</name>')   whitespace before the closing tag
  // and each of them is emitted exactly once here.  Whitespace is inserted in
  // front of the child it precedes so that the order in the tree is right; the
  // loop therefore walks the original siblings and remembers the next one
  // before it inserts anything.
  const char *RestoreElementBody( RapidNode *Element, RapidDoc *Doc, const char *Name, std::size_t NameSize,
                                  const char *SourceBegin, const char *SourceEnd );

  // True when Name points into the parsed source buffer; elements created at
  // runtime carry names from the document's string pool instead and have no
  // source text to recover formatting from.
  bool IsSourceBacked( const char *Name, const char *Buffer, const char *BufferEnd )
  {
    return Name && Name >= Buffer && Name < BufferEnd;
  }

  const char *RestoreElementBody( RapidNode *Element, RapidDoc *Doc, const char *Name, std::size_t NameSize,
                                  const char *SourceBegin, const char *SourceEnd )
  {
    const char *TagEnd = FindStartTagEnd( Name + NameSize );

    if ( TagEnd > Name && TagEnd[ -1 ] == '/' )
    {
      Element->value( SelfClosingMarker(), 0 );
      return TagEnd + 1;                        // self closing, nothing inside
    }

    MarkEmptyPair( Element );

    const char *Cursor = TagEnd + 1;             // content start

    for ( RapidNode *c = Element->first_node(); c; )
    {
      RapidNode *Next = c->next_sibling();

      const char *Start = NULL;
      const char *End = NULL;

      if ( c->type() == rapidxml::node_element )
      {
        if ( !IsSourceBacked( c->name(), SourceBegin, SourceEnd ) )
          continue;                              // built at runtime, nothing to restore

        // Element names point into the source, so an element's '<' is one byte
        // in front of its name.
        Start = c->name() - 1;
        End = RestoreElementBody( c, Doc, c->name(), c->name_size(), SourceBegin, SourceEnd );
      }
      else if ( c->type() == rapidxml::node_data || c->type() == rapidxml::node_cdata )
      {
        Start = c->value();
        End = c->value() + c->value_size();
      }
      else if ( c->name() )
      {
        Start = c->name();
        End = c->name() + c->name_size();
      }

      if ( Start )
      {
        InsertWhitespace( Element, Doc, c, Cursor, Start );

        if ( End > Cursor ) Cursor = End;
      }

      c = Next;
    }

    // Whitespace in front of the closing tag.
    const char *Close = FindElementClose( Cursor, Name, NameSize );
    if ( !Close ) return Cursor;

    AppendWhitespace( Element, Doc, Cursor, Close - NameSize - 2 );
    return Close + 1;
  }

  // Entry point.  The document node has no tag of its own: the root element is
  // the only element child, and anything in front of or behind it is whitespace
  // the document must carry.
  void RestoreFormatting( RapidNode *Document, RapidDoc *Doc, const char *SourceBegin, const char *SourceEnd )
  {
    RapidNode *Root = Document->first_node();
    if ( !Root || Root->type() != rapidxml::node_element )
      return;

    // A document that was built rather than parsed has no source text.
    if ( !IsSourceBacked( Root->name(), SourceBegin, SourceEnd ) )
      return;

    RestoreElementBody( Root, Doc, Root->name(), Root->name_size(), SourceBegin, SourceEnd );

    // Whitespace after the root element.
    const char *End = Root->name();
    while ( End && *End ) End++;

    const char *Close = FindElementClose( Root->name(), Root->name(), Root->name_size() );
    if ( !Close )
      return;

    const char *Cursor = Close + 1;
    const char *Tail = Cursor;
    while ( *Tail && *Tail != '<' ) Tail++;

    AppendWhitespace( Document, Doc, Cursor, Tail );
  }

  // rapidxml links attributes in the reverse of their document order.  MSXML
  // keeps the source order, so the list is rebuilt in the original order.
  void FixAttributeOrder( RapidNode *n, RapidDoc *Doc )
  {
    if ( n->type() != rapidxml::node_element || !n->first_attribute() ) return;

    std::vector<RapidAttr*> Attributes;
    for ( RapidAttr *a = n->last_attribute(); a; a = a->previous_attribute() )
      Attributes.push_back( a );

    for ( RapidAttr *a = n->first_attribute(); a; )
    {
      RapidAttr *Next = a->next_attribute();
      n->remove_attribute( a );
      a = Next;
    }

    for ( size_t i = Attributes.size(); i > 0; i-- )
    {
      RapidAttr *a = Doc->allocate_attribute(
        Attributes[ i - 1 ]->name(), Attributes[ i - 1 ]->value(),
        Attributes[ i - 1 ]->name_size(), Attributes[ i - 1 ]->value_size() );
      n->prepend_attribute( a );
    }
  }
}

// ---------------------------------------------------------------------------
// CXMLDocImpl
// ---------------------------------------------------------------------------

CXMLDocImpl::CXMLDocImpl()
{
  DocPtr = new RapidDoc();
  TextBuffer = NULL;
}

CXMLDocImpl::~CXMLDocImpl()
{
  delete (RapidDoc*)DocPtr;
  DocPtr = NULL;

  delete[] TextBuffer;
  TextBuffer = NULL;
}

void CXMLDocImpl::Clear()
{
  RapidDoc *d = (RapidDoc*)DocPtr;
  d->clear();

  delete[] TextBuffer;
  TextBuffer = NULL;

  Strings.clear();
  ParsedOk = false;
  ParseErrorLine = 0;
  ParseErrorColumn = 0;
  ParseErrorReason = _T( "" );
}

RapidNode *CXMLDocImpl::DocumentNode()
{
  return (RapidNode*)DocPtr;
}

RapidNode *CXMLDocImpl::CreateRootElement( const char *Name )
{
  RapidDoc *d = (RapidDoc*)DocPtr;

  // A freshly allocated xml_document carries no name; MSXML's document node
  // also refuses to serialise a name, so only the root element is created.
  const char *Stored = Intern( Name );
  RapidNode *n = d->allocate_node( rapidxml::node_element, Stored, NULL, strlen( Stored ) );
  d->append_node( n );
  return n;
}

char *CXMLDocImpl::AllocateString( TS32 Length )
{
  if ( Length < 0 ) Length = 0;
  return ( (RapidDoc*)DocPtr )->allocate_string( NULL, (std::size_t)Length + 1 );
}

const char *CXMLDocImpl::Intern( const char *Str )
{
  if ( !Str ) Str = "";
  return Strings.insert( std::string( Str ) ).first->c_str();
}

bool CXMLDocImpl::Parse( const char *Text, TU32 Length )
{
  // RapidXML keeps pointers into this buffer, so it has to live for as long as
  // the document does.
  char *Buffer = new char[ (size_t)Length + 1 ];
  if ( Text && Length )
    memcpy( Buffer, Text, Length );
  Buffer[ Length ] = 0;

  // Replace the document (and therefore the previously parsed text buffer) in
  // one step - the serialised output never includes the XML declaration,
  // matching MSXML.
  RapidDoc *Old = (RapidDoc*)DocPtr;
  RapidDoc *New = new RapidDoc();
  DocPtr = New;
  delete Old;

  Strings.clear();

  ParsedOk = false;
  ParseErrorLine = 0;
  ParseErrorColumn = 0;
  ParseErrorReason = _T( "" );

  try
  {
    // No string terminators so the source text (which the formatting recovery
    // below reads) stays untouched; entities are still translated, so text
    // values come out decoded exactly like MSXML returned them.
    New->parse<rapidxml::parse_no_string_terminators>( Buffer );

    delete[] TextBuffer;
    TextBuffer = Buffer;

    for ( RapidNode *c = New->first_node(); c; c = c->next_sibling() )
      FixAttributeOrder( c, New );

    RestoreFormatting( New, New, Buffer, Buffer + Length );
  }
  catch ( rapidxml::parse_error &e )
  {
    const char *Where = e.where<char>();
    const char *What = e.what();

    ParseErrorReason = CString( What ? What : "unknown error" );

    TS32 Line = 1, Column = 1;
    for ( const char *p = Buffer; p && Where && p < Where; p++ )
    {
      if ( *p == '\n' ) { Line++; Column = 1; }
      else Column++;
    }
    ParseErrorLine = Line;
    ParseErrorColumn = Column;

    if ( !TextBuffer ) TextBuffer = Buffer;
    else delete[] Buffer;

    ParsedOk = false;
    return false;
  }
  catch ( std::exception &e )
  {
    ParseErrorReason = CString( e.what() ? e.what() : "unknown error" );

    if ( !TextBuffer ) TextBuffer = Buffer;
    else delete[] Buffer;

    ParsedOk = false;
    return false;
  }

  ParsedOk = true;
  return true;
}

// ---------------------------------------------------------------------------
// serialisation
// ---------------------------------------------------------------------------

namespace
{
  // Appending into a CString reallocates on every call, which is quadratic for a
  // 13 MB project file, so the serialiser builds a std::string and only converts
  // once at the end.
  void WriteEscaped( std::string &Out, const char *s, TS32 len, bool Attribute )
  {
    for ( TS32 i = 0; i < len; i++ )
    {
      char c = s[ i ];
      switch ( c )
      {
      case '&': Out += "&amp;"; break;
      case '<': Out += "&lt;"; break;
      case '>': Out += "&gt;"; break;
      case '"': if ( Attribute ) Out += "&quot;"; else Out += c; break;
      default: Out += c; break;
      }
    }
  }
}

// Shared with XMLNode.cpp (CXMLNode::SaveToString) - not part of the public API.
void ApexXMLWriteNode( std::string &Out, const rapidxml::xml_node<char> *n )
{
  switch ( n->type() )
  {
  case rapidxml::node_data:
  case rapidxml::node_cdata:
  {
    if ( n->value_size() )
      WriteEscaped( Out, n->value(), (TS32)n->value_size(), false );
    break;
  }
  case rapidxml::node_element:
  {
    Out += '<';
    Out.append( n->name(), n->name_size() );

    for ( const rapidxml::xml_attribute<char> *a = n->first_attribute(); a; a = a->next_attribute() )
    {
      Out += ' ';
      Out.append( a->name(), a->name_size() );
      Out += "=\"";
      WriteEscaped( Out, a->value(), (TS32)a->value_size(), true );
      Out += '"';
    }

    // Element values are not used by this code base (SetText creates a text
    // child instead), but keep them for completeness.
    const rapidxml::xml_node<char> *First = n->first_node();
    if ( !First && n->value_size() )
    {
      Out += '>';
      WriteEscaped( Out, n->value(), (TS32)n->value_size(), false );
      Out += "</";
      Out.append( n->name(), n->name_size() );
      Out += '>';
      break;
    }

    if ( !First )
    {
      if ( IsSelfClosingNode( n ) )
        Out += "/>";
      else
      {
        Out += "></";
        Out.append( n->name(), n->name_size() );
        Out += '>';
      }
      break;
    }

    Out += '>';
    for ( const rapidxml::xml_node<char> *c = First; c; c = c->next_sibling() )
      ApexXMLWriteNode( Out, c );
    Out += "</";
    Out.append( n->name(), n->name_size() );
    Out += '>';
    break;
  }
  case rapidxml::node_comment:
    Out += "<!--";
    if ( n->value_size() ) Out.append( n->value(), n->value_size() );
    Out += "-->";
    break;
  case rapidxml::node_declaration:
    Out += "<?xml";
    for ( const rapidxml::xml_attribute<char> *a = n->first_attribute(); a; a = a->next_attribute() )
    {
      Out += ' ';
      Out.append( a->name(), a->name_size() );
      Out += "=\"";
      Out.append( a->value(), a->value_size() );
      Out += '"';
    }
    Out += "?>";
    break;
  case rapidxml::node_doctype:
    Out += "<!DOCTYPE";
    if ( n->value_size() ) { Out += ' '; Out.append( n->value(), n->value_size() ); }
    Out += '>';
    break;
  case rapidxml::node_pi:
    Out += "<?";
    Out.append( n->name(), n->name_size() );
    if ( n->value_size() ) { Out += ' '; Out.append( n->value(), n->value_size() ); }
    Out += "?>";
    break;
  default:
    break;
  }
}

void CXMLDocImpl::WriteString( CString &Out ) const
{
  std::string Buffer;

  const RapidNode *Doc = (const RapidNode*)DocPtr;
  for ( const RapidNode *c = Doc->first_node(); c; c = c->next_sibling() )
    ApexXMLWriteNode( Buffer, c );

  Out = CString( Buffer.c_str(), (TU32)Buffer.size() );
}

void CXMLDocImpl::WriteFile( FILE *f ) const
{
  CString s;
  WriteString( s );
  fwrite( s.GetPointer(), 1, (size_t)s.Length(), f );
}

// ---------------------------------------------------------------------------
// CXMLDocument - API compatible replacement of the MSXML implementation
// ---------------------------------------------------------------------------

CXMLDocument::CXMLDocument( void )
{
  pDoc = NULL;
  Allocate();
}

CXMLDocument::~CXMLDocument( void )
{
  Cleanup();
}

TBOOL CXMLDocument::Allocate()
{
  if ( pDoc ) return true;

  pDoc = new CXMLDocImpl();
  if ( !pDoc )
  {
    LOG_ERR( "[XML] Failed to create document object!" );
    return false;
  }

  return true;
}

TBOOL CXMLDocument::Cleanup()
{
  if ( pDoc )
  {
    delete pDoc;
    pDoc = NULL;
  }

  return true;
}

TBOOL CXMLDocument::LoadFromFile( TCHAR * szFileName )
{
  IGNOREFREEERRORS( true );

  if ( !pDoc )
  {
    IGNOREFREEERRORS( false );
    return false;
  }

  FILE *f = fopen( szFileName, "rb" );
  if ( !f )
  {
    LOG_ERR( "[XML] Failed to open document '%s'", szFileName );
    IGNOREFREEERRORS( false );
    return false;
  }

  fseek( f, 0, SEEK_END );
  long Size = ftell( f );
  fseek( f, 0, SEEK_SET );

  if ( Size < 0 )
  {
    fclose( f );
    LOG_ERR( "[XML] Failed to read document '%s'", szFileName );
    IGNOREFREEERRORS( false );
    return false;
  }

  char *Data = new char[ (size_t)Size + 1 ];
  size_t Read = Size ? fread( Data, 1, (size_t)Size, f ) : 0;
  fclose( f );
  Data[ Read ] = 0;

  bool Ok = pDoc->Parse( Data, (TU32)Read );

  delete[] Data;

  if ( !Ok )
  {
    LOG_ERR( "[XML] Load Failed at line %d column %d (%s)", pDoc->ParseErrorLine, pDoc->ParseErrorColumn,
             pDoc->ParseErrorReason.GetPointer() );
    IGNOREFREEERRORS( false );
    return false;
  }

  IGNOREFREEERRORS( false );
  return true;
}

TBOOL CXMLDocument::LoadFromString( CString s )
{
  IGNOREFREEERRORS( true );

  if ( !pDoc )
  {
    IGNOREFREEERRORS( false );
    return false;
  }

  bool Ok = pDoc->Parse( s.GetPointer(), (TU32)s.Length() );

  if ( !Ok )
    LOG_ERR( "[xml] Xml Parsing Failed: %s", pDoc->ParseErrorReason.GetPointer() );

  IGNOREFREEERRORS( false );
  return Ok ? true : false;
}

CXMLNode CXMLDocument::GetDocumentNode()
{
  if ( !pDoc ) return CXMLNode();
  return CXMLNode( pDoc->DocumentNode(), this, 0 );
}

CString CXMLDocument::SaveToString()
{
  CString s;
  if ( pDoc )
    pDoc->WriteString( s );
  return s;
}

TBOOL CXMLDocument::SaveToFile( TCHAR * sz )
{
  FILE *f = fopen( sz, "wb" );
  if ( !f ) return false;

  if ( pDoc )
    pDoc->WriteFile( f );

  fclose( f );
  return true;
}

CXMLNode CXMLDocument::CreateNode( VARIANT /*type*/, BSTR /*Name*/, TS32 Level )
{
  // Mirrors the original behaviour: creates an empty text node.
  if ( !pDoc ) return CXMLNode();

  RapidDoc *d = (RapidDoc*)pDoc->RawDoc();
  RapidNode *n = d->allocate_node( rapidxml::node_data );
  return CXMLNode( n, this, Level );
}
