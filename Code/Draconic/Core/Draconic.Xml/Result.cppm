// Draconic::Xml - :result partition
//
// XmlResult: the parse/lex outcome. Success is Ok; every other value is a
// specific (FourCC-coded) error. No exceptions - all XML operations return
// this. Ported from Sedulous.Xml/XmlResult.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.xml:result;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::xml
{
    enum class XmlResult : u32
    {
        Ok = 0,

        // General syntax
        SyntaxError = 0x53594E54,         // 'SYNT'
        UnexpectedEndOfFile = 0x554E4546, // 'UNEF'

        // Tags
        TagMismatch = 0x54414D53,        // 'TAMS'
        TagInvalid = 0x54414956,         // 'TAIV'
        TagUnclosed = 0x5441554E,        // 'TAUN'
        TagUnexpectedClose = 0x54415543, // 'TAUC'

        // Names
        NameEmpty = 0x4E4D454D,          // 'NMEM'
        NameIllegalChar = 0x4E4D4943,    // 'NMIC'
        NameReservedPrefix = 0x4E4D5250, // 'NMRP'

        // Attributes
        AttributeDuplicate = 0x41544450,     // 'ATDP'
        AttributeInvalid = 0x41544956,       // 'ATIV'
        AttributeValueInvalid = 0x41565649,  // 'AVVI'
        AttributeMissingEquals = 0x41544D45, // 'ATME'
        AttributeMissingQuote = 0x41544D51,  // 'ATMQ'

        // Entities / character references
        EntityUnknown = 0x454E554B,     // 'ENUK'
        EntityMalformed = 0x454E4D46,   // 'ENMF'
        CharRefInvalid = 0x43524956,    // 'CRIV'
        CharRefOutOfRange = 0x43524F52, // 'CROR'

        // Content
        CDataMalformed = 0x43444D46,         // 'CDMF'
        CDataUnclosed = 0x4344554E,          // 'CDUN'
        CommentMalformed = 0x434D4D46,       // 'CMMF'
        CommentUnclosed = 0x434D554E,        // 'CMUN'
        CommentIllegalSequence = 0x434D4953, // 'CMIS'
        PIInvalid = 0x50494956,              // 'PIIV'
        PIUnclosed = 0x5049554E,             // 'PIUN'

        // Declaration
        DeclarationInvalid = 0x4443494E,  // 'DCIN'
        DeclarationPosition = 0x44435053, // 'DCPS'
        DeclarationVersion = 0x44435652,  // 'DCVR'

        // Namespaces
        NamespaceUndeclared = 0x4E53554E,       // 'NSUN'
        NamespaceInvalid = 0x4E534956,          // 'NSIV'
        PrefixReserved = 0x50465253,            // 'PFRS'
        NamespaceDefaultUndeclare = 0x4E534455, // 'NSDU'

        // Structure
        MultipleRoots = 0x4D554C54,     // 'MULT'
        ContentBeforeRoot = 0x43425254, // 'CBRT'
        ContentAfterRoot = 0x43415254,  // 'CART'
        NoRootElement = 0x4E4F5254,     // 'NORT'

        // Encoding
        EncodingUnsupported = 0x454E5553, // 'ENUS'
        EncodingInvalidUtf8 = 0x454E5538, // 'ENU8'
    };

    [[nodiscard]] inline bool IsOk(XmlResult r) { return r == XmlResult::Ok; }
    [[nodiscard]] inline bool IsError(XmlResult r) { return r != XmlResult::Ok; }

    // Human-readable description of a result code.
    [[nodiscard]] inline StringView Describe(XmlResult r)
    {
        switch (r)
        {
        case XmlResult::Ok:
            return u8"Operation completed successfully";
        case XmlResult::SyntaxError:
            return u8"Syntax error";
        case XmlResult::UnexpectedEndOfFile:
            return u8"Unexpected end of file";
        case XmlResult::TagMismatch:
            return u8"Opening and closing tags do not match";
        case XmlResult::TagInvalid:
            return u8"Tag name is invalid";
        case XmlResult::TagUnclosed:
            return u8"Tag is not closed";
        case XmlResult::TagUnexpectedClose:
            return u8"Unexpected closing tag";
        case XmlResult::NameEmpty:
            return u8"No name found where one was expected";
        case XmlResult::NameIllegalChar:
            return u8"Name contains an illegal character";
        case XmlResult::NameReservedPrefix:
            return u8"Name starts with reserved prefix";
        case XmlResult::AttributeDuplicate:
            return u8"Duplicate attribute in element";
        case XmlResult::AttributeInvalid:
            return u8"Attribute name is invalid";
        case XmlResult::AttributeValueInvalid:
            return u8"Attribute value is invalid";
        case XmlResult::AttributeMissingEquals:
            return u8"Missing equals sign in attribute";
        case XmlResult::AttributeMissingQuote:
            return u8"Missing quote in attribute value";
        case XmlResult::EntityUnknown:
            return u8"Unknown entity reference";
        case XmlResult::EntityMalformed:
            return u8"Entity reference is malformed";
        case XmlResult::CharRefInvalid:
            return u8"Character reference is invalid";
        case XmlResult::CharRefOutOfRange:
            return u8"Character reference value is out of range";
        case XmlResult::CDataMalformed:
            return u8"CDATA section is malformed";
        case XmlResult::CDataUnclosed:
            return u8"CDATA section is not closed";
        case XmlResult::CommentMalformed:
            return u8"Comment is malformed";
        case XmlResult::CommentUnclosed:
            return u8"Comment is not closed";
        case XmlResult::CommentIllegalSequence:
            return u8"Comment contains illegal sequence (--)";
        case XmlResult::PIInvalid:
            return u8"Processing instruction is invalid";
        case XmlResult::PIUnclosed:
            return u8"Processing instruction is not closed";
        case XmlResult::DeclarationInvalid:
            return u8"XML declaration is invalid";
        case XmlResult::DeclarationPosition:
            return u8"XML declaration in wrong position";
        case XmlResult::DeclarationVersion:
            return u8"Version attribute is missing or invalid";
        case XmlResult::NamespaceUndeclared:
            return u8"Namespace prefix is undeclared";
        case XmlResult::NamespaceInvalid:
            return u8"Namespace declaration is invalid";
        case XmlResult::PrefixReserved:
            return u8"Prefix is reserved (xml, xmlns)";
        case XmlResult::NamespaceDefaultUndeclare:
            return u8"Cannot undeclare default namespace";
        case XmlResult::MultipleRoots:
            return u8"Document has multiple root elements";
        case XmlResult::ContentBeforeRoot:
            return u8"Content before root element";
        case XmlResult::ContentAfterRoot:
            return u8"Content after root element";
        case XmlResult::NoRootElement:
            return u8"Document has no root element";
        case XmlResult::EncodingUnsupported:
            return u8"Encoding is not supported";
        case XmlResult::EncodingInvalidUtf8:
            return u8"Invalid UTF-8 sequence";
        }
        return u8"Unknown error";
    }
}
