#ifndef AUDIO_AUDIOCLIPREADER_H
#define AUDIO_AUDIOCLIPREADER_H

#include <string>
#include <vector>

namespace aud
{
    class ClipReader
    {
    public:
        virtual ~ClipReader() {}

        // Return the source size in bytes. A ranged pak clip supplies its
        // entry length directly and does not need this query.
        virtual bool Size(const std::string& path, unsigned int& bytes) = 0;

        // Read a whole file when length is zero, otherwise read exactly the
        // requested byte range.
        virtual bool Read(const std::string& path, unsigned int offset,
                  unsigned int length, std::vector<unsigned char>& out) = 0;
    };

    class FileClipReader : public ClipReader
    {
    public:
        virtual bool Size(const std::string& path, unsigned int& bytes);
        virtual bool Read(const std::string& path, unsigned int offset,
                          unsigned int length, std::vector<unsigned char>& out);
    };

}

#endif // AUDIO_AUDIOCLIPREADER_H