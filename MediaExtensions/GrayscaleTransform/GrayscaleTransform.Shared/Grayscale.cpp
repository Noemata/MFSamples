// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.

#include "pch.h"
#include <wrl\module.h>
#include "Grayscale.h"
#include "VideoBufferLock.h"

#pragma comment(lib, "d2d1")

using namespace Microsoft::WRL;

ActivatableClass(CGrayscaleEffect);

/*

This sample implements a video effect as a Media Foundation transform (MFT).

NOTES ON THE MFT IMPLEMENTATION

1. The MFT has fixed streams: One input stream and one output stream. 

2. The MFT supports the following formats: UYVY, YUY2, NV12.

3. If the MFT is holding an input sample, SetInputType and SetOutputType both fail.

4. The input and output types must be identical.

5. If both types are set, no type can be set until the current type is cleared.

6. Preferred input types:
 
     (a) If the output type is set, that's the preferred type.
     (b) Otherwise, the preferred types are partial types, constructed from the 
         list of supported subtypes.
 
7. Preferred output types: As above.

8. Streaming: 
 
    The private BeingStreaming() method is called in response to the 
    MFT_MESSAGE_NOTIFY_BEGIN_STREAMING message. 

    If the client does not send MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, the MFT calls
    BeginStreaming inside the first call to ProcessInput or ProcessOutput. 

    This is a good approach for allocating resources that your MFT requires for
    streaming. 
    
9. The configuration attributes are applied in the BeginStreaming method. If the 
   client changes the attributes during streaming, the change is ignored until 
   streaming is stopped (either by changing the media types or by sending the 
   MFT_MESSAGE_NOTIFY_END_STREAMING message) and then restarted.
   
*/

// Place holder requisite ref class to make component usable. -------------------------
ref class HolderFunc sealed
{
public:	HolderFunc() {}
};

HolderFunc^ MakeHolder()
{
	return ref new HolderFunc();
}
// Note: A WinRT component compiled with C++/CX flags needs at least one ref class
//       to result in a usable binary else an exception will result in dllexports.cpp
//		 when the DLL is being loaded because the compiler is unable to create a valid
//		 C++/CX DLL due to no usage of C++/CX.  So use of C++/CX in this case is
//		 forward looking since we anticipate adding C++/CX code in the future and need
//		 a place holder for now.
// ------------------------------------------------------------------------------------

// Video FOURCC codes.
const DWORD FOURCC_YUY2 = '2YUY'; 
const DWORD FOURCC_UYVY = 'YVYU'; 
const DWORD FOURCC_NV12 = '21VN'; 

// Static array of media types (preferred and accepted).
const GUID g_MediaSubtypes[] =
{
    MFVideoFormat_NV12,
    MFVideoFormat_YUY2,
    MFVideoFormat_UYVY
};

DWORD GetImageSize(DWORD fcc, UINT32 width, UINT32 height);
LONG GetDefaultStride(IMFMediaType *pType);

template <typename T>
inline T clamp(const T &val, const T &minVal, const T &maxVal)
{
    return (val < minVal ? minVal : (val > maxVal ? maxVal : val));
}

//-------------------------------------------------------------------
// Functions to convert a YUV images to grayscale.
//
// In all cases, the same transformation is applied to the 8-bit
// chroma values, but the pixel layout in memory differs.
//
// The image conversion functions take the following parameters:
//
// mat               Transfomation matrix for chroma values.
// rcDest            Destination rectangle.
// pDest             Pointer to the destination buffer.
// lDestStride       Stride of the destination buffer, in bytes.
// pSrc              Pointer to the source buffer.
// lSrcStride        Stride of the source buffer, in bytes.
// dwWidthInPixels   Frame width in pixels.
// dwHeightInPixels  Frame height, in pixels.
//-------------------------------------------------------------------

// Convert UYVY image.

void TransformImage_UYVY(
    const D2D_RECT_U &rcDest,
    _Inout_updates_(_Inexpressible_(lDestStride * dwHeightInPixels)) BYTE *pDest, 
    _In_ LONG lDestStride, 
    _In_reads_(_Inexpressible_(lSrcStride * dwHeightInPixels)) const BYTE *pSrc,
    _In_ LONG lSrcStride, 
    _In_ DWORD dwWidthInPixels, 
    _In_ DWORD dwHeightInPixels)
{
    DWORD y = 0;
    // Round down to the even value.
    const UINT32 left = rcDest.left & ~(1);
    const UINT32 right = rcDest.right & ~(1);
    const DWORD y0 = min(rcDest.bottom, dwHeightInPixels);

    // Lines above the destination rectangle.
    for ( ; y < rcDest.top; y++)
    {
        CopyMemory(pDest, pSrc, dwWidthInPixels * 2);
        pSrc += lSrcStride;
        pDest += lDestStride;
    }

    // Lines within the destination rectangle.
    for ( ; y < y0; y++)
    {        
        const WORD *pSrc_Pixel = reinterpret_cast<const WORD*>(pSrc);
        WORD *pDest_Pixel = reinterpret_cast<WORD*>(pDest);

        CopyMemory(pDest, pSrc, left * 2);
        for (DWORD x = left; (x + 1) < right; x += 2)
        {
            // Byte order is Y0 U0 Y1 V0
            // Each WORD is a byte pair (Y, U/V)
            // Windows is little-endian so the order appears reversed.

            DWORD tmp = *reinterpret_cast<const DWORD*>(&pSrc_Pixel[x]);
            *reinterpret_cast<DWORD*>(&pDest_Pixel[x]) = (tmp & 0xFF00FF00) | 0x00800080;
        }
        CopyMemory(pDest + (right * 2), pSrc + (right * 2), (dwWidthInPixels - right) * 2);

        pDest += lDestStride;
        pSrc += lSrcStride;
    }

    // Lines below the destination rectangle.
    for ( ; y < dwHeightInPixels; y++)
    {
        CopyMemory(pDest, pSrc, dwWidthInPixels * 2);
        pSrc += lSrcStride;
        pDest += lDestStride;
    }
}


// Convert YUY2 image.

void TransformImage_YUY2(
    const D2D_RECT_U &rcDest,
    _Inout_updates_(_Inexpressible_(lDestStride * dwHeightInPixels)) BYTE *pDest, 
    _In_ LONG lDestStride, 
    _In_reads_(_Inexpressible_(lSrcStride * dwHeightInPixels)) const BYTE *pSrc,
    _In_ LONG lSrcStride, 
    _In_ DWORD dwWidthInPixels, 
    _In_ DWORD dwHeightInPixels)
{
    DWORD y = 0;
    // Round down to the even value.
    const UINT32 left = rcDest.left & ~(1);
    const UINT32 right = rcDest.right & ~(1);
    const DWORD y0 = min(rcDest.bottom, dwHeightInPixels);

    // Lines above the destination rectangle.
    for ( ; y < rcDest.top; y++)
    {
        CopyMemory(pDest, pSrc, dwWidthInPixels * 2);
        pSrc += lSrcStride;
        pDest += lDestStride;
    }

    // Lines within the destination rectangle.
    for ( ; y < y0; y++)
    {
        const WORD *pSrc_Pixel = reinterpret_cast<const WORD*>(pSrc);
        WORD *pDest_Pixel = reinterpret_cast<WORD*>(pDest);

        CopyMemory(pDest, pSrc, left * 2);
        for (DWORD x = left; (x + 1) < right; x += 2)
        {
            // Byte order is Y0 U0 Y1 V0
            // Each WORD is a byte pair (Y, U/V)
            // Windows is little-endian so the order appears reversed.

            DWORD tmp = *reinterpret_cast<const DWORD*>(&pSrc_Pixel[x]);
            *reinterpret_cast<DWORD*>(&pDest_Pixel[x]) = (tmp & 0x00FF00FF) | 0x80008000;
        }
        CopyMemory(pDest + (right * 2), pSrc + (right * 2), (dwWidthInPixels - right) * 2);

        pDest += lDestStride;
        pSrc += lSrcStride;
    }

    // Lines below the destination rectangle.
    for ( ; y < dwHeightInPixels; y++)
    {
        CopyMemory(pDest, pSrc, dwWidthInPixels * 2);
        pSrc += lSrcStride;
        pDest += lDestStride;
    }
}

// Convert NV12 image

void TransformImage_NV12(
    const D2D_RECT_U &rcDest,
    _Inout_updates_(_Inexpressible_(2 * lDestStride * dwHeightInPixels)) BYTE *pDest, 
    _In_ LONG lDestStride, 
    _In_reads_(_Inexpressible_(2 * lSrcStride * dwHeightInPixels)) const BYTE *pSrc,
    _In_ LONG lSrcStride, 
    _In_ DWORD dwWidthInPixels, 
    _In_ DWORD dwHeightInPixels)
{
    // NV12 is planar: Y plane, followed by packed U-V plane.

    // Y plane
    for (DWORD y = 0; y < dwHeightInPixels; y++)
    {
        CopyMemory(pDest, pSrc, dwWidthInPixels);
        pDest += lDestStride;
        pSrc += lSrcStride;
    }

    // U-V plane

    // NOTE: The U-V plane has 1/2 the number of lines as the Y plane.

    // Lines above the destination rectangle.
    DWORD y = 0;
    const DWORD y0 = min(rcDest.bottom, dwHeightInPixels);

    for ( ; y < rcDest.top/2; y++)
    {
        CopyMemory(pDest, pSrc, dwWidthInPixels);
        pSrc += lSrcStride;
        pDest += lDestStride;
    }

    // Lines within the destination rectangle.
    for ( ; y < y0/2; y++)
    {
        CopyMemory(pDest, pSrc, rcDest.left);
        FillMemory(pDest + rcDest.left, rcDest.right - rcDest.left, 128);
        CopyMemory(pDest + rcDest.right, pSrc + rcDest.right, dwWidthInPixels - rcDest.right);
        pDest += lDestStride;
        pSrc += lSrcStride;
    }

    // Lines below the destination rectangle.
    for ( ; y < dwHeightInPixels/2; y++)
    {
        CopyMemory(pDest, pSrc, dwWidthInPixels);
        pSrc += lSrcStride;
        pDest += lDestStride;
    }
}

CGrayscaleEffect::CGrayscaleEffect() 
    : m_pTransformFn(nullptr)
    , m_imageWidthInPixels(0)
    , m_imageHeightInPixels(0)
    , m_cbImageSize(0)
    , m_rcDest(D2D1::RectU())
    , m_fStreamingInitialized(false)
{
}

CGrayscaleEffect::~CGrayscaleEffect()
{
}

// Initialize the instance.
STDMETHODIMP CGrayscaleEffect::RuntimeClassInitialize()
{
    // Create the attribute store.
    return MFCreateAttributes(&m_spAttributes, 3);
}

// IMediaExtension methods

//-------------------------------------------------------------------
// SetProperties
// Sets the configuration of the effect
//-------------------------------------------------------------------
HRESULT CGrayscaleEffect::SetProperties(ABI::Windows::Foundation::Collections::IPropertySet *pConfiguration)
{
    return S_OK;
}

// IMFTransform methods. Refer to the Media Foundation SDK documentation for details.

//-------------------------------------------------------------------
// GetStreamLimits
// Returns the minimum and maximum number of streams.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetStreamLimits(
    DWORD   *pdwInputMinimum,
    DWORD   *pdwInputMaximum,
    DWORD   *pdwOutputMinimum,
    DWORD   *pdwOutputMaximum
)
{
    if ((pdwInputMinimum == nullptr) ||
        (pdwInputMaximum == nullptr) ||
        (pdwOutputMinimum == nullptr) ||
        (pdwOutputMaximum == nullptr))
    {
        return E_POINTER;
    }

    // This MFT has a fixed number of streams.
    *pdwInputMinimum = 1;
    *pdwInputMaximum = 1;
    *pdwOutputMinimum = 1;
    *pdwOutputMaximum = 1;
    return S_OK;
}


//-------------------------------------------------------------------
// GetStreamCount
// Returns the actual number of streams.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetStreamCount(
    DWORD   *pcInputStreams,
    DWORD   *pcOutputStreams
)
{
    if ((pcInputStreams == nullptr) || (pcOutputStreams == nullptr))

    {
        return E_POINTER;
    }

    // This MFT has a fixed number of streams.
    *pcInputStreams = 1;
    *pcOutputStreams = 1;
    return S_OK;
}



//-------------------------------------------------------------------
// GetStreamIDs
// Returns stream IDs for the input and output streams.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetStreamIDs(
    DWORD   dwInputIDArraySize,
    DWORD   *pdwInputIDs,
    DWORD   dwOutputIDArraySize,
    DWORD   *pdwOutputIDs
)
{
    // It is not required to implement this method if the MFT has a fixed number of
    // streams AND the stream IDs are numbered sequentially from zero (that is, the
    // stream IDs match the stream indexes).

    // In that case, it is OK to return E_NOTIMPL.
    return E_NOTIMPL;
}


//-------------------------------------------------------------------
// GetInputStreamInfo
// Returns information about an input stream.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetInputStreamInfo(
    DWORD                     dwInputStreamID,
    MFT_INPUT_STREAM_INFO *   pStreamInfo
)
{
    if (pStreamInfo == nullptr)
    {
        return E_POINTER;
    }

    AutoLock lock(m_critSec);

    if (!IsValidInputStream(dwInputStreamID))
    {
        return MF_E_INVALIDSTREAMNUMBER;
    }

    // NOTE: This method should succeed even when there is no media type on the
    //       stream. If there is no media type, we only need to fill in the dwFlags
    //       member of MFT_INPUT_STREAM_INFO. The other members depend on having a
    //       a valid media type.

    pStreamInfo->hnsMaxLatency = 0;
    pStreamInfo->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;

    if (m_spInputType == nullptr)
    {
        pStreamInfo->cbSize = 0;
    }
    else
    {
        pStreamInfo->cbSize = m_cbImageSize;
    }

    pStreamInfo->cbMaxLookahead = 0;
    pStreamInfo->cbAlignment = 0;

    return S_OK;
}

//-------------------------------------------------------------------
// GetOutputStreamInfo
// Returns information about an output stream.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetOutputStreamInfo(
    DWORD                     dwOutputStreamID,
    MFT_OUTPUT_STREAM_INFO *  pStreamInfo
)
{
    if (pStreamInfo == nullptr)
    {
        return E_POINTER;
    }

    AutoLock lock(m_critSec);

    if (!IsValidOutputStream(dwOutputStreamID))
    {
        return MF_E_INVALIDSTREAMNUMBER;
    }

    // NOTE: This method should succeed even when there is no media type on the
    //       stream. If there is no media type, we only need to fill in the dwFlags
    //       member of MFT_OUTPUT_STREAM_INFO. The other members depend on having a
    //       a valid media type.

    pStreamInfo->dwFlags =
        MFT_OUTPUT_STREAM_WHOLE_SAMPLES |
        MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER |
        MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE ;

    if (m_spOutputType == nullptr)
    {
        pStreamInfo->cbSize = 0;
    }
    else
    {
        pStreamInfo->cbSize = m_cbImageSize;
    }

    pStreamInfo->cbAlignment = 0;

    return S_OK;
}


//-------------------------------------------------------------------
// GetAttributes
// Returns the attributes for the MFT.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetAttributes(IMFAttributes **ppAttributes)
{
    if (ppAttributes == nullptr)
    {
        return E_POINTER;
    }

    AutoLock lock(m_critSec);

    *ppAttributes = m_spAttributes.Get();
    (*ppAttributes)->AddRef();

    return S_OK;
}


//-------------------------------------------------------------------
// GetInputStreamAttributes
// Returns stream-level attributes for an input stream.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetInputStreamAttributes(
    DWORD           dwInputStreamID,
    IMFAttributes   **ppAttributes
)
{
    // This MFT does not support any stream-level attributes, so the method is not implemented.
    return E_NOTIMPL;
}


//-------------------------------------------------------------------
// GetOutputStreamAttributes
// Returns stream-level attributes for an output stream.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetOutputStreamAttributes(
    DWORD           dwOutputStreamID,
    IMFAttributes   **ppAttributes
)
{
    // This MFT does not support any stream-level attributes, so the method is not implemented.
    return E_NOTIMPL;
}


//-------------------------------------------------------------------
// DeleteInputStream
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::DeleteInputStream(DWORD dwStreamID)
{
    // This MFT has a fixed number of input streams, so the method is not supported.
    return E_NOTIMPL;
}


//-------------------------------------------------------------------
// AddInputStreams
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::AddInputStreams(
    DWORD   cStreams,
    DWORD   *adwStreamIDs
)
{
    // This MFT has a fixed number of output streams, so the method is not supported.
    return E_NOTIMPL;
}


//-------------------------------------------------------------------
// GetInputAvailableType
// Returns a preferred input type.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetInputAvailableType(
    DWORD           dwInputStreamID,
    DWORD           dwTypeIndex, // 0-based
    IMFMediaType    **ppType
)
{
    HRESULT hr = S_OK;
    try
    {
        if (ppType == nullptr)
        {
            throw ref new InvalidArgumentException();
        }

        AutoLock lock(m_critSec);

        if (!IsValidInputStream(dwInputStreamID))
        {
            ThrowException(MF_E_INVALIDSTREAMNUMBER);
        }

        // If the output type is set, return that type as our preferred input type.
        if (m_spOutputType == nullptr)
        {
            // The output type is not set. Create a partial media type.
            *ppType = OnGetPartialType(dwTypeIndex).Detach();
        }
        else if (dwTypeIndex > 0)
        {
            return MF_E_NO_MORE_TYPES;
        }
        else
        {
            *ppType = m_spOutputType.Get();
            (*ppType)->AddRef();
        }
    }
    catch(Exception ^exc)
    {
        hr = exc->HResult;
    }

    return hr;
}



//-------------------------------------------------------------------
// GetOutputAvailableType
// Returns a preferred output type.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetOutputAvailableType(
    DWORD           dwOutputStreamID,
    DWORD           dwTypeIndex, // 0-based
    IMFMediaType    **ppType
)
{
    HRESULT hr = S_OK;

    try
    {
        if (ppType == nullptr)
        {
            throw ref new InvalidArgumentException();
        }

        AutoLock lock(m_critSec);

        if (!IsValidOutputStream(dwOutputStreamID))
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }

        if (m_spInputType == nullptr)
        {
            // The input type is not set. Create a partial media type.
            *ppType = OnGetPartialType(dwTypeIndex).Detach();
        }
        else if (dwTypeIndex > 0)
        {
            return MF_E_NO_MORE_TYPES;
        }
        else
        {
            *ppType = m_spInputType.Get();
            (*ppType)->AddRef();
        }
    }
    catch(Exception ^exc)
    {
        hr = exc->HResult;
    }

    return hr;
}


//-------------------------------------------------------------------
// SetInputType
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::SetInputType(
    DWORD           dwInputStreamID,
    IMFMediaType    *pType, // Can be nullptr to clear the input type.
    DWORD           dwFlags
)
{
    HRESULT hr = S_OK;

    try
    {
        // Validate flags.
        if (dwFlags & ~MFT_SET_TYPE_TEST_ONLY)
        {
            throw ref new InvalidArgumentException();
        }

        AutoLock lock(m_critSec);

        if (!IsValidInputStream(dwInputStreamID))
        {
            ThrowException(MF_E_INVALIDSTREAMNUMBER);
        }

        // Does the caller want us to set the type, or just test it?
        bool fReallySet = ((dwFlags & MFT_SET_TYPE_TEST_ONLY) == 0);

        // If we have an input sample, the client cannot change the type now.
        if (HasPendingOutput())
        {
            ThrowException(MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING);
        }

        // Validate the type, if non-nullptr.
        if (pType != nullptr)
        {
            OnCheckInputType(pType);
        }

        // The type is OK. Set the type, unless the caller was just testing.
        if (fReallySet)
        {
            OnSetInputType(pType);
            // When the type changes, end streaming.
            EndStreaming();
        }
    }
    catch(Exception ^exc)
    {
        hr = exc->HResult;
    }

    return hr;
}



//-------------------------------------------------------------------
// SetOutputType
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::SetOutputType(
    DWORD           dwOutputStreamID,
    IMFMediaType    *pType, // Can be nullptr to clear the output type.
    DWORD           dwFlags
)
{
    HRESULT hr = S_OK;

    try
    {
        if (!IsValidOutputStream(dwOutputStreamID))
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }

        // Validate flags.
        if (dwFlags & ~MFT_SET_TYPE_TEST_ONLY)
        {
            return E_INVALIDARG;
        }

        AutoLock lock(m_critSec);

        // Does the caller want us to set the type, or just test it?
        bool fReallySet = ((dwFlags & MFT_SET_TYPE_TEST_ONLY) == 0);

        // If we have an input sample, the client cannot change the type now.
        if (HasPendingOutput())
        {
            ThrowException(MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING);
        }

        // Validate the type, if non-nullptr.
        if (pType != nullptr)
        {
            OnCheckOutputType(pType);
        }

        if (fReallySet)
        {
            // The type is OK.
            // Set the type, unless the caller was just testing.
            OnSetOutputType(pType);

            EndStreaming();
        }
    }
    catch (Exception ^exc)
    {
        hr = exc->HResult;
    }

    return hr;
}


//-------------------------------------------------------------------
// GetInputCurrentType
// Returns the current input type.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetInputCurrentType(
    DWORD           dwInputStreamID,
    IMFMediaType    **ppType
)
{
    if (ppType == nullptr)
    {
        return E_POINTER;
    }

    HRESULT hr = S_OK;

    AutoLock lock(m_critSec);

    if (!IsValidInputStream(dwInputStreamID))
    {
        hr = MF_E_INVALIDSTREAMNUMBER;
    }
    else if (!m_spInputType)
    {
        hr = MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    else
    {
        *ppType = m_spInputType.Get();
        (*ppType)->AddRef();
    }

    return hr;
}


//-------------------------------------------------------------------
// GetOutputCurrentType
// Returns the current output type.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetOutputCurrentType(
    DWORD           dwOutputStreamID,
    IMFMediaType    **ppType
)
{
    if (ppType == nullptr)
    {
        return E_POINTER;
    }

    HRESULT hr = S_OK;

    AutoLock lock(m_critSec);

    if (!IsValidOutputStream(dwOutputStreamID))
    {
        hr = MF_E_INVALIDSTREAMNUMBER;
    }
    else if (!m_spOutputType)
    {
        hr = MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    else
    {
        *ppType = m_spOutputType.Get();
        (*ppType)->AddRef();
    }

    return hr;
}


//-------------------------------------------------------------------
// GetInputStatus
// Query if the MFT is accepting more input.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetInputStatus(
    DWORD           dwInputStreamID,
    DWORD           *pdwFlags
)
{
    if (pdwFlags == nullptr)
    {
        return E_POINTER;
    }

    AutoLock lock(m_critSec);

    if (!IsValidInputStream(dwInputStreamID))
    {
        return MF_E_INVALIDSTREAMNUMBER;
    }

    // If an input sample is already queued, do not accept another sample until the 
    // client calls ProcessOutput or Flush.

    // NOTE: It is possible for an MFT to accept more than one input sample. For 
    // example, this might be required in a video decoder if the frames do not 
    // arrive in temporal order. In the case, the decoder must hold a queue of 
    // samples. For the video effect, each sample is transformed independently, so
    // there is no reason to queue multiple input samples.

    if (m_spSample == nullptr)
    {
        *pdwFlags = MFT_INPUT_STATUS_ACCEPT_DATA;
    }
    else
    {
        *pdwFlags = 0;
    }

    return S_OK;
}



//-------------------------------------------------------------------
// GetOutputStatus
// Query if the MFT can produce output.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::GetOutputStatus(DWORD *pdwFlags)
{
    if (pdwFlags == nullptr)
    {
        return E_POINTER;
    }

    AutoLock lock(m_critSec);

    // The MFT can produce an output sample if (and only if) there an input sample.
    if (m_spSample != nullptr)
    {
        *pdwFlags = MFT_OUTPUT_STATUS_SAMPLE_READY;
    }
    else
    {
        *pdwFlags = 0;
    }

    return S_OK;
}


//-------------------------------------------------------------------
// SetOutputBounds
// Sets the range of time stamps that the MFT will output.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::SetOutputBounds(
    LONGLONG        hnsLowerBound,
    LONGLONG        hnsUpperBound
)
{
    // Implementation of this method is optional.
    return E_NOTIMPL;
}


//-------------------------------------------------------------------
// ProcessEvent
// Sends an event to an input stream.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::ProcessEvent(
    DWORD              dwInputStreamID,
    IMFMediaEvent      *pEvent
)
{
    // This MFT does not handle any stream events, so the method can
    // return E_NOTIMPL. This tells the pipeline that it can stop
    // sending any more events to this MFT.
    return E_NOTIMPL;
}


//-------------------------------------------------------------------
// ProcessMessage
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::ProcessMessage(
    MFT_MESSAGE_TYPE    eMessage,
    ULONG_PTR           ulParam
)
{
    AutoLock lock(m_critSec);

    HRESULT hr = S_OK;

    try
    {
        switch (eMessage)
        {
        case MFT_MESSAGE_COMMAND_FLUSH:
            // Flush the MFT.
            OnFlush();
            break;

        case MFT_MESSAGE_COMMAND_DRAIN:
            // Drain: Tells the MFT to reject further input until all pending samples are
            // processed. That is our default behavior already, so there is nothing to do.
            //
            // For a decoder that accepts a queue of samples, the MFT might need to drain
            // the queue in response to this command.
        break;

        case MFT_MESSAGE_SET_D3D_MANAGER:
            // Sets a pointer to the IDirect3DDeviceManager9 interface.

            // The pipeline should never send this message unless the MFT sets the MF_SA_D3D_AWARE 
            // attribute set to TRUE. Because this MFT does not set MF_SA_D3D_AWARE, it is an error
            // to send the MFT_MESSAGE_SET_D3D_MANAGER message to the MFT. Return an error code in
            // this case.

            // NOTE: If this MFT were D3D-enabled, it would cache the IMFDXGIDeviceManager
            // pointer for use during streaming.

            ThrowException(E_NOTIMPL);
            break;

        case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
            BeginStreaming();
            break;

        case MFT_MESSAGE_NOTIFY_END_STREAMING:
            EndStreaming();
            break;

        // The next two messages do not require any action from this MFT.

        case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
            break;

        case MFT_MESSAGE_NOTIFY_START_OF_STREAM:
            break;
        }
    }
    catch(Exception ^exc)
    {
        hr = exc->HResult;
    }

    return hr;
}


//-------------------------------------------------------------------
// ProcessInput
// Process an input sample.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::ProcessInput(
    DWORD               dwInputStreamID,
    IMFSample           *pSample,
    DWORD               dwFlags
)
{
    HRESULT hr = S_OK;

    try
    {
        // Check input parameters.
        if (pSample == nullptr)
        {
            throw ref new InvalidArgumentException();
        }

        if (dwFlags != 0)
        {
            throw ref new InvalidArgumentException(); // dwFlags is reserved and must be zero.
        }

        AutoLock lock(m_critSec);

        // Validate the input stream number.
        if (!IsValidInputStream(dwInputStreamID))
        {
            ThrowException(MF_E_INVALIDSTREAMNUMBER);
        }

        // Check for valid media types.
        // The client must set input and output types before calling ProcessInput.
        if (m_spInputType == nullptr || m_spOutputType == nullptr)
        {
            ThrowException(MF_E_NOTACCEPTING);   
        }

        // Check if an input sample is already queued.
        if (m_spSample != nullptr)
        {
            ThrowException(MF_E_NOTACCEPTING);   // We already have an input sample.
        }

        // Initialize streaming.
        BeginStreaming();

        // Cache the sample. We do the actual work in ProcessOutput.
        m_spSample = pSample;
    }
    catch(Exception ^exc)
    {
        hr = exc->HResult;
    }

    return hr;
}


//-------------------------------------------------------------------
// ProcessOutput
// Process an output sample.
//-------------------------------------------------------------------

HRESULT CGrayscaleEffect::ProcessOutput(
    DWORD                   dwFlags,
    DWORD                   cOutputBufferCount,
    MFT_OUTPUT_DATA_BUFFER  *pOutputSamples, // one per stream
    DWORD                   *pdwStatus
)
{
    HRESULT hr = S_OK;
    AutoLock lock(m_critSec);

    try
    {
        // Check input parameters...

        // This MFT does not accept any flags for the dwFlags parameter.

        // The only defined flag is MFT_PROCESS_OUTPUT_DISCARD_WHEN_NO_BUFFER. This flag 
        // applies only when the MFT marks an output stream as lazy or optional. But this
        // MFT has no lazy or optional streams, so the flag is not valid.

        if (dwFlags != 0)
        {
            throw ref new InvalidArgumentException();
        }

        if (pOutputSamples == nullptr || pdwStatus == nullptr)
        {
            throw ref new InvalidArgumentException();
        }

        // There must be exactly one output buffer.
        if (cOutputBufferCount != 1)
        {
            throw ref new InvalidArgumentException();
        }

        // It must contain a sample.
        if (pOutputSamples[0].pSample == nullptr)
        {
            throw ref new InvalidArgumentException();
        }

        ComPtr<IMFMediaBuffer> spInput;
        ComPtr<IMFMediaBuffer> spOutput;

        // There must be an input sample available for processing.
        if (m_spSample == nullptr)
        {
            return MF_E_TRANSFORM_NEED_MORE_INPUT;
        }

        // Initialize streaming.
        BeginStreaming();

        // Get the input buffer.
        ThrowIfError(m_spSample->ConvertToContiguousBuffer(&spInput));

        // Get the output buffer.
        ThrowIfError(pOutputSamples[0].pSample->ConvertToContiguousBuffer(&spOutput));

        OnProcessOutput(spInput.Get(), spOutput.Get());

        // Set status flags.
        pOutputSamples[0].dwStatus = 0;
        *pdwStatus = 0;


        // Copy the duration and time stamp from the input sample, if present.

        LONGLONG hnsDuration = 0;
        LONGLONG hnsTime = 0;

        if (SUCCEEDED(m_spSample->GetSampleDuration(&hnsDuration)))
        {
            ThrowIfError(pOutputSamples[0].pSample->SetSampleDuration(hnsDuration));
        }

        if (SUCCEEDED(m_spSample->GetSampleTime(&hnsTime)))
        {
            ThrowIfError(pOutputSamples[0].pSample->SetSampleTime(hnsTime));
        }
    }
    catch(Exception ^exc)
    {
        hr = exc->HResult;
    }

    m_spSample.Reset(); // Release our input sample.

    return hr;
}

// PRIVATE METHODS

// All methods that follow are private to this MFT and are not part of the IMFTransform interface.

// Create a partial media type from our list.
//
// dwTypeIndex: Index into the list of peferred media types.
// ppmt:        Receives a pointer to the media type.

ComPtr<IMFMediaType> CGrayscaleEffect::OnGetPartialType(DWORD dwTypeIndex)
{
    if (dwTypeIndex >= ARRAYSIZE(g_MediaSubtypes))
    {
        ThrowException(MF_E_NO_MORE_TYPES);
    }

    ComPtr<IMFMediaType> spMT;

    ThrowIfError(MFCreateMediaType(&spMT));

    ThrowIfError(spMT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));

    ThrowIfError(spMT->SetGUID(MF_MT_SUBTYPE, g_MediaSubtypes[dwTypeIndex]));

    return spMT;
}


// Validate an input media type.

void CGrayscaleEffect::OnCheckInputType(IMFMediaType *pmt)
{
    assert(pmt != nullptr);

    // If the output type is set, see if they match.
    if (m_spOutputType != nullptr)
    {
        DWORD flags = 0;
        // IsEqual can return S_FALSE. Treat this as failure.

		// MP! removed:
        //if (pmt->IsEqual(m_spOutputType.Get(), &flags) != S_OK)
        //{
        //    ThrowException(MF_E_INVALIDMEDIATYPE);
        //}
    }
    else
    {
        // Output type is not set. Just check this type.
        OnCheckMediaType(pmt);
    }
}


// Validate an output media type.

void CGrayscaleEffect::OnCheckOutputType(IMFMediaType *pmt)
{
    assert(pmt != nullptr);

    // If the input type is set, see if they match.
    if (m_spInputType != nullptr)
    {
        DWORD flags = 0;
        // IsEqual can return S_FALSE. Treat this as failure.
        if (pmt->IsEqual(m_spInputType.Get(), &flags) != S_OK)
        {
            ThrowException(MF_E_INVALIDMEDIATYPE);
        }
    }
    else
    {
        // Input type is not set. Just check this type.
        OnCheckMediaType(pmt);
    }
}


// Validate a media type (input or output)

void CGrayscaleEffect::OnCheckMediaType(IMFMediaType *pmt)
{
    bool fFoundMatchingSubtype = false;

    // Major type must be video.
    GUID major_type;
    ThrowIfError(pmt->GetGUID(MF_MT_MAJOR_TYPE, &major_type));

    if (major_type != MFMediaType_Video)
    {
        ThrowException(MF_E_INVALIDMEDIATYPE);
    }

    // Subtype must be one of the subtypes in our global list.

    // Get the subtype GUID.
    GUID subtype;
    ThrowIfError(pmt->GetGUID(MF_MT_SUBTYPE, &subtype));

    // Look for the subtype in our list of accepted types.
    for (DWORD i = 0; i < ARRAYSIZE(g_MediaSubtypes); i++)
    {
        if (subtype == g_MediaSubtypes[i])
        {
            fFoundMatchingSubtype = true;
            break;
        }
    }

    if (!fFoundMatchingSubtype)
    {
        ThrowException(MF_E_INVALIDMEDIATYPE); // The MFT does not support this subtype.
    }

    // Reject single-field media types. 
    UINT32 interlace = MFGetAttributeUINT32(pmt, MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (interlace == MFVideoInterlace_FieldSingleUpper  || interlace == MFVideoInterlace_FieldSingleLower)
    {
        ThrowException(MF_E_INVALIDMEDIATYPE);
    }
}


// Set or clear the input media type.
//
// Prerequisite: The input type was already validated.

void CGrayscaleEffect::OnSetInputType(IMFMediaType *pmt)
{
    // if pmt is nullptr, clear the type.
    // if pmt is non-nullptr, set the type.
    m_spInputType = pmt;

    // Update the format information.
    UpdateFormatInfo();
}


// Set or clears the output media type.
//
// Prerequisite: The output type was already validated.

void CGrayscaleEffect::OnSetOutputType(IMFMediaType *pmt)
{
    // If pmt is nullptr, clear the type. Otherwise, set the type.
    m_spOutputType = pmt;
}


// Initialize streaming parameters.
//
// This method is called if the client sends the MFT_MESSAGE_NOTIFY_BEGIN_STREAMING
// message, or when the client processes a sample, whichever happens first.

void CGrayscaleEffect::BeginStreaming()
{
    if (!m_fStreamingInitialized)
    {
        m_rcDest = D2D1::RectU(0, 0, m_imageWidthInPixels, m_imageHeightInPixels);
        m_fStreamingInitialized = true;
    }
}


// End streaming. 

// This method is called if the client sends an MFT_MESSAGE_NOTIFY_END_STREAMING
// message, or when the media type changes. In general, it should be called whenever
// the streaming parameters need to be reset.

void CGrayscaleEffect::EndStreaming()
{
    m_fStreamingInitialized = false;
}



// Generate output data.

void CGrayscaleEffect::OnProcessOutput(IMFMediaBuffer *pIn, IMFMediaBuffer *pOut)
{
    // Stride if the buffer does not support IMF2DBuffer
    const LONG lDefaultStride = GetDefaultStride(m_spInputType.Get());

    // Helper objects to lock the buffers.
    VideoBufferLock inputLock(pIn, MF2DBuffer_LockFlags_Read, m_imageHeightInPixels, lDefaultStride);
    VideoBufferLock outputLock(pOut, MF2DBuffer_LockFlags_Write, m_imageHeightInPixels, lDefaultStride);

    // Invoke the image transform function.
    assert (m_pTransformFn != nullptr);
    if (m_pTransformFn)
    {
        (*m_pTransformFn)(m_rcDest, outputLock.GetTopRow(), outputLock.GetStride(), inputLock.GetTopRow(), inputLock.GetStride(), m_imageWidthInPixels, m_imageHeightInPixels);
    }
    else
    {
        ThrowException(E_UNEXPECTED);
    }

    // Set the data size on the output buffer.
    ThrowIfError(pOut->SetCurrentLength(m_cbImageSize));

}


// Flush the MFT.

void CGrayscaleEffect::OnFlush()
{
    // For this MFT, flushing just means releasing the input sample.
    m_spSample.Reset();
}


// Update the format information. This method is called whenever the
// input type is set.

void CGrayscaleEffect::UpdateFormatInfo()
{
    GUID subtype = GUID_NULL;

    m_imageWidthInPixels = 0;
    m_imageHeightInPixels = 0;
    m_cbImageSize = 0;

    m_pTransformFn = nullptr;

    if (m_spInputType != nullptr)
    {
        ThrowIfError(m_spInputType->GetGUID(MF_MT_SUBTYPE, &subtype));
        if (subtype == MFVideoFormat_YUY2)
        {
            m_pTransformFn = TransformImage_YUY2;
        }
        else if (subtype == MFVideoFormat_UYVY)
        {
            m_pTransformFn = TransformImage_UYVY;
        }
        else if (subtype == MFVideoFormat_NV12)
        {
            m_pTransformFn = TransformImage_NV12;
        }
        else
        {
            ThrowException(E_UNEXPECTED);
        }

        ThrowIfError(MFGetAttributeSize(m_spInputType.Get(), MF_MT_FRAME_SIZE, &m_imageWidthInPixels, &m_imageHeightInPixels));

        // Calculate the image size (not including padding)
        m_cbImageSize = GetImageSize(subtype.Data1, m_imageWidthInPixels, m_imageHeightInPixels);
    }
}


// Calculate the size of the buffer needed to store the image.

// fcc: The FOURCC code of the video format.

DWORD GetImageSize(DWORD fcc, UINT32 width, UINT32 height)
{
    switch (fcc)
    {
    case FOURCC_YUY2:
    case FOURCC_UYVY:
        // check overflow
        if ((width > MAXDWORD / 2) || (width * 2 > MAXDWORD / height))
        {
            throw ref new InvalidArgumentException();
        }
        else
        {   
            // 16 bpp
            return width * height * 2;
        }

    case FOURCC_NV12:
        // check overflow
        if ((height/2 > MAXDWORD - height) || ((height + height/2) > MAXDWORD / width))
        {
            throw ref new InvalidArgumentException();
        }
        else
        {
            // 12 bpp
            return width * (height + (height/2));
        }

    default:
        // Unsupported type.
        ThrowException(MF_E_INVALIDTYPE);    
    }
    
    return 0;
}

// Get the default stride for a video format. 
LONG GetDefaultStride(IMFMediaType *pType)
{
    LONG lStride = 0;

    // Try to get the default stride from the media type.
    if (FAILED(pType->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&lStride)))
    {
        // Attribute not set. Try to calculate the default stride.
        GUID subtype = GUID_NULL;

        UINT32 width = 0;
        UINT32 height = 0;

        // Get the subtype and the image size.
        ThrowIfError(pType->GetGUID(MF_MT_SUBTYPE, &subtype));
        ThrowIfError(MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &width, &height));
        if (subtype == MFVideoFormat_NV12)
        {
            lStride = width;
        }
        else if (subtype == MFVideoFormat_YUY2 || subtype == MFVideoFormat_UYVY)
        {
            lStride = ((width * 2) + 3) & ~3;
        }
        else
        {
            throw ref new InvalidArgumentException();
        }

        // Set the attribute for later reference.
        (void)pType->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(lStride));
    }

    return lStride;
}
