// CIncludeParser
//------------------------------------------------------------------------------

// Includes
//------------------------------------------------------------------------------
#include "Tools/FBuild/FBuildCore/PrecompiledHeader.h"

#include "CIncludeParser.h"

#include "Tools/FBuild/FBuildCore/FLog.h"
#include "Tools/FBuild/FBuildCore/Graph/NodeGraph.h"

// Core
#include "Core/FileIO/PathUtils.h"
#include "Core/Math/Murmur3.h"
#include "Core/Strings/AStackString.h"
#include "Core/Tracing/Tracing.h"

#include <string.h>

//------------------------------------------------------------------------------
CIncludeParser::CIncludeParser()
	: m_LastCRC1( 0 )
	, m_CRCs1( 4096, true )
	, m_LastCRC2( 0 )
	, m_CRCs2( 4096, true )
	, m_Includes( 4096, true )
#ifdef DEBUG
	, m_NonUniqueCount( 0 )
#endif
{
}

// DESTRUCTOR
//------------------------------------------------------------------------------
CIncludeParser::~CIncludeParser()
{
}

// Parse
//------------------------------------------------------------------------------
bool CIncludeParser::ParseMSCL_Output( const char * compilerOutput, 
									   size_t compilerOutputSize )
{
	// we require null terminated input
	ASSERT( compilerOutput[ compilerOutputSize ] == 0 );
	(void)compilerOutputSize;

	const char * pos = compilerOutput;
	//const char * end = pos + compilerOutputSize;
	for (;;)
	{
		const char * lineStart = pos;

		// find end of the line
		pos = strchr( pos, '\r' );
		if ( !pos )
		{
			break; // end of output
		}

		const char * lineEnd = pos;

		ASSERT( *pos == '\r' );
		++pos; // skip \r for next line

		const char * ch = lineStart;

		// count colons in the line
		const char * colon1 = nullptr;
		for ( ; ch < lineEnd ; ++ch )
		{
			if ( *ch == ':' )
			{
				if ( colon1 == nullptr )
				{
					colon1 = ch;
				}
				else
				{
					break;
				}
			}
		}

		// check that we have two colons separated by at least one char
		if ( colon1 == nullptr || colon1 == lineStart ||
			 *ch != ':' || (ch - colon1) < 2 )
		{
			continue; // next line
		}

		ASSERT( *ch == ':' );
		const char * colon2 = ch;

		// skip whitespace (always spaces)
		do
		{
			++ch;
		}
		while ( *ch == ' ' );

		// must have whitespaces
		if ( ch == colon2 )
		{
			continue; // next line
		}

		const char * includeStart = ch;
		const char * includeEnd = lineEnd;

		// validates the windows path
		bool validated = ( includeStart < includeEnd );
		size_t colonCount( 0 );
		for ( ; validated && ( ch < includeEnd ); ++ch )
		{
			switch ( *ch )
			{
    			// https://msdn.microsoft.com/en-us/library/windows/desktop/aa365247(v=vs.85).aspx
    			case '<':
    			case '>':
    			case '"':
    			case '|':
    			case '?':
    			case '*':
				{
    				validated = false;
    				break;
				}
				case ':':
				{
					// This logic handles warnings which might otherwise appear as valid paths
					++colonCount;
					if ( colonCount > 1 )
					{
	    				validated = false;
					}
					break;
				}
    			default:
    				break;
			}
		}

		if ( validated )
		{
			const char c1 = includeStart[ 0 ];
			const bool driveLetter = ( ( ( c1 >= 'A' ) && ( c1 <= 'Z' ) ) || ( ( c1 >= 'a' ) && ( c1 <= 'z' ) ) );
			const bool validPath = driveLetter && ( includeStart[ 1 ] == ':' );
			if ( validPath )
			{
				AddInclude( includeStart, includeEnd );
			}
		}
	}

	return true;
}

// Parse
//------------------------------------------------------------------------------
bool CIncludeParser::ParseMSCL_Preprocessed( const char * compilerOutput, 
											 size_t compilerOutputSize )
{
	// we require null terminated input
	ASSERT( compilerOutput[ compilerOutputSize ] == 0 );
	(void)compilerOutputSize;

	const char * pos = compilerOutput;

	// special case for include on first line
	// (out of loop to keep loop logic simple)
	if ( strncmp( pos, "#line ", 6 ) == 0 )
	{
		pos += 6;
		goto foundInclude;
	}

	for (;;)
	{
		pos = strstr( pos, "\n#line " );
		if ( !pos )
		{
			break;
		}
		pos += 7;

	foundInclude:

		// go to opening quote
		pos = strchr( pos, '"' );
		if ( !pos )
		{
			return false;
		}
		pos++;

		const char * lineStart = pos;

		// find end of line
		pos = strchr( pos, '"' );
		if ( !pos )
		{
			return false;
		}

		const char * lineEnd = pos;

		AddInclude( lineStart, lineEnd );
	}

	return true;
}

// Parse
//------------------------------------------------------------------------------
bool CIncludeParser::ParseGCC_Preprocessed( const char * compilerOutput, 
											size_t compilerOutputSize )
{
	// we require null terminated input	
	ASSERT( compilerOutput[ compilerOutputSize ] == 0 );
	(void)compilerOutputSize;

	const char * pos = compilerOutput;

	// special case for include on first line
	// (out of loop to keep loop logic simple)
	if ( pos[ 0 ] == '#' )
	{
		++pos;
		goto possibleInclude;
	}

	for (;;)
	{
		pos = strstr( pos, "\n#" );
		if ( !pos )
		{
			break;
		}
		pos += 2;
    possibleInclude:
        if ( *pos == ' ' )
        {
            ++pos;
            goto foundInclude;
        }
        if ( strncmp( pos, "line ", 5 ) == 0 )
        {
            pos += 5;
            goto foundInclude;
        }
        continue; // some other directive we don't care about

	foundInclude:

		// skip number
		for ( ;; )
		{
			char c = * pos;
			if ( ( c >= '0' ) && ( c <= '9' ) )
			{
				pos++;
				continue;
			}
			break; // non numeric
		}

		// single space
		if ( *pos != ' ' )
		{
			continue;
		}
		pos++;

		// opening quote
		if ( *pos != '"' )
		{
			continue;
		}
		pos++;

		// ignore special case GCC "<built-in>" and "<command line>"
		if ( *pos == '<' )
		{
			continue;
		}

		const char * lineStart = pos;

		// find end of line
		pos = strchr( pos, '"' );
		if ( !pos )
		{
			return false; // corrupt input
		}

		const char * lineEnd = pos;

		// ignore GCC paths
		const char lastChar( lineEnd[ -1 ] );
		if ( ( lastChar == NATIVE_SLASH ) || ( lastChar == OTHER_SLASH ) )
		{
			continue;
		}

		AddInclude( lineStart, lineEnd );
	}

	return true;
}

// SwapIncludes
//------------------------------------------------------------------------------
void CIncludeParser::SwapIncludes( Array< AString > & includes )
{
	m_Includes.Swap( includes );
}

// AddInclude
//------------------------------------------------------------------------------
void CIncludeParser::AddInclude( const char * begin, const char * end )
{
	#ifdef DEBUG
		m_NonUniqueCount++;
	#endif

	// quick check
	uint32_t crc1 = Murmur3::Calc32( begin, end - begin );
	if ( crc1 == m_LastCRC1 )
	{
		return;
	}
	m_LastCRC1 = crc1;
	if ( m_CRCs1.Find( crc1 ) )
	{
		return;
	}
	m_CRCs1.Append( crc1 );

	// robust check
	AStackString< 256 > include( begin, end );
	AStackString< 256 > cleanInclude;
	NodeGraph::CleanPath( include, cleanInclude );
	#if defined( __WINDOWS__ ) || defined( __OSX__ )
		// Windows and OSX are case-insensitive
		AStackString<> lowerCopy( cleanInclude );
		lowerCopy.ToLower();
		uint32_t crc2 = Murmur3::Calc32( lowerCopy );
	#else
		// Linux is case-sensitive
		uint32_t crc2 = Murmur3::Calc32( cleanInclude );
	#endif
	if ( crc2 == m_LastCRC2 )
	{
		return;
	}
	m_LastCRC2 = crc2;
	if ( m_CRCs2.Find( crc2 ) == nullptr )
	{
		m_CRCs2.Append( crc2 );
		m_Includes.Append( cleanInclude );
	}
}

//------------------------------------------------------------------------------
