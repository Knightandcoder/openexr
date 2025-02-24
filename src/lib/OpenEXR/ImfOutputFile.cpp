//
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Contributors to the OpenEXR Project.
//

//-----------------------------------------------------------------------------
//
//	class OutputFile
//
//-----------------------------------------------------------------------------

#include "ImfOutputFile.h"
#include "ImfChannelList.h"
#include "ImfHeader.h"
#include "ImfInputFile.h"

#include "Iex.h"
#include "IlmThreadPool.h"
#include "IlmThreadSemaphore.h"
#include "ImfArray.h"
#include "ImfCompressor.h"
#include "ImfFrameBuffer.h"
#include "ImfInputPart.h"
#include "ImfMisc.h"
#include "ImfOutputStreamMutex.h"
#include "ImfPartType.h"
#include "ImfPreviewImageAttribute.h"
#include "ImfStdIO.h"
#include "ImfXdr.h"
#include <ImathBox.h>
#include <ImathFun.h>

#include "ImfOutputPartData.h"

#include <algorithm>
#include <assert.h>
#include <fstream>
#include <string>
#include <vector>

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_ENTER

using ILMTHREAD_NAMESPACE::Semaphore;
using ILMTHREAD_NAMESPACE::Task;
using ILMTHREAD_NAMESPACE::TaskGroup;
using ILMTHREAD_NAMESPACE::ThreadPool;
using IMATH_NAMESPACE::Box2i;
using IMATH_NAMESPACE::divp;
using IMATH_NAMESPACE::modp;
using std::max;
using std::min;
using std::string;
using std::vector;

namespace
{

struct OutSliceInfo
{
    PixelType   type;
    const char* base;
    size_t      xStride;
    size_t      yStride;
    int         xSampling;
    int         ySampling;
    bool        zero;

    OutSliceInfo (
        PixelType   type      = HALF,
        const char* base      = 0,
        size_t      xStride   = 0,
        size_t      yStride   = 0,
        int         xSampling = 1,
        int         ySampling = 1,
        bool        zero      = false);
};

OutSliceInfo::OutSliceInfo (
    PixelType t, const char* b, size_t xs, size_t ys, int xsm, int ysm, bool z)
    : type (t)
    , base (b)
    , xStride (xs)
    , yStride (ys)
    , xSampling (xsm)
    , ySampling (ysm)
    , zero (z)
{
    // empty
}

struct LineBuffer
{
    Array<char> buffer;
    const char* dataPtr;
    int         dataSize;
    char*       endOfLineBufferData;
    int         minY;
    int         maxY;
    int         scanLineMin;
    int         scanLineMax;
    Compressor* compressor;
    bool        partiallyFull; // has incomplete data
    bool        hasException;
    string      exception;

    LineBuffer (Compressor* comp);
    ~LineBuffer ();

    void wait () { _sem.wait (); }
    void post () { _sem.post (); }

private:
    Semaphore _sem;
};

LineBuffer::LineBuffer (Compressor* comp)
    : dataPtr (0)
    , dataSize (0)
    , compressor (comp)
    , partiallyFull (false)
    , hasException (false)
    , exception ()
    , _sem (1)
{
    // empty
}

LineBuffer::~LineBuffer ()
{
    delete compressor;
}

} // namespace

struct OutputFile::Data
{
    Header      header;    // the image header
    bool        multiPart; // is the file multipart?
    int         version;   // version attribute \todo NOT BEING WRITTEN PROPERLY
    uint64_t    previewPosition;              // file position for preview
    FrameBuffer frameBuffer;                  // framebuffer to write into
    int         currentScanLine;              // next scanline to be written
    int         missingScanLines;             // number of lines to write
    LineOrder   lineOrder;                    // the file's lineorder
    int         minX;                         // data window's min x coord
    int         maxX;                         // data window's max x coord
    int         minY;                         // data window's min y coord
    int         maxY;                         // data window's max x coord
    vector<uint64_t> lineOffsets;             // stores offsets in file for
                                              // each scanline
    vector<size_t> bytesPerLine;              // combined size of a line over
                                              // all channels
    vector<size_t> offsetInLineBuffer;        // offset for each scanline in
                                              // its linebuffer
    Compressor::Format   format;              // compressor's data format
    vector<OutSliceInfo> slices;              // info about channels in file
    uint64_t             lineOffsetsPosition; // file position for line
                                              // offset table

    vector<LineBuffer*> lineBuffers;   // each holds one line buffer
    int                 linesInBuffer; // number of scanlines each
                                       // buffer holds
    size_t lineBufferSize;             // size of the line buffer

    int                partNumber; // the output part number
    OutputStreamMutex* _streamData;
    bool               _deleteStream;
    Data (int numThreads);
    ~Data ();

    Data (const Data& other) = delete;
    Data& operator= (const Data& other) = delete;
    Data (Data&& other)                 = delete;
    Data& operator= (Data&& other) = delete;

    inline LineBuffer* getLineBuffer (int number); // hash function from line
                                                   // buffer indices into our
                                                   // vector of line buffers
};

OutputFile::Data::Data (int numThreads)
    : lineOffsetsPosition (0)
    , partNumber (-1)
    , _streamData (0)
    , _deleteStream (false)
{
    //
    // We need at least one lineBuffer, but if threading is used,
    // to keep n threads busy we need 2*n lineBuffers.
    //

    lineBuffers.resize (max (1, 2 * numThreads));
}

OutputFile::Data::~Data ()
{
    for (size_t i = 0; i < lineBuffers.size (); i++)
        delete lineBuffers[i];
}

LineBuffer*
OutputFile::Data::getLineBuffer (int number)
{
    return lineBuffers[number % lineBuffers.size ()];
}

namespace
{

uint64_t
writeLineOffsets (
    OPENEXR_IMF_INTERNAL_NAMESPACE::OStream& os,
    const vector<uint64_t>&                  lineOffsets)
{
    uint64_t pos = os.tellp ();

    if (pos == static_cast<uint64_t> (-1))
        IEX_NAMESPACE::throwErrnoExc (
            "Cannot determine current file position (%T).");

    for (unsigned int i = 0; i < lineOffsets.size (); i++)
        Xdr::write<StreamIO> (os, lineOffsets[i]);

    return pos;
}

void
writePixelData (
    OutputStreamMutex* filedata,
    OutputFile::Data*  partdata,
    int                lineBufferMinY,
    const char         pixelData[],
    int                pixelDataSize)
{
    //
    // Store a block of pixel data in the output file, and try
    // to keep track of the current writing position the file
    // without calling tellp() (tellp() can be fairly expensive).
    //

    uint64_t currentPosition  = filedata->currentPosition;
    filedata->currentPosition = 0;

    if (currentPosition == 0) currentPosition = filedata->os->tellp ();

    partdata->lineOffsets
        [(partdata->currentScanLine - partdata->minY) /
         partdata->linesInBuffer] = currentPosition;

#ifdef DEBUG

    assert (filedata->os->tellp () == currentPosition);

#endif

    if (partdata->multiPart)
    {
        OPENEXR_IMF_INTERNAL_NAMESPACE::Xdr::write<
            OPENEXR_IMF_INTERNAL_NAMESPACE::StreamIO> (
            *filedata->os, partdata->partNumber);
    }

    OPENEXR_IMF_INTERNAL_NAMESPACE::Xdr::write<
        OPENEXR_IMF_INTERNAL_NAMESPACE::StreamIO> (
        *filedata->os, lineBufferMinY);
    OPENEXR_IMF_INTERNAL_NAMESPACE::Xdr::write<
        OPENEXR_IMF_INTERNAL_NAMESPACE::StreamIO> (
        *filedata->os, pixelDataSize);
    filedata->os->write (pixelData, pixelDataSize);

    filedata->currentPosition =
        currentPosition + Xdr::size<int> () + Xdr::size<int> () + pixelDataSize;

    if (partdata->multiPart) { filedata->currentPosition += Xdr::size<int> (); }
}

inline void
writePixelData (
    OutputStreamMutex* filedata,
    OutputFile::Data*  partdata,
    const LineBuffer*  lineBuffer)
{
    writePixelData (
        filedata,
        partdata,
        lineBuffer->minY,
        lineBuffer->dataPtr,
        lineBuffer->dataSize);
}

void
convertToXdr (
    OutputFile::Data* ofd,
    Array<char>&      lineBuffer,
    int               lineBufferMinY,
    int               lineBufferMaxY,
    int               inSize)
{
    //
    // Convert the contents of a lineBuffer from the machine's native
    // representation to Xdr format.  This function is called by
    // CompressLineBuffer::execute(), below, if the compressor wanted
    // its input pixel data in the machine's native format, but then
    // failed to compress the data (most compressors will expand rather
    // than compress random input data).
    //
    // Note that this routine assumes that the machine's native
    // representation of the pixel data has the same size as the
    // Xdr representation.  This makes it possible to convert the
    // pixel data in place, without an intermediate temporary buffer.
    //

    //
    // Iterate over all scanlines in the lineBuffer to convert.
    //

    char* writePtr = &lineBuffer[0];
    for (int y = lineBufferMinY; y <= lineBufferMaxY; y++)
    {
        //
        // Set these to point to the start of line y.
        // We will write to writePtr from readPtr.
        //

        const char* readPtr = writePtr;

        //
        // Iterate over all slices in the file.
        //

        for (unsigned int i = 0; i < ofd->slices.size (); ++i)
        {
            //
            // Test if scan line y of this channel is
            // contains any data (the scan line contains
            // data only if y % ySampling == 0).
            //

            const OutSliceInfo& slice = ofd->slices[i];

            if (modp (y, slice.ySampling) != 0) continue;

            //
            // Find the number of sampled pixels, dMaxX-dMinX+1, for
            // slice i in scan line y (i.e. pixels within the data window
            // for which x % xSampling == 0).
            //

            int dMinX = divp (ofd->minX, slice.xSampling);
            int dMaxX = divp (ofd->maxX, slice.xSampling);

            //
            // Convert the samples in place.
            //

            convertInPlace (writePtr, readPtr, slice.type, dMaxX - dMinX + 1);
        }
    }
}

//
// A LineBufferTask encapsulates the task of copying a set of scanlines
// from the user's frame buffer into a LineBuffer object, compressing
// the data if necessary.
//

class LineBufferTask : public Task
{
public:
    LineBufferTask (
        TaskGroup*        group,
        OutputFile::Data* ofd,
        int               number,
        int               scanLineMin,
        int               scanLineMax);

    virtual ~LineBufferTask ();

    virtual void execute ();

private:
    OutputFile::Data* _ofd;
    LineBuffer*       _lineBuffer;
};

LineBufferTask::LineBufferTask (
    TaskGroup*        group,
    OutputFile::Data* ofd,
    int               number,
    int               scanLineMin,
    int               scanLineMax)
    : Task (group), _ofd (ofd), _lineBuffer (_ofd->getLineBuffer (number))
{
    //
    // Wait for the lineBuffer to become available
    //

    _lineBuffer->wait ();

    //
    // Initialize the lineBuffer data if necessary
    //

    if (!_lineBuffer->partiallyFull)
    {
        _lineBuffer->endOfLineBufferData = _lineBuffer->buffer;

        _lineBuffer->minY = _ofd->minY + number * _ofd->linesInBuffer;

        _lineBuffer->maxY =
            min (_lineBuffer->minY + _ofd->linesInBuffer - 1, _ofd->maxY);

        _lineBuffer->partiallyFull = true;
    }

    _lineBuffer->scanLineMin = max (_lineBuffer->minY, scanLineMin);
    _lineBuffer->scanLineMax = min (_lineBuffer->maxY, scanLineMax);
}

LineBufferTask::~LineBufferTask ()
{
    //
    // Signal that the line buffer is now free
    //

    _lineBuffer->post ();
}

void
LineBufferTask::execute ()
{
    try
    {
        //
        // First copy the pixel data from the
        // frame buffer into the line buffer
        //

        int yStart, yStop, dy;

        if (_ofd->lineOrder == INCREASING_Y)
        {
            yStart = _lineBuffer->scanLineMin;
            yStop  = _lineBuffer->scanLineMax + 1;
            dy     = 1;
        }
        else
        {
            yStart = _lineBuffer->scanLineMax;
            yStop  = _lineBuffer->scanLineMin - 1;
            dy     = -1;
        }

        int y;

        for (y = yStart; y != yStop; y += dy)
        {
            //
            // Gather one scan line's worth of pixel data and store
            // them in _ofd->lineBuffer.
            //

            char* writePtr =
                _lineBuffer->buffer + _ofd->offsetInLineBuffer[y - _ofd->minY];
            //
            // Iterate over all image channels.
            //

            for (unsigned int i = 0; i < _ofd->slices.size (); ++i)
            {
                //
                // Test if scan line y of this channel contains any data
                // (the scan line contains data only if y % ySampling == 0).
                //

                const OutSliceInfo& slice = _ofd->slices[i];

                if (modp (y, slice.ySampling) != 0) continue;

                //
                // Find the x coordinates of the leftmost and rightmost
                // sampled pixels (i.e. pixels within the data window
                // for which x % xSampling == 0).
                //

                int dMinX = divp (_ofd->minX, slice.xSampling);
                int dMaxX = divp (_ofd->maxX, slice.xSampling);

                //
                // Fill the line buffer with with pixel data.
                //

                if (slice.zero)
                {
                    //
                    // The frame buffer contains no data for this channel.
                    // Store zeroes in _lineBuffer->buffer.
                    //

                    fillChannelWithZeroes (
                        writePtr, _ofd->format, slice.type, dMaxX - dMinX + 1);
                }
                else
                {
                    //
                    // If necessary, convert the pixel data to Xdr format.
                    // Then store the pixel data in _ofd->lineBuffer.
                    //
                    // slice.base may be 'negative' but
                    // pointer arithmetic is not allowed to overflow, so
                    // perform computation with the non-pointer 'intptr_t' instead
                    //
                    intptr_t base = reinterpret_cast<intptr_t> (slice.base);
                    intptr_t linePtr =
                        base + divp (y, slice.ySampling) * slice.yStride;

                    const char* readPtr = reinterpret_cast<const char*> (
                        linePtr + dMinX * slice.xStride);
                    const char* endPtr = reinterpret_cast<const char*> (
                        linePtr + dMaxX * slice.xStride);

                    copyFromFrameBuffer (
                        writePtr,
                        readPtr,
                        endPtr,
                        slice.xStride,
                        _ofd->format,
                        slice.type);
                }
            }

            if (_lineBuffer->endOfLineBufferData < writePtr)
                _lineBuffer->endOfLineBufferData = writePtr;

#ifdef DEBUG

            assert (
                writePtr - (_lineBuffer->buffer +
                            _ofd->offsetInLineBuffer[y - _ofd->minY]) ==
                (int) _ofd->bytesPerLine[y - _ofd->minY]);

#endif
        }

        //
        // If the next scanline isn't past the bounds of the lineBuffer
        // then we are done, otherwise compress the linebuffer
        //

        if (y >= _lineBuffer->minY && y <= _lineBuffer->maxY) return;

        _lineBuffer->dataPtr = _lineBuffer->buffer;

        _lineBuffer->dataSize =
            _lineBuffer->endOfLineBufferData - _lineBuffer->buffer;

        //
        // Compress the data
        //

        Compressor* compressor = _lineBuffer->compressor;

        if (compressor)
        {
            const char* compPtr;

            int compSize = compressor->compress (
                _lineBuffer->dataPtr,
                _lineBuffer->dataSize,
                _lineBuffer->minY,
                compPtr);

            if (compSize < _lineBuffer->dataSize)
            {
                _lineBuffer->dataSize = compSize;
                _lineBuffer->dataPtr  = compPtr;
            }
            else if (_ofd->format == Compressor::NATIVE)
            {
                //
                // The data did not shrink during compression, but
                // we cannot write to the file using the machine's
                // native format, so we need to convert the lineBuffer
                // to Xdr.
                //

                convertToXdr (
                    _ofd,
                    _lineBuffer->buffer,
                    _lineBuffer->minY,
                    _lineBuffer->maxY,
                    _lineBuffer->dataSize);
            }
        }

        _lineBuffer->partiallyFull = false;
    }
    catch (std::exception& e)
    {
        if (!_lineBuffer->hasException)
        {
            _lineBuffer->exception    = e.what ();
            _lineBuffer->hasException = true;
        }
    }
    catch (...)
    {
        if (!_lineBuffer->hasException)
        {
            _lineBuffer->exception    = "unrecognized exception";
            _lineBuffer->hasException = true;
        }
    }
}

} // namespace

OutputFile::OutputFile (
    const char fileName[], const Header& header, int numThreads)
    : _data (new Data (numThreads))
{
    _data->_streamData   = new OutputStreamMutex ();
    _data->_deleteStream = true;
    try
    {
        header.sanityCheck ();
        _data->_streamData->os = new StdOFStream (fileName);
        _data->multiPart       = false; // only one header, not multipart
        initialize (header);
        _data->_streamData->currentPosition = _data->_streamData->os->tellp ();

        // Write header and empty offset table to the file.
        writeMagicNumberAndVersionField (
            *_data->_streamData->os, _data->header);
        _data->previewPosition =
            _data->header.writeTo (*_data->_streamData->os);
        _data->lineOffsetsPosition =
            writeLineOffsets (*_data->_streamData->os, _data->lineOffsets);
    }
    catch (IEX_NAMESPACE::BaseExc& e)
    {
        // ~OutputFile will not run, so free memory here
        if (_data)
        {
            if (_data->_streamData)
            {
                delete _data->_streamData->os;
                delete _data->_streamData;
            }

            delete _data;
        }

        REPLACE_EXC (
            e,
            "Cannot open image file "
            "\"" << fileName
                 << "\". " << e.what ());
        throw;
    }
    catch (...)
    {
        // ~OutputFile will not run, so free memory here
        if (_data)
        {
            if (_data->_streamData)
            {
                delete _data->_streamData->os;
                delete _data->_streamData;
            }
            delete _data;
        }

        throw;
    }
}

OutputFile::OutputFile (
    OPENEXR_IMF_INTERNAL_NAMESPACE::OStream& os,
    const Header&                            header,
    int                                      numThreads)
    : _data (new Data (numThreads))
{
    _data->_streamData   = new OutputStreamMutex ();
    _data->_deleteStream = false;
    try
    {
        header.sanityCheck ();
        _data->_streamData->os = &os;
        _data->multiPart       = false;
        initialize (header);
        _data->_streamData->currentPosition = _data->_streamData->os->tellp ();

        // Write header and empty offset table to the file.
        writeMagicNumberAndVersionField (
            *_data->_streamData->os, _data->header);
        _data->previewPosition =
            _data->header.writeTo (*_data->_streamData->os);
        _data->lineOffsetsPosition =
            writeLineOffsets (*_data->_streamData->os, _data->lineOffsets);
    }
    catch (IEX_NAMESPACE::BaseExc& e)
    {
        // ~OutputFile will not run, so free memory here
        if (_data)
        {
            if (_data->_streamData) delete _data->_streamData;
            delete _data;
        }

        REPLACE_EXC (
            e,
            "Cannot open image file "
            "\"" << os.fileName ()
                 << "\". " << e.what ());
        throw;
    }
    catch (...)
    {
        // ~OutputFile will not run, so free memory here
        if (_data)
        {
            if (_data->_streamData) delete _data->_streamData;
            delete _data;
        }

        throw;
    }
}

OutputFile::OutputFile (const OutputPartData* part) : _data (NULL)
{
    try
    {
        if (part->header.type () != SCANLINEIMAGE)
            throw IEX_NAMESPACE::ArgExc (
                "Can't build a OutputFile from a type-mismatched part.");

        _data                = new Data (part->numThreads);
        _data->_streamData   = part->mutex;
        _data->_deleteStream = false;
        _data->multiPart     = part->multipart;

        initialize (part->header);
        _data->partNumber          = part->partNumber;
        _data->lineOffsetsPosition = part->chunkOffsetTablePosition;
        _data->previewPosition     = part->previewPosition;
    }
    catch (IEX_NAMESPACE::BaseExc& e)
    {
        if (_data) delete _data;

        REPLACE_EXC (
            e,
            "Cannot initialize output part "
            "\"" << part->partNumber
                 << "\". " << e.what ());
        throw;
    }
    catch (...)
    {
        if (_data) delete _data;

        throw;
    }
}

void
OutputFile::initialize (const Header& header)
{
    _data->header = header;

    // "fix" the type if it happens to be set incorrectly
    // (attribute is optional, but ensure it is correct if it exists)
    if (_data->header.hasType ()) { _data->header.setType (SCANLINEIMAGE); }

    const Box2i& dataWindow = header.dataWindow ();

    _data->currentScanLine = (header.lineOrder () == INCREASING_Y)
                                 ? dataWindow.min.y
                                 : dataWindow.max.y;

    _data->missingScanLines = dataWindow.max.y - dataWindow.min.y + 1;
    _data->lineOrder        = header.lineOrder ();
    _data->minX             = dataWindow.min.x;
    _data->maxX             = dataWindow.max.x;
    _data->minY             = dataWindow.min.y;
    _data->maxY             = dataWindow.max.y;

    size_t maxBytesPerLine =
        bytesPerLineTable (_data->header, _data->bytesPerLine);

    for (size_t i = 0; i < _data->lineBuffers.size (); ++i)
    {
        _data->lineBuffers[i] = new LineBuffer (newCompressor (
            _data->header.compression (), maxBytesPerLine, _data->header));
    }

    LineBuffer* lineBuffer = _data->lineBuffers[0];
    _data->format          = defaultFormat (lineBuffer->compressor);
    _data->linesInBuffer   = numLinesInBuffer (lineBuffer->compressor);
    _data->lineBufferSize  = maxBytesPerLine * _data->linesInBuffer;

    for (size_t i = 0; i < _data->lineBuffers.size (); i++)
        _data->lineBuffers[i]->buffer.resizeErase (_data->lineBufferSize);

    int lineOffsetSize =
        (dataWindow.max.y - dataWindow.min.y + _data->linesInBuffer) /
        _data->linesInBuffer;

    _data->lineOffsets.resize (lineOffsetSize);

    offsetInLineBufferTable (
        _data->bytesPerLine, _data->linesInBuffer, _data->offsetInLineBuffer);
}

OutputFile::~OutputFile ()
{
    if (_data)
    {
        {
#if ILMTHREAD_THREADING_ENABLED
            std::lock_guard<std::mutex> lock (*_data->_streamData);
#endif
            uint64_t originalPosition = _data->_streamData->os->tellp ();

            if (_data->lineOffsetsPosition > 0)
            {
                try
                {
                    _data->_streamData->os->seekp (_data->lineOffsetsPosition);
                    writeLineOffsets (
                        *_data->_streamData->os, _data->lineOffsets);

                    //
                    // Restore the original position.
                    //
                    _data->_streamData->os->seekp (originalPosition);
                }
                catch (
                    ...) //NOSONAR - suppress vulnerability reports from SonarCloud.
                {
                    //
                    // We cannot safely throw any exceptions from here.
                    // This destructor may have been called because the
                    // stack is currently being unwound for another
                    // exception.
                    //
                }
            }
        }

        if (_data->_deleteStream && _data->_streamData)
            delete _data->_streamData->os;

        if (_data->partNumber == -1 && _data->_streamData)
            delete _data->_streamData;

        delete _data;
    }
}

const char*
OutputFile::fileName () const
{
    return _data->_streamData->os->fileName ();
}

const Header&
OutputFile::header () const
{
    return _data->header;
}

void
OutputFile::setFrameBuffer (const FrameBuffer& frameBuffer)
{
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (*_data->_streamData);
#endif
    //
    // Check if the new frame buffer descriptor
    // is compatible with the image file header.
    //

    const ChannelList& channels = _data->header.channels ();

    for (ChannelList::ConstIterator i = channels.begin (); i != channels.end ();
         ++i)
    {
        FrameBuffer::ConstIterator j = frameBuffer.find (i.name ());

        if (j == frameBuffer.end ()) continue;

        if (i.channel ().type != j.slice ().type)
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Pixel type of \"" << i.name ()
                                   << "\" channel "
                                      "of output file \""
                                   << fileName ()
                                   << "\" is "
                                      "not compatible with the frame buffer's "
                                      "pixel type.");
        }

        if (i.channel ().xSampling != j.slice ().xSampling ||
            i.channel ().ySampling != j.slice ().ySampling)
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "X and/or y subsampling factors "
                "of \""
                    << i.name ()
                    << "\" channel "
                       "of output file \""
                    << fileName ()
                    << "\" are "
                       "not compatible with the frame buffer's "
                       "subsampling factors.");
        }
    }

    //
    // Initialize slice table for writePixels().
    //

    vector<OutSliceInfo> slices;

    for (ChannelList::ConstIterator i = channels.begin (); i != channels.end ();
         ++i)
    {
        FrameBuffer::ConstIterator j = frameBuffer.find (i.name ());

        if (j == frameBuffer.end ())
        {
            //
            // Channel i is not present in the frame buffer.
            // In the file, channel i will contain only zeroes.
            //

            slices.push_back (OutSliceInfo (
                i.channel ().type,
                0, // base
                0, // xStride,
                0, // yStride,
                i.channel ().xSampling,
                i.channel ().ySampling,
                true)); // zero
        }
        else
        {
            //
            // Channel i is present in the frame buffer.
            //

            slices.push_back (OutSliceInfo (
                j.slice ().type,
                j.slice ().base,
                j.slice ().xStride,
                j.slice ().yStride,
                j.slice ().xSampling,
                j.slice ().ySampling,
                false)); // zero
        }
    }

    //
    // Store the new frame buffer.
    //

    _data->frameBuffer = frameBuffer;
    _data->slices      = slices;
}

const FrameBuffer&
OutputFile::frameBuffer () const
{
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (*_data->_streamData);
#endif
    return _data->frameBuffer;
}

void
OutputFile::writePixels (int numScanLines)
{
    try
    {
#if ILMTHREAD_THREADING_ENABLED
        std::lock_guard<std::mutex> lock (*_data->_streamData);
#endif
        if (_data->slices.size () == 0)
            throw IEX_NAMESPACE::ArgExc (
                "No frame buffer specified as pixel data source.");

        //
        // Maintain two iterators:
        //     nextWriteBuffer: next linebuffer to be written to the file
        //     nextCompressBuffer: next linebuffer to compress
        //

        int first =
            (_data->currentScanLine - _data->minY) / _data->linesInBuffer;

        int nextWriteBuffer = first;
        int nextCompressBuffer;
        int stop;
        int step;
        int scanLineMin;
        int scanLineMax;

        {
            //
            // Create a task group for all line buffer tasks. When the
            // taskgroup goes out of scope, the destructor waits until
            // all tasks are complete.
            //

            TaskGroup taskGroup;

            //
            // Determine the range of lineBuffers that intersect the scan
            // line range.  Then add the initial compression tasks to the
            // thread pool.  We always add in at least one task but the
            // individual task might not do anything if numScanLines == 0.
            //

            if (_data->lineOrder == INCREASING_Y)
            {
                int last = (_data->currentScanLine + (numScanLines - 1) -
                            _data->minY) /
                           _data->linesInBuffer;

                scanLineMin = _data->currentScanLine;
                scanLineMax = _data->currentScanLine + numScanLines - 1;

                int numTasks = max (
                    min ((int) _data->lineBuffers.size (), last - first + 1),
                    1);

                for (int i = 0; i < numTasks; i++)
                {
                    ThreadPool::addGlobalTask (new LineBufferTask (
                        &taskGroup,
                        _data,
                        first + i,
                        scanLineMin,
                        scanLineMax));
                }

                nextCompressBuffer = first + numTasks;
                stop               = last + 1;
                step               = 1;
            }
            else
            {
                int last = (_data->currentScanLine - (numScanLines - 1) -
                            _data->minY) /
                           _data->linesInBuffer;

                scanLineMax = _data->currentScanLine;
                scanLineMin = _data->currentScanLine - numScanLines + 1;

                int numTasks = max (
                    min ((int) _data->lineBuffers.size (), first - last + 1),
                    1);

                for (int i = 0; i < numTasks; i++)
                {
                    ThreadPool::addGlobalTask (new LineBufferTask (
                        &taskGroup,
                        _data,
                        first - i,
                        scanLineMin,
                        scanLineMax));
                }

                nextCompressBuffer = first - numTasks;
                stop               = last - 1;
                step               = -1;
            }

            while (true)
            {
                if (_data->missingScanLines <= 0)
                {
                    throw IEX_NAMESPACE::ArgExc (
                        "Tried to write more scan lines "
                        "than specified by the data window.");
                }

                //
                // Wait until the next line buffer is ready to be written
                //

                LineBuffer* writeBuffer =
                    _data->getLineBuffer (nextWriteBuffer);

                writeBuffer->wait ();

                int numLines =
                    writeBuffer->scanLineMax - writeBuffer->scanLineMin + 1;

                _data->missingScanLines -= numLines;

                //
                // If the line buffer is only partially full, then it is
                // not complete and we cannot write it to disk yet.
                //

                if (writeBuffer->partiallyFull)
                {
                    _data->currentScanLine =
                        _data->currentScanLine + step * numLines;
                    writeBuffer->post ();

                    return;
                }

                //
                // Write the line buffer
                //

                writePixelData (_data->_streamData, _data, writeBuffer);
                nextWriteBuffer += step;

                _data->currentScanLine =
                    _data->currentScanLine + step * numLines;

#ifdef DEBUG

                assert (
                    _data->currentScanLine ==
                    ((_data->lineOrder == INCREASING_Y)
                         ? writeBuffer->scanLineMax + 1
                         : writeBuffer->scanLineMin - 1));

#endif

                //
                // Release the lock on the line buffer
                //

                writeBuffer->post ();

                //
                // If this was the last line buffer in the scanline range
                //

                if (nextWriteBuffer == stop) break;

                //
                // If there are no more line buffers to compress,
                // then only continue to write out remaining lineBuffers
                //

                if (nextCompressBuffer == stop) continue;

                //
                // Add nextCompressBuffer as a compression task
                //

                ThreadPool::addGlobalTask (new LineBufferTask (
                    &taskGroup,
                    _data,
                    nextCompressBuffer,
                    scanLineMin,
                    scanLineMax));

                //
                // Update the next line buffer we need to compress
                //

                nextCompressBuffer += step;
            }

            //
            // Finish all tasks
            //
        }

        //
        // Exeption handling:
        //
        // LineBufferTask::execute() may have encountered exceptions, but
        // those exceptions occurred in another thread, not in the thread
        // that is executing this call to OutputFile::writePixels().
        // LineBufferTask::execute() has caught all exceptions and stored
        // the exceptions' what() strings in the line buffers.
        // Now we check if any line buffer contains a stored exception; if
        // this is the case then we re-throw the exception in this thread.
        // (It is possible that multiple line buffers contain stored
        // exceptions.  We re-throw the first exception we find and
        // ignore all others.)
        //

        const string* exception = 0;

        for (size_t i = 0; i < _data->lineBuffers.size (); ++i)
        {
            LineBuffer* lineBuffer = _data->lineBuffers[i];

            if (lineBuffer->hasException && !exception)
                exception = &lineBuffer->exception;

            lineBuffer->hasException = false;
        }

        if (exception) throw IEX_NAMESPACE::IoExc (*exception);
    }
    catch (IEX_NAMESPACE::BaseExc& e)
    {
        REPLACE_EXC (
            e,
            "Failed to write pixel data to image "
            "file \""
                << fileName () << "\". " << e.what ());
        throw;
    }
}

int
OutputFile::currentScanLine () const
{
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (*_data->_streamData);
#endif
    return _data->currentScanLine;
}

void
OutputFile::copyPixels (InputFile& in)
{
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (*_data->_streamData);
#endif
    //
    // Check if this file's and and the InputFile's
    // headers are compatible.
    //

    const Header& hdr   = _data->header;
    const Header& inHdr = in.header ();

    if (inHdr.find ("tiles") != inHdr.end ())
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Cannot copy pixels from image "
            "file \""
                << in.fileName ()
                << "\" to image "
                   "file \""
                << fileName ()
                << "\". "
                   "The input file is tiled, but the output file is "
                   "not. Try using TiledOutputFile::copyPixels "
                   "instead.");

    if (!(hdr.dataWindow () == inHdr.dataWindow ()))
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Cannot copy pixels from image "
            "file \""
                << in.fileName ()
                << "\" to image "
                   "file \""
                << fileName ()
                << "\". "
                   "The files have different data windows.");

    if (!(hdr.lineOrder () == inHdr.lineOrder ()))
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Quick pixel copy from image "
            "file \""
                << in.fileName ()
                << "\" to image "
                   "file \""
                << fileName ()
                << "\" failed. "
                   "The files have different line orders.");

    if (!(hdr.compression () == inHdr.compression ()))
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Quick pixel copy from image "
            "file \""
                << in.fileName ()
                << "\" to image "
                   "file \""
                << fileName ()
                << "\" failed. "
                   "The files use different compression methods.");

    if (!(hdr.channels () == inHdr.channels ()))
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Quick pixel copy from image "
            "file \""
                << in.fileName ()
                << "\" to image "
                   "file \""
                << fileName ()
                << "\" failed.  "
                   "The files have different channel lists.");

    //
    // Verify that no pixel data have been written to this file yet.
    //

    const Box2i& dataWindow = hdr.dataWindow ();

    if (_data->missingScanLines != dataWindow.max.y - dataWindow.min.y + 1)
        THROW (
            IEX_NAMESPACE::LogicExc,
            "Quick pixel copy from image "
            "file \""
                << in.fileName ()
                << "\" to image "
                   "file \""
                << fileName ()
                << "\" failed. "
                   "\""
                << fileName ()
                << "\" already contains "
                   "pixel data.");

    //
    // Copy the pixel data.
    //

    while (_data->missingScanLines > 0)
    {
        const char* pixelData;
        int         pixelDataSize;

        in.rawPixelData (_data->currentScanLine, pixelData, pixelDataSize);

        writePixelData (
            _data->_streamData,
            _data,
            lineBufferMinY (
                _data->currentScanLine, _data->minY, _data->linesInBuffer),
            pixelData,
            pixelDataSize);

        _data->currentScanLine += (_data->lineOrder == INCREASING_Y)
                                      ? _data->linesInBuffer
                                      : -_data->linesInBuffer;

        _data->missingScanLines -= _data->linesInBuffer;
    }
}

void
OutputFile::copyPixels (InputPart& in)
{
    copyPixels (*in.file);
}

void
OutputFile::updatePreviewImage (const PreviewRgba newPixels[])
{
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (*_data->_streamData);
#endif
    if (_data->previewPosition <= 0)
        THROW (
            IEX_NAMESPACE::LogicExc,
            "Cannot update preview image pixels. "
            "File \""
                << fileName ()
                << "\" does not "
                   "contain a preview image.");

    //
    // Store the new pixels in the header's preview image attribute.
    //

    PreviewImageAttribute& pia =
        _data->header.typedAttribute<PreviewImageAttribute> ("preview");

    PreviewImage& pi        = pia.value ();
    PreviewRgba*  pixels    = pi.pixels ();
    int           numPixels = pi.width () * pi.height ();

    for (int i = 0; i < numPixels; ++i)
        pixels[i] = newPixels[i];

    //
    // Save the current file position, jump to the position in
    // the file where the preview image starts, store the new
    // preview image, and jump back to the saved file position.
    //

    uint64_t savedPosition = _data->_streamData->os->tellp ();

    try
    {
        _data->_streamData->os->seekp (_data->previewPosition);
        pia.writeValueTo (*_data->_streamData->os, _data->version);
        _data->_streamData->os->seekp (savedPosition);
    }
    catch (IEX_NAMESPACE::BaseExc& e)
    {
        REPLACE_EXC (
            e,
            "Cannot update preview image pixels for "
            "file \""
                << fileName () << "\". " << e.what ());
        throw;
    }
}

void
OutputFile::breakScanLine (int y, int offset, int length, char c)
{
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (*_data->_streamData);
#endif
    uint64_t position =
        _data->lineOffsets[(y - _data->minY) / _data->linesInBuffer];

    if (!position)
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Cannot overwrite scan line "
                << y
                << ". "
                   "The scan line has not yet been stored in "
                   "file \""
                << fileName () << "\".");

    _data->_streamData->currentPosition = 0;
    _data->_streamData->os->seekp (position + offset);

    for (int i = 0; i < length; ++i)
        _data->_streamData->os->write (&c, 1);
}

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_EXIT
