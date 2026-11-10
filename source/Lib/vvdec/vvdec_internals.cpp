/* -----------------------------------------------------------------------------
The copyright in this software is being made available under the Clear BSD
License, included below. No patent rights, trademark rights and/or
other Intellectual Property Rights other than the copyrights concerning the
Software are granted under this license.

The Clear BSD License

Copyright (c) 2018-2026, Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V. & The VVdeC Authors.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted (subject to the limitations in the disclaimer below) provided that
the following conditions are met:

     * Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

     * Redistributions in binary form must reproduce the above copyright
     notice and this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.

     * Neither the name of the copyright holder nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.


------------------------------------------------------------------------------------------- */
/** \file     vvdec_internals.cpp
    \brief    Internals C API implementation for coding-structure analysis.
              Reads CodingStructure / CtuData / CodingUnit / TransformUnit
              and packs the data into flat C buffers for external consumption.
*/

#include "vvdec/vvdec_internals.h"
#include "vvdecimpl.h"

#include "CommonLib/CommonDef.h"
#include "CommonLib/CodingStructure.h"
#include "CommonLib/Picture.h"
#include "CommonLib/Slice.h"
#include "CommonLib/Unit.h"
#include "CommonLib/UnitTools.h"

#include <algorithm>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Helper: resolve a vvdecFrame* back to the internal Picture* via the public
// accessor getPictureFromFrame() on VVDecImpl. Mirrors the lookup in
// findFrameSei(). Returns nullptr if the frame is not in the decoder's frame
// list (e.g. already unref'd or belongs to a different decoder instance).
// ---------------------------------------------------------------------------
const vvdec::Picture* pictureFromFrame( vvdec::VVDecImpl* impl, const vvdecFrame* frame )
{
  if( !impl || !frame )
  {
    return nullptr;
  }
  return impl->getPictureFromFrame( frame );
}

// ---------------------------------------------------------------------------
// Helper: cast vvdecDecoder* to VVDecImpl*. The opaque handle is created in
// vvdec_decoder_open as a VVDecImpl instance.
// ---------------------------------------------------------------------------
vvdec::VVDecImpl* implFromDecoder( vvdecDecoder* dec )
{
  return reinterpret_cast<vvdec::VVDecImpl*>( dec );
}

void writeRefPocList( const vvdec::Slice* slice, const vvdec::RefPicList list, int* numRefPoc, int refPoc[16] )
{
  const int numRef = (int) slice->getNumRefIdx( list );
  if( numRefPoc )
  {
    *numRefPoc = numRef;
  }
  if( refPoc )
  {
    const int n = std::min( numRef, 16 );
    for( int i = 0; i < 16; i++ )
    {
      refPoc[i] = ( i < n ) ? slice->getRefPOC( list, i ) : 0;
    }
  }
}

void writeGopScalars( const vvdec::Picture* pic, const vvdec::Slice* slice, int* poc, int* temporalLayer, int* sliceType, int* isRAP )
{
  if( poc )
  {
    *poc = pic->poc;
  }
  if( temporalLayer )
  {
    *temporalLayer = (int) slice->getTLayer();
  }
  if( sliceType )
  {
    *sliceType = (int) slice->getSliceType();
  }
  if( isRAP )
  {
    *isRAP = slice->isClvssPu() ? 1 : 0;
  }
}

} // anonymous namespace

#ifdef __cplusplus
extern "C" {
#endif

VVDEC_NAMESPACE_BEGIN

/* ===========================================================================
 * CTB (CTU) layout & slice index
 * =========================================================================== */

int vvdec_internals_get_ctb_info_layout_dec( vvdecDecoder *dec,
                                              const vvdecFrame *frame,
                                              int *widthInCtbs,
                                              int *heightInCtbs,
                                              int *log2CtuSize )
{
  auto* impl = implFromDecoder( dec );
  if( !impl )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Picture* pic = pictureFromFrame( impl, frame );
  if( !pic )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::PreCalcValues* pcv = pic->cs->pcv;
  if( widthInCtbs )
  {
    *widthInCtbs = (int) pcv->widthInCtus;
  }
  if( heightInCtbs )
  {
    *heightInCtbs = (int) pcv->heightInCtus;
  }
  if( log2CtuSize )
  {
    *log2CtuSize = (int) pcv->maxCUWidthLog2;
  }
  return VVDEC_OK;
}

int vvdec_internals_get_ctb_slice_idx_dec( vvdecDecoder *dec,
                                            const vvdecFrame *frame,
                                            uint16_t *out )
{
  auto* impl = implFromDecoder( dec );
  if( !impl )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Picture* pic = pictureFromFrame( impl, frame );
  if( !pic )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  if( !out )
  {
    return VVDEC_ERR_PARAMETER;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const int numCtus = (int) cs->pcv->sizeInCtus;
  for( int ctuRsAddr = 0; ctuRsAddr < numCtus; ctuRsAddr++ )
  {
    const vvdec::CtuData& ctuData = cs->getCtuData( ctuRsAddr );
    uint16_t sliceIdx = 0;
    if( ctuData.slice )
    {
      sliceIdx = (uint16_t) ctuData.slice->getIndependentSliceIdx();
    }
    out[ctuRsAddr] = sliceIdx;
  }
  return VVDEC_OK;
}

/* ===========================================================================
 * CB (CU) info — min-CU grid (4x4 luma samples)
 * =========================================================================== */

int vvdec_internals_get_cb_info_layout_dec( vvdecDecoder *dec,
                                             const vvdecFrame *frame,
                                             int *widthInMinCUs,
                                             int *heightInMinCUs,
                                             int *log2MinCuSize )
{
  auto* impl = implFromDecoder( dec );
  if( !impl )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Picture* pic = pictureFromFrame( impl, frame );
  if( !pic )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::PreCalcValues* pcv = pic->cs->pcv;
  if( widthInMinCUs )
  {
    *widthInMinCUs = ( (int) pcv->lumaWidth + (int) pcv->minCUWidth - 1 ) >> pcv->minCUWidthLog2;
  }
  if( heightInMinCUs )
  {
    *heightInMinCUs = ( (int) pcv->lumaHeight + (int) pcv->minCUHeight - 1 ) >> pcv->minCUHeightLog2;
  }
  if( log2MinCuSize )
  {
    *log2MinCuSize = (int) pcv->minCUWidthLog2;
  }
  return VVDEC_OK;
}

// Pack CU fields into a 16-bit value (see header for bit layout).
// VVC CUs can be non-square (e.g. 16x4, 4x16), so both log2(width) and
// log2(height) are stored (offset by 2, since min CU is 4x4 => log2=2).
// Bit 6 is the origin flag: set ONLY at the CU's top-left min-CU cell.
static uint16_t packCuInfo( const vvdec::CodingUnit* cu, bool isOrigin )
{
  if( !cu || !isOrigin )
  {
    return 0;
  }
  using namespace vvdec;
  uint16_t v = 0;
  // bits 0-2: log2(cuWidth) - 2  (0=4, 1=8, 2=16, 3=32, 4=64)
  const int log2W = (int) getLog2( (int) cu->lwidth() ) - 2;
  v |= (uint16_t)( log2W & 0x7 );
  // bits 3-5: log2(cuHeight) - 2  (0=4, 1=8, 2=16, 3=32, 4=64)
  const int log2H = (int) getLog2( (int) cu->lheight() ) - 2;
  v |= (uint16_t)( ( log2H & 0x7 ) << 3 );
  // bit 6: origin flag (1 = this cell is the CU's top-left)
  v |= ( 1 << 6 );
  // bits 7-8: predMode (0=INTER, 1=INTRA, 2=IBC)
  v |= (uint16_t)( ( cu->predMode() & 0x3 ) << 7 );
  // bits 9-11: qtDepth
  v |= (uint16_t)( ( cu->qtDepth & 0x7 ) << 9 );
  // bits 12-14: MT depth (without QT depth)
  v |= (uint16_t)( ( ( cu->depth - cu->qtDepth ) & 0x7 ) << 12 );
  // bit 15: skip flag (most useful single flag; merge/affine omitted for space)
  if( cu->skip() )
  {
    v |= ( 1 << 15 );
  }
  return v;
}

int vvdec_internals_get_cb_info_dec( vvdecDecoder *dec,
                                     const vvdecFrame *frame,
                                     uint16_t *out )
{
  auto* impl = implFromDecoder( dec );
  if( !impl )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Picture* pic = pictureFromFrame( impl, frame );
  if( !pic )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  if( !out )
  {
    return VVDEC_ERR_PARAMETER;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  const int minCuLog2 = (int) pcv->minCUWidthLog2;
  const int widthInMinCUs  = ( (int) pcv->lumaWidth  + (int) pcv->minCUWidth  - 1 ) >> minCuLog2;
  const int heightInMinCUs = ( (int) pcv->lumaHeight + (int) pcv->minCUHeight - 1 ) >> minCuLog2;
  for( int y = 0; y < heightInMinCUs; y++ )
  {
    for( int x = 0; x < widthInMinCUs; x++ )
    {
      const int px = x << minCuLog2;
      const int py = y << minCuLog2;
      const vvdec::Position pos( px, py );
      const vvdec::CodingUnit* cu = nullptr;
      bool isOrigin = false;
      if( (unsigned) px < (unsigned) pcv->lumaWidth && (unsigned) py < (unsigned) pcv->lumaHeight )
      {
        cu = cs->getCU( pos, vvdec::CHANNEL_TYPE_LUMA );
        // A cell is the CU origin iff it coincides with the CU's top-left luma block.
        if( cu )
        {
          isOrigin = ( cu->Y().x == px && cu->Y().y == py );
        }
      }
      out[ y * widthInMinCUs + x ] = packCuInfo( cu, isOrigin );
    }
  }
  return VVDEC_OK;
}

/* ===========================================================================
 * PB (PU) info — min-CU grid (4x4 luma samples)
 * =========================================================================== */

int vvdec_internals_get_pb_info_layout_dec( vvdecDecoder *dec,
                                            const vvdecFrame *frame,
                                            int *widthInMinCUs,
                                            int *heightInMinCUs,
                                            int *log2MinCuSize )
{
  return vvdec_internals_get_cb_info_layout_dec( dec, frame, widthInMinCUs, heightInMinCUs, log2MinCuSize );
}

int vvdec_internals_get_pb_info_dec( vvdecDecoder *dec,
                                     const vvdecFrame *frame,
                                     int16_t *out_mv0_x,
                                     int16_t *out_mv0_y,
                                     int16_t *out_mv1_x,
                                     int16_t *out_mv1_y,
                                     int16_t *out_ref_idx0,
                                     int16_t *out_ref_idx1 )
{
  auto* impl = implFromDecoder( dec );
  if( !impl )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Picture* pic = pictureFromFrame( impl, frame );
  if( !pic )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  if( !out_mv0_x || !out_mv0_y || !out_mv1_x || !out_mv1_y || !out_ref_idx0 || !out_ref_idx1 )
  {
    return VVDEC_ERR_PARAMETER;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  const int minCuLog2 = (int) pcv->minCUWidthLog2;
  const int widthInMinCUs  = ( (int) pcv->lumaWidth  + (int) pcv->minCUWidth  - 1 ) >> minCuLog2;
  const int heightInMinCUs = ( (int) pcv->lumaHeight + (int) pcv->minCUHeight - 1 ) >> minCuLog2;
  for( int y = 0; y < heightInMinCUs; y++ )
  {
    for( int x = 0; x < widthInMinCUs; x++ )
    {
      const int px = x << minCuLog2;
      const int py = y << minCuLog2;
      const vvdec::Position pos( px, py );
      const vvdec::CodingUnit* cu = nullptr;
      if( cs->area.blocks[vvdec::CHANNEL_TYPE_LUMA].contains( pos ) )
      {
        cu = cs->getCU( pos, vvdec::CHANNEL_TYPE_LUMA );
      }
      const int idx = y * widthInMinCUs + x;
      if( cu )
      {
        // Per-4x4 field, unlike cu->mv[][] it keeps affine/ATMVP/GEO/DMVR; intra and IBC are MI_NOT_VALID.
        const vvdec::MotionInfo& mi = cs->getMotionInfo( pos );
        out_mv0_x[idx] = (int16_t) mi.mv[vvdec::REF_PIC_LIST_0].getHor();
        out_mv0_y[idx] = (int16_t) mi.mv[vvdec::REF_PIC_LIST_0].getVer();
        out_mv1_x[idx] = (int16_t) mi.mv[vvdec::REF_PIC_LIST_1].getHor();
        out_mv1_y[idx] = (int16_t) mi.mv[vvdec::REF_PIC_LIST_1].getVer();
        out_ref_idx0[idx] = (int16_t) mi.miRefIdx[vvdec::REF_PIC_LIST_0];
        out_ref_idx1[idx] = (int16_t) mi.miRefIdx[vvdec::REF_PIC_LIST_1];
      }
      else
      {
        out_mv0_x[idx] = 0;
        out_mv0_y[idx] = 0;
        out_mv1_x[idx] = 0;
        out_mv1_y[idx] = 0;
        out_ref_idx0[idx] = -1;
        out_ref_idx1[idx] = -1;
      }
    }
  }
  return VVDEC_OK;
}

/* ===========================================================================
 * Intra direction info — min-CU grid (4x4 luma samples)
 * =========================================================================== */

int vvdec_internals_get_intra_dir_info_layout_dec( vvdecDecoder *dec,
                                                    const vvdecFrame *frame,
                                                    int *widthInMinCUs,
                                                    int *heightInMinCUs,
                                                    int *log2MinCuSize )
{
  return vvdec_internals_get_cb_info_layout_dec( dec, frame, widthInMinCUs, heightInMinCUs, log2MinCuSize );
}

int vvdec_internals_get_intra_dir_info_dec( vvdecDecoder *dec,
                                            const vvdecFrame *frame,
                                            uint8_t *out_intra_dir_y,
                                            uint8_t *out_intra_dir_c )
{
  auto* impl = implFromDecoder( dec );
  if( !impl )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Picture* pic = pictureFromFrame( impl, frame );
  if( !pic )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  if( !out_intra_dir_y || !out_intra_dir_c )
  {
    return VVDEC_ERR_PARAMETER;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  const int minCuLog2 = (int) pcv->minCUWidthLog2;
  const int widthInMinCUs  = ( (int) pcv->lumaWidth  + (int) pcv->minCUWidth  - 1 ) >> minCuLog2;
  const int heightInMinCUs = ( (int) pcv->lumaHeight + (int) pcv->minCUHeight - 1 ) >> minCuLog2;
  for( int y = 0; y < heightInMinCUs; y++ )
  {
    for( int x = 0; x < widthInMinCUs; x++ )
    {
      const int px = x << minCuLog2;
      const int py = y << minCuLog2;
      const vvdec::Position pos( px, py );
      const vvdec::CodingUnit* cu = nullptr;
      if( cs->area.blocks[vvdec::CHANNEL_TYPE_LUMA].contains( pos ) )
      {
        cu = cs->getCU( pos, vvdec::CHANNEL_TYPE_LUMA );
      }
      const int idx = y * widthInMinCUs + x;
      if( cu )
      {
        // getCU() expects chroma-resolution positions; in a dual tree the chroma CU owns the mode.
        const vvdec::Position posC = vvdec::recalcPosition( cs->area.chromaFormat, vvdec::CHANNEL_TYPE_LUMA,
                                                            vvdec::CHANNEL_TYPE_CHROMA, pos );
        const vvdec::CodingUnit* cuC = cs->getCU( posC, vvdec::CHANNEL_TYPE_CHROMA );
        if( !cuC )
        {
          cuC = cu;
        }
        out_intra_dir_y[idx] = (uint8_t) cu->intraDir[vvdec::CHANNEL_TYPE_LUMA];
        out_intra_dir_c[idx] = (uint8_t) cuC->intraDir[vvdec::CHANNEL_TYPE_CHROMA];
      }
      else
      {
        out_intra_dir_y[idx] = 0;
        out_intra_dir_c[idx] = 0;
      }
    }
  }
  return VVDEC_OK;
}

/* ===========================================================================
 * TU (transform unit) info — min-CU grid (4x4 luma samples)
 * =========================================================================== */

int vvdec_internals_get_tu_info_layout_dec( vvdecDecoder *dec,
                                             const vvdecFrame *frame,
                                             int *widthInMinCUs,
                                             int *heightInMinCUs,
                                             int *log2MinCuSize )
{
  return vvdec_internals_get_cb_info_layout_dec( dec, frame, widthInMinCUs, heightInMinCUs, log2MinCuSize );
}

int vvdec_internals_get_tu_info_dec( vvdecDecoder *dec,
                                     const vvdecFrame *frame,
                                     uint8_t *out )
{
  auto* impl = implFromDecoder( dec );
  if( !impl )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Picture* pic = pictureFromFrame( impl, frame );
  if( !pic )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  if( !out )
  {
    return VVDEC_ERR_PARAMETER;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  const int minCuLog2 = (int) pcv->minCUWidthLog2;
  const int widthInMinCUs  = ( (int) pcv->lumaWidth  + (int) pcv->minCUWidth  - 1 ) >> minCuLog2;
  const int heightInMinCUs = ( (int) pcv->lumaHeight + (int) pcv->minCUHeight - 1 ) >> minCuLog2;

  for( int y = 0; y < heightInMinCUs; y++ )
  {
    for( int x = 0; x < widthInMinCUs; x++ )
    {
      const int px = x << minCuLog2;
      const int py = y << minCuLog2;
      const vvdec::Position pos( px, py );
      const vvdec::CodingUnit* cu = nullptr;
      if( cs->area.blocks[vvdec::CHANNEL_TYPE_LUMA].contains( pos ) )
      {
        cu = cs->getCU( pos, vvdec::CHANNEL_TYPE_LUMA );
      }
      const int idx = y * widthInMinCUs + x;
      uint8_t tuDepth = 0;
      if( cu )
      {
        // TransformUnit stores no tree depth: derive it from both dimensions. TT side TUs read one level too deep.
        const int cuWLog2 = (int) vvdec::getLog2( (int) cu->lwidth() );
        const int cuHLog2 = (int) vvdec::getLog2( (int) cu->lheight() );
        for( const vvdec::TransformUnit* tu = &cu->firstTU; tu != nullptr; tu = tu->next )
        {
          if( tu->Y().contains( pos ) )
          {
            const int wDepth = cuWLog2 - (int) vvdec::getLog2( (int) tu->lwidth() );
            const int hDepth = cuHLog2 - (int) vvdec::getLog2( (int) tu->lheight() );
            const int depth  = std::max( wDepth, hDepth );
            tuDepth = (uint8_t)( depth > 0 ? depth : 0 );
            break;
          }
        }
      }
      out[idx] = tuDepth;
    }
  }

  return VVDEC_OK;
}

/* ===========================================================================
 * GOP info — per-frame picture order, references, temporal layer
 * =========================================================================== */

int vvdec_internals_get_gop_info_dec( vvdecDecoder *dec,
                                      const vvdecFrame *frame,
                                      int *poc,
                                      int *temporalLayer,
                                      int *sliceType,
                                      int *isRAP,
                                      int *numRefPocL0,
                                      int *numRefPocL1,
                                      int refPocL0[16],
                                      int refPocL1[16] )
{
  auto* impl = implFromDecoder( dec );
  if( !impl )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Picture* pic = pictureFromFrame( impl, frame );
  if( !pic || pic->slices.empty() )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Slice* slice = pic->slices.front();
  writeGopScalars( pic, slice, poc, temporalLayer, sliceType, isRAP );
  writeRefPocList( slice, vvdec::REF_PIC_LIST_0, numRefPocL0, refPocL0 );
  writeRefPocList( slice, vvdec::REF_PIC_LIST_1, numRefPocL1, refPocL1 );
  return VVDEC_OK;
}

/* ===========================================================================
 * GOP info (header-only) — extract GOP metadata for the most recently
 * completed header-only-parsed picture, without any decoded frame.
 * =========================================================================== */
int vvdec_internals_get_gop_info_headeronly( vvdecDecoder *dec,
                                             int *poc,
                                             int *temporalLayer,
                                             int *sliceType,
                                             int *nalUnitType,
                                             int *isRAP,
                                             int *allSlicesIntra,
                                             int *numRefPocL0,
                                             int *numRefPocL1,
                                             int refPocL0[16],
                                             int refPocL1[16],
                                             int64_t *picSequence )
{
  auto* impl = implFromDecoder( dec );
  if( !impl )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Picture* pic = impl->getLastParsedPic();
  if( !pic || pic->slices.empty() )
  {
    vvdec::msg( vvdec::WARNING, "vvdec_internals_get_gop_info_headeronly: no parsed picture available yet\n" );
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::Slice* slice = pic->slices.front();
  writeGopScalars( pic, slice, poc, temporalLayer, sliceType, isRAP );
  if( nalUnitType )
  {
    *nalUnitType = (int) slice->getNalUnitType();
  }
  if( allSlicesIntra )
  {
    bool allIntra = true;
    for( const vvdec::Slice* s : pic->slices )
    {
      if( !s || s->getSliceType() != vvdec::I_SLICE )
      {
        allIntra = false;
        break;
      }
    }
    *allSlicesIntra = allIntra ? 1 : 0;
  }
  writeRefPocList( slice, vvdec::REF_PIC_LIST_0, numRefPocL0, refPocL0 );
  writeRefPocList( slice, vvdec::REF_PIC_LIST_1, numRefPocL1, refPocL1 );
  if( picSequence )
  {
    *picSequence = (int64_t) impl->getLastParsedPicSequence();
  }
  return VVDEC_OK;
}

/* ===========================================================================
 * Image timestamp (named "dts" for API compatibility, returns CTS)
 * =========================================================================== */

int64_t vvdec_internals_get_image_dts_dec( const vvdecFrame *frame )
{
  if( !frame )
  {
    return -1;
  }
  // vvdecFrame carries no DTS, only cts.
  return frame->ctsValid ? (int64_t) frame->cts : -1;
}

VVDEC_NAMESPACE_END

#ifdef __cplusplus
}
#endif /*__cplusplus */
