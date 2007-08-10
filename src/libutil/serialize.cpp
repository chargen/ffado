/*
 * Copyright (C) 2005-2007 by Daniel Wagner
 *
 * This file is part of FFADO
 * FFADO = Free Firewire (pro-)audio drivers for linux
 *
 * FFADO is based upon FreeBoB.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software Foundation;
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include "serialize.h"

using namespace std;

void tokenize(const string& str,
              vector<string>& tokens,
              const string& delimiters = " ")
{
    // Skip delimiters at beginning.
    string::size_type lastPos = str.find_first_not_of(delimiters, 0);
    // Find first "non-delimiter".
    string::size_type pos     = str.find_first_of(delimiters, lastPos);

    while (string::npos != pos || string::npos != lastPos) {
        // Found a token, add it to the vector.
        tokens.push_back(str.substr(lastPos, pos - lastPos));
        // Skip delimiters.  Note the "not_of"
        lastPos = str.find_first_not_of(delimiters, pos);
        // Find next "non-delimiter"
        pos = str.find_first_of(delimiters, lastPos);
    }
}

/////////////////////////////////

IMPL_DEBUG_MODULE( Util::XMLSerialize,   XMLSerialize,   DEBUG_LEVEL_NORMAL );
IMPL_DEBUG_MODULE( Util::XMLDeserialize, XMLDeserialize, DEBUG_LEVEL_NORMAL );

Util::XMLSerialize::XMLSerialize( Glib::ustring fileName )
    : IOSerialize()
    , m_filepath( fileName )
    , m_verboseLevel( 0 )
{
    try {
        m_doc.create_root_node( "ffado_cache" );
    } catch ( const exception& ex ) {
        cout << "Exception caught: " << ex.what();
    }
}


Util::XMLSerialize::XMLSerialize( Glib::ustring fileName, int verboseLevel )
    : IOSerialize()
    , m_filepath( fileName )
    , m_verboseLevel( verboseLevel )
{
    try {
        m_doc.create_root_node( "ffado_cache" );
    } catch ( const exception& ex ) {
        cout << "Exception caught: " << ex.what();
    }
}

Util::XMLSerialize::~XMLSerialize()
{
    try {
        m_doc.write_to_file_formatted( m_filepath );
    } catch ( const exception& ex ) {
        cout << "Exception caugth: " << ex.what();
    }

}

bool
Util::XMLSerialize::write( std::string strMemberName,
                           long long value )

{
    debugOutput( DEBUG_LEVEL_VERY_VERBOSE, "write %s = %d\n",
                 strMemberName.c_str(), value );

    vector<string> tokens;
    tokenize( strMemberName, tokens, "/" );

    if ( tokens.size() == 0 ) {
        debugWarning( "token size is 0\n" );
        return false;
    }

    xmlpp::Node* pNode = m_doc.get_root_node();
    pNode = getNodePath( pNode, tokens );

    // element to be added
    xmlpp::Element* pElem = pNode->add_child( tokens[tokens.size() - 1] );
    char* valstr;
    asprintf( &valstr, "%lld", value );
    pElem->set_child_text( valstr );
    free( valstr );

    return true;
}

bool
Util::XMLSerialize::write( std::string strMemberName,
                           Glib::ustring str)
{
    debugOutput( DEBUG_LEVEL_VERY_VERBOSE, "write %s = %s\n",
                 strMemberName.c_str(), str.c_str() );

    vector<string> tokens;
    tokenize( strMemberName, tokens, "/" );

    if ( tokens.size() == 0 ) {
        debugWarning( "token size is 0\n" );
        return false;
    }

    xmlpp::Node* pNode = m_doc.get_root_node();
    pNode = getNodePath( pNode, tokens );

    // element to be added
    xmlpp::Element* pElem = pNode->add_child( tokens[tokens.size() - 1] );
    pElem->set_child_text( str );

    return true;
}

xmlpp::Node*
Util::XMLSerialize::getNodePath( xmlpp::Node* pRootNode,
                                 std::vector<string>& tokens )
{
    // returns the correct node on which the new element has to be added.
    // if the path does not exist, it will be created.

    if ( tokens.size() == 1 ) {
        return pRootNode;
    }

    unsigned int iTokenIdx = 0;
    xmlpp::Node* pCurNode = pRootNode;
    for (bool bFound = false;
         ( iTokenIdx < tokens.size() - 1 );
         bFound = false, iTokenIdx++ )
    {
        xmlpp::Node::NodeList nodeList = pCurNode->get_children();
        for ( xmlpp::Node::NodeList::iterator it = nodeList.begin();
              it != nodeList.end();
              ++it )
        {
            if ( ( *it )->get_name() == tokens[iTokenIdx] ) {
                pCurNode = *it;
                bFound = true;
                break;
            }
        }
        if ( !bFound ) {
            break;
        }
    }

    for ( unsigned int i = iTokenIdx; i < tokens.size() - 1; i++, iTokenIdx++ ) {
        pCurNode = pCurNode->add_child( tokens[iTokenIdx] );
    }
    return pCurNode;

}

/***********************************/

Util::XMLDeserialize::XMLDeserialize( Glib::ustring fileName )
    : IODeserialize()
    , m_filepath( fileName )
    , m_verboseLevel( 0 )
{
    try {
        m_parser.set_substitute_entities(); //We just want the text to
                                            //be resolved/unescaped
                                            //automatically.
        m_parser.parse_file( m_filepath );
    } catch ( const exception& ex ) {
        cout << "Exception caught: " << ex.what();
    }
}

Util::XMLDeserialize::XMLDeserialize( Glib::ustring fileName, int verboseLevel )
    : IODeserialize()
    , m_filepath( fileName )
    , m_verboseLevel( verboseLevel )
{
    try {
        m_parser.set_substitute_entities(); //We just want the text to
                                            //be resolved/unescaped
                                            //automatically.
        m_parser.parse_file( m_filepath );
    } catch ( const exception& ex ) {
        cout << "Exception caught: " << ex.what();
    }
}

Util::XMLDeserialize::~XMLDeserialize()
{
}

bool
Util::XMLDeserialize::read( std::string strMemberName,
                            long long& value )

{
    debugOutput( DEBUG_LEVEL_VERY_VERBOSE, "lookup %s\n", strMemberName.c_str() );

    xmlpp::Document *pDoc=m_parser.get_document();
    if(!pDoc) {
        debugWarning( "no document found\n" );
        return false;
    }
    xmlpp::Node* pNode = pDoc->get_root_node();

    debugOutput( DEBUG_LEVEL_VERY_VERBOSE, "pNode = %s\n", pNode->get_name().c_str() );

    xmlpp::NodeSet nodeSet = pNode->find( strMemberName );
    for ( xmlpp::NodeSet::iterator it = nodeSet.begin();
          it != nodeSet.end();
          ++it )
    {
        const xmlpp::Element* pElement =
            dynamic_cast< const xmlpp::Element* >( *it );
        if ( pElement && pElement->has_child_text() ) {
            char* tail;
            value = strtoll( pElement->get_child_text()->get_content().c_str(),
                             &tail, 0 );
            debugOutput( DEBUG_LEVEL_VERY_VERBOSE, "found %s = %d\n",
                         strMemberName.c_str(), value );
            return true;
        }
        debugWarning( "no such a node %s\n", strMemberName.c_str() );
        return false;
    }

    debugWarning( "no such a node %s\n", strMemberName.c_str() );
    return false;
}

bool
Util::XMLDeserialize::read( std::string strMemberName,
                            Glib::ustring& str )
{
    debugOutput( DEBUG_LEVEL_VERY_VERBOSE, "lookup %s\n", strMemberName.c_str() );

    xmlpp::Document *pDoc=m_parser.get_document();
    if(!pDoc) {
        debugWarning( "no document found\n" );
        return false;
    }
    xmlpp::Node* pNode = pDoc->get_root_node();

    xmlpp::NodeSet nodeSet = pNode->find( strMemberName );
    for ( xmlpp::NodeSet::iterator it = nodeSet.begin();
          it != nodeSet.end();
          ++it )
    {
        const xmlpp::Element* pElement = dynamic_cast< const xmlpp::Element* >( *it );
        if ( pElement ) {
            if ( pElement->has_child_text() ) {
                str = pElement->get_child_text()->get_content();
            } else {
                str = "";
            }
            debugOutput( DEBUG_LEVEL_VERY_VERBOSE, "found %s = %s\n",
                         strMemberName.c_str(), str.c_str() );
            return true;
        }
        debugWarning( "no such a node %s\n", strMemberName.c_str() );
        return false;
    }

    debugWarning( "no such a node %s\n", strMemberName.c_str() );
    return false;
}

bool
Util::XMLDeserialize::isExisting( std::string strMemberName )
{
    xmlpp::Document *pDoc=m_parser.get_document();
    if(!pDoc) {
        return false;
    }
    xmlpp::Node* pNode = pDoc->get_root_node();
    xmlpp::NodeSet nodeSet = pNode->find( strMemberName );
    return nodeSet.size() > 0;
}
