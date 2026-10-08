// Round trip test for the RapidXML based CXMLDocument / CXMLNode replacement.
//
// The access pattern below mirrors what Phoenix_Tool's .apx loader does:
// children selected by tag name and occurrence index, attributes by name,
// text values read through GetText / GetValue.

#include "XMLDocument.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static TS32 Failures = 0;

static void Check( bool Condition, const char *What )
{
  if ( Condition )
  {
    printf( "  ok   %s\n", What );
    return;
  }

  printf( "  FAIL %s\n", What );
  Failures++;
}

static TS32 CountChildren( CXMLNode &Root, const char *Name )
{
  return Root.GetChildCount( (TCHAR*)Name );
}

struct Expected
{
  const char *Name;
  TS32 Count;
};

int main( int argc, char **argv )
{
  // The project file is NOT part of this repository: Clean Slate is Conspiracy's
  // released artwork under CC BY-NC (see .gitignore), so CI has no copy of it.
  // Pass a path, or set APEX_TEST_PROJECT, to exercise the file-based checks;
  // without one those checks are skipped rather than failed, so the synthetic
  // half of the suite still runs everywhere.  (This used to hardcode the
  // author's local path and therefore failed on every machine but his.)
  const char *Path = ( argc > 1 ) ? argv[ 1 ] : getenv( "APEX_TEST_PROJECT" );

  printf( "== apEx XML round trip test ==\n" );

  if ( !Path )
  {
    printf( "no project file given (argv[1] or APEX_TEST_PROJECT);\n"
            "running the synthetic checks only.\n" );
    Path = "/tmp/phx_no_project.apx"; // deliberately absent: skips the file half
  }

  printf( "input: %s\n", Path );

  FILE *f = fopen( Path, "rb" );
  if ( !f )
  {
    if ( argc > 1 || getenv( "APEX_TEST_PROJECT" ) )
    {
      printf( "FAIL: cannot open %s (you asked for it explicitly)\n", Path );
      return 1;
    }
    printf( "skipped: no project file at %s\n", Path );
    printf( "\n== SKIPPED (0 failures) ==\n" );
    return 0;
  }
  fseek( f, 0, SEEK_END );
  long FileSize = ftell( f );
  fseek( f, 0, SEEK_SET );
  char *Original = new char[ FileSize + 1 ];
  size_t Read = fread( Original, 1, (size_t)FileSize, f );
  fclose( f );
  Original[ Read ] = 0;
  printf( "size : %ld bytes\n", (long)Read );

  // -------------------------------------------------------------------------
  printf( "\n-- parse from file (LoadFromFile) --\n" );

  CXMLDocument Doc;
  Check( Doc.LoadFromFile( (TCHAR*)Path ) != 0, "LoadFromFile succeeded" );

  CXMLNode DocNode = Doc.GetDocumentNode();
  Check( DocNode.IsValid() != 0, "document node valid" );
  Check( DocNode.GetChildCount( (TCHAR*)"apExProject" ) == 1, "exactly one apExProject root" );

  CXMLNode Root = DocNode.GetChild( (TCHAR*)"apExProject" );
  Check( Root.IsValid() != 0, "root node valid" );
  Check( Root.GetNodeName() == CString( "apExProject" ), "root tag name is apExProject" );
  Check( Root.HasAttribute( (TCHAR*)"build" ) != 0, "root has build attribute" );

  CString Build = Root.GetAttribute( (TCHAR*)"build" );
  printf( "  build attribute = '%s'\n", Build.GetPointer() );
  Check( Build.Length() > 0, "build attribute is not empty" );

  // -------------------------------------------------------------------------
  printf( "\n-- name indexed child counts (loader access pattern) --\n" );

  Expected Expect[] =
  {
    { "model", 105 },
    { "material", 15 },
    { "rendertechnique", 34 },
    { "texturefilter", 37 },
    { "texturepage", 47 },
    { "rendertarget", 13 },
    { "renderlayer", 4 },
  };

  for ( int i = 0; i < (int)( sizeof( Expect ) / sizeof( Expect[ 0 ] ) ); i++ )
  {
    TS32 Count = CountChildren( Root, Expect[ i ].Name );
    char Buffer[ 128 ];
    snprintf( Buffer, sizeof( Buffer ), "root/%s count == %d", Expect[ i ].Name, Expect[ i ].Count );
    printf( "  %-22s = %d\n", Expect[ i ].Name, Count );
    Check( Count == Expect[ i ].Count, Buffer );
  }

  // -------------------------------------------------------------------------
  printf( "\n-- text values / deep path --\n" );

  CXMLNode Page = Root.GetChild( (TCHAR*)"texturepage", 0 );
  Check( Page.IsValid() != 0, "texturepage[0] valid" );

  CString PageName = Page.GetChild( (TCHAR*)"Name" ).GetText();
  printf( "  texturepage[0]/Name = '%s'\n", PageName.GetPointer() );
  Check( PageName == CString( "colors & gradients" ), "entity decoded text ('colors & gradients')" );

  CXMLNode Op = Page.GetChild( (TCHAR*)"Operator", 0 );
  Check( Op.IsValid() != 0, "texturepage[0]/Operator[0] valid" );

  CString Filter = Op.GetChild( (TCHAR*)"Filter" ).GetText();
  printf( "  texturepage[0]/Operator[0]/Filter = '%s'\n", Filter.GetPointer() );
  Check( Filter.Length() == 32, "Filter text is a 32 character GUID" );

  TS32 X1 = 0, Y1 = 0;
  Op.GetChild( (TCHAR*)"x1" ).GetValue( X1 );
  Op.GetChild( (TCHAR*)"y1" ).GetValue( Y1 );
  printf( "  texturepage[0]/Operator[0]/x1,y1 = %d,%d\n", X1, Y1 );
  Check( X1 == 15 && Y1 == 2, "numeric text values decode (15, 2)" );

  TS32 OpCount = Page.GetChildCount( (TCHAR*)"Operator" );
  printf( "  texturepage[0]/Operator count = %d\n", OpCount );
  Check( OpCount > 1, "multiple Operators found by name" );

  // -------------------------------------------------------------------------
  printf( "\n-- attributes by name --\n" );

  bool SawParameterId = false;
  bool SawParameterText = false;
  CXMLNode Param = Op.GetChild( (TCHAR*)"Parameter", 0 );
  if ( Param.IsValid() && Param.HasAttribute( (TCHAR*)"ID" ) )
  {
    TS32 Id = -1;
    Param.GetAttributeAsInteger( (TCHAR*)"ID", &Id );
    CString Value = Param.GetText();
    printf( "  texturepage[0]/Operator[0]/Parameter[0] ID=%d value='%s'\n", Id, Value.GetPointer() );
    SawParameterId = ( Id == 0 );
    SawParameterText = ( Value == CString( "255" ) );
  }
  Check( SawParameterId, "Parameter ID attribute read as integer" );
  Check( SawParameterText, "Parameter text value read" );

  // Pick a known numeric attribute from the loader's vocabulary.
  bool SawMaterialGuid = false;
  {
    CXMLNode Material = Root.GetChild( (TCHAR*)"material", 0 );
    if ( Material.IsValid() )
    {
      CString Guid = Material.GetChild( (TCHAR*)"GUID" ).GetText();
      printf( "  material[0]/GUID = '%s'\n", Guid.GetPointer() );
      SawMaterialGuid = Guid.Length() == 32;
    }
  }
  Check( SawMaterialGuid, "material GUID text read" );

  // transformation index="i" / Filter Type=".." / build=".."
  bool SawTransformationIndex = false;
  {
    for ( TS32 m = 0; m < CountChildren( Root, "model" ) && !SawTransformationIndex; m++ )
    {
      CXMLNode Model = Root.GetChild( (TCHAR*)"model", m );
      if ( !Model.IsValid() ) continue;

      for ( TS32 o = 0; o < Model.GetChildCount( (TCHAR*)"Object" ); o++ )
      {
        CXMLNode Object = Model.GetChild( (TCHAR*)"Object", o );
        if ( !Object.IsValid() ) continue;

        for ( TS32 t = 0; t < Object.GetChildCount( (TCHAR*)"transformation" ); t++ )
        {
          CXMLNode Tr = Object.GetChild( (TCHAR*)"transformation", t );
          if ( Tr.IsValid() && Tr.HasAttribute( (TCHAR*)"index" ) )
          {
            TS32 Index = -1;
            Tr.GetAttributeAsInteger( (TCHAR*)"index", &Index );
            printf( "  model[%d]/Object[%d]/transformation[%d] index=%d\n", m, o, t, Index );
            SawTransformationIndex = true;
            break;
          }
        }
      }
    }
  }
  Check( SawTransformationIndex, "transformation index attribute found" );

  bool SawFilterType = false;
  {
    CXMLNode Filter = Root.GetChild( (TCHAR*)"texturefilter", 0 ).GetChild( (TCHAR*)"Type" );
    if ( Filter.IsValid() )
    {
      CString T = Filter.GetText();
      printf( "  texturefilter[0]/Type = '%s'\n", T.GetPointer() );
      SawFilterType = T.Length() > 0;
    }
  }
  Check( SawFilterType, "texturefilter Type text found" );

  // -------------------------------------------------------------------------
  printf( "\n-- parse from memory (LoadFromString) --\n" );

  {
    CString Text( Original, (TU32)Read );
    CXMLDocument Doc2;
    Check( Doc2.LoadFromString( Text ) != 0, "LoadFromString succeeded" );

    CXMLNode Root2 = Doc2.GetDocumentNode().GetChild( (TCHAR*)"apExProject" );
    Check( Root2.GetChildCount( (TCHAR*)"model" ) == 105, "in-memory parse finds 105 models" );
    Check( Doc2.SaveToString().Length() == (TU32)Read, "serialised document has the input length" );
  }

  // -------------------------------------------------------------------------
  printf( "\n-- serialise -> compare with input --\n" );

  CString Serialised = Doc.SaveToString();
  printf( "  serialised = %u bytes (input %ld)\n", (unsigned)Serialised.Length(), (long)Read );
  Check( Serialised.Length() == (TU32)Read, "serialised length equals input length" );
  Check( Serialised.Length() == (TU32)Read && memcmp( Serialised.GetPointer(), Original, (size_t)Read ) == 0,
         "serialised bytes are identical to the input file" );

  // -------------------------------------------------------------------------
  printf( "\n-- parse -> serialise -> re-parse round trip --\n" );

  {
    CXMLDocument Doc3;
    Check( Doc3.LoadFromString( Serialised ) != 0, "re-parse of serialised document" );

    CXMLNode Root3 = Doc3.GetDocumentNode().GetChild( (TCHAR*)"apExProject" );
    Check( Root3.IsValid() != 0, "re-parsed root valid" );

    for ( int i = 0; i < (int)( sizeof( Expect ) / sizeof( Expect[ 0 ] ) ); i++ )
    {
      TS32 Count = CountChildren( Root3, Expect[ i ].Name );
      char Buffer[ 128 ];
      snprintf( Buffer, sizeof( Buffer ), "round trip root/%s count unchanged (%d)", Expect[ i ].Name, Expect[ i ].Count );
      Check( Count == Expect[ i ].Count, Buffer );
    }

    CString RoundTrip = Doc3.SaveToString();
    Check( RoundTrip.Length() == Serialised.Length() && memcmp( RoundTrip.GetPointer(), Serialised.GetPointer(), Serialised.Length() ) == 0,
           "second serialisation is byte identical to the first" );
  }

  // -------------------------------------------------------------------------
  printf( "\n-- document building (the Export path) --\n" );

  {
    CXMLDocument Out;
    CXMLNode Root2 = Out.GetDocumentNode();
    Root2 = Root2.AddChild( (TCHAR*)"apExProject" );
    Root2.SetAttribute( (TCHAR*)"build", "0498_19821r" );

    Root2.AddChild( (TCHAR*)"AspectX" ).SetInt( 16 );
    Root2.AddChild( (TCHAR*)"AspectY" ).SetInt( 9 );
    Root2.AddChild( (TCHAR*)"gridbpm" ).SetFloat( 120.0f );
    Root2.AddChild( (TCHAR*)"Title" ).SetText( "Clean Slate" );

    CXMLNode Filter = Root2.AddChild( (TCHAR*)"texturefilter" );
    Filter.AddChild( (TCHAR*)"Name" ).SetText( "colors & gradients" );
    Filter.AddChild( (TCHAR*)"Type" ).SetInt( 2 );
    CXMLNode Param = Filter.AddChild( (TCHAR*)"Parameter" );
    Param.SetAttributeFromInteger( (TCHAR*)"ID", 0 );
    Param.SetText( "255" );
    Param.SetAttributeFromFloat( (TCHAR*)"weight", 0.5f );

    CString Text = Out.SaveToString();
    printf( "%s\n", Text.GetPointer() );

    // The exact formatting MSXML produced for this sequence of AddChild calls:
    // a blank line between siblings (the CRLF of the trailing node plus the
    // CRLF of the next node's Pre), and the closing tag indented to the level
    // of the element's own parent.
Check( Text == CString( "\r\n<apExProject build=\"0498_19821r\">\r\n  <AspectX>16</AspectX>\r\n\r\n  <AspectY>9</AspectY>\r\n\r\n  <gridbpm>120</gridbpm>\r\n\r\n  <Title>Clean Slate</Title>\r\n\r\n  <texturefilter>\r\n    <Name>colors &amp; gradients</Name>\r\n  \r\n    <Type>2</Type>\r\n  \r\n    <Parameter ID=\"0\" weight=\"0.5\">255</Parameter>\r\n  </texturefilter>\r\n</apExProject>" ),
           "built document serialises exactly like MSXML did" );

    // Attributes by name on the freshly built document.
    Check( Filter.GetChild( (TCHAR*)"Parameter" ).GetAttribute( (TCHAR*)"ID" ) == CString( "0" ), "built attribute readable" );
    Check( Filter.GetChild( (TCHAR*)"Parameter" ).GetText() == CString( "255" ), "built text readable" );

    // Node level API: children selected by name must skip whitespace text nodes.
    Check( Filter.GetChild( (TCHAR*)"Parameter", 0 ).IsValid() != 0, "GetChild(name, 0) works" );
    Check( Filter.GetChild( (TCHAR*)"Parameter", 1 ).IsValid() == 0, "GetChild(name, 1) is invalid" );
    Check( Filter.GetChildCount() > Filter.GetChildCount( (TCHAR*)"Name" ), "GetChildCount() counts whitespace nodes too" );
  }

  // -------------------------------------------------------------------------
  printf( "\n-- node level SaveToString --\n" );

  {
    CXMLDocument Out;
    CXMLNode Root2 = Out.GetDocumentNode().AddChild( (TCHAR*)"root" );
    Root2.AddChild( (TCHAR*)"child" ).SetText( "text" );

    CString NodeText = Root2.SaveToString();
    printf( "  SaveToString(node) = '%s'\n", NodeText.GetPointer() );
    Check( NodeText == CString( "<root>\r\n  <child>text</child>\r\n</root>" ), "CXMLNode::SaveToString serialises the subtree" );
  }

  // -------------------------------------------------------------------------
  printf( "\n-- SaveToFile --\n" );

  {
    const char *TempFile = "work/xml_roundtrip_tmp.apx";
    Check( Doc.SaveToFile( (TCHAR*)TempFile ) != 0, "SaveToFile succeeded" );

    CXMLDocument Reloaded;
    Check( Reloaded.LoadFromFile( (TCHAR*)TempFile ) != 0, "saved file parses again" );
    Check( Reloaded.SaveToString().Length() == (TU32)Read, "saved file has the same length" );
  }

  delete[] Original;

  printf( "\n== %s (%d failure%s) ==\n", Failures ? "FAILED" : "PASSED", Failures, Failures == 1 ? "" : "s" );
  return Failures ? 1 : 0;
}
