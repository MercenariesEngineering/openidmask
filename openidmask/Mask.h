/*

             *
            ***
           *****
   *********************       Mercenaries Engineering SARL
     *****************              Copyright (C) 2016
       *************
         *********        http://www.mercenaries-engineering.com
        ***********
       ****     ****
      **           **

*/

#pragma once
#include "Sample.h"

#include <vector>
#include <map>
#include <ImfCompression.h>
#include <half.h>

namespace openidmask
{

// The Mask object hold the data needed to dynamically craft the mask images.
// The Mask is built using a Builder object. It can be loaded and saved in an EXR file.
class Mask
{
	friend class Query;
public:

	// Build an empty Mask
	Mask ();

	// Build a mask using the Builder.
	Mask (const class Builder &builder, const std::vector<std::string> &names);

	// Read a Mask from an EXR file.
	// This method throws exceptions in case of reading issues.
	void read (const char *filename);

	// Write the mask in an EXR file.
	void write (const char *filename, Imf::Compression compression=Imf::ZIPS_COMPRESSION) const;

	// Returns the image size
	// This method is thread safe
	inline std::pair<int,int>	getSize () const;

	// Returns the number of sample in the pixel
	// This method is thread safe
	inline int getSampleN (int x, int y) const;

	// Returns the pixel n-th sample
	// This method is thread safe
	// x and y and samples must be in the valid range
	inline const Sample getSample (int x, int y, int sample) const;

	// Returns the pixel n-th sample
	// x and y and samples must be in the valid range
	inline Sample getSample (int x, int y, int sample);

	// Returns the sample name
	// This method is thread safe
	// x and y and samples must be in the valid range
	// The returned pointer is valid until the Mask content is changed or destroyed
	inline const char *getSampleName (int x, int y, int sample) const;

	// Returns the id limit, i-e the largest id + 1.
	// This method is thread safe
	inline uint32_t getIdN () const;

	// Returns the name using a sample id.
	// The id should be < than getIdN(). "" is returned if no name is found for this id.
	// The returned pointer is valid until the Mask content is changed or destroyed
	// This method is thread safe
	inline const char *getName (uint32_t id) const;

private:

	// The image resolution
	int _Width, _Height;

	// For each name, the index of the begining of the string in the _Names buffer.
	std::vector<uint32_t>	_NamesIndexes;

	// All the names concatenated in a large string. The strings are C strings, 
	// with an ending \0 character.
	std::string				_Names;

	// For each pixels, the index of the first pixel sample in the _Samples vector.
	// The number of sample in the pixel p is (_PixelsIndexes[p+1]-_PixelsIndexes[p]).
	// _PixelsIndexes size is _Width*_Height+1.
	std::vector<uint32_t>	_PixelsIndexes;

	// The pixel id concatenated in a single vector.
	std::vector<uint32_t>	_Ids;

	// The pixel samples concatenated in a single vector.
	std::vector<half>	_Coverage;

	// Mask version
	const uint32_t	_Version = 1;
};

inline std::pair<int,int> Mask::getSize () const 
{
	return {_Width, _Height};
}

inline int Mask::getSampleN (int x, int y) const
{
	const int offset = x+y*_Width;
	return _PixelsIndexes[offset+1]-_PixelsIndexes[offset];
}

inline const Sample Mask::getSample (int x, int y, int sample) const
{
	const int index = _PixelsIndexes[x+y*_Width]+sample;
	return {_Ids[index], _Coverage[index]};
}

inline Sample Mask::getSample (int x, int y, int sample)
{
	const int index = _PixelsIndexes[x+y*_Width]+sample;
	return {_Ids[index], _Coverage[index]};
}

inline const char *Mask::getSampleName (int x, int y, int sample) const
{
	const Sample &s = getSample (x, y, sample);
	return getName (s.Id);
}

inline uint32_t Mask::getIdN () const
{
	return (uint32_t)_NamesIndexes.size ();
}

inline const char *Mask::getName (uint32_t id) const
{
	if (id < getIdN ())
		return &_Names[_NamesIndexes[id]];
	else
		return "";
}

}