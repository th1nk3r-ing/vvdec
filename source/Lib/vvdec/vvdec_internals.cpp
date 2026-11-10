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
  if( !pic->cs )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  if( !pcv )
  {
    return VVDEC_ERR_INITIALIZE;
  }
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
  if( !pic || !pic->cs )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  if( !pcv )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const int numCtus = (int) pcv->sizeInCtus;
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
  if( !pic || !pic->cs )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::PreCalcValues* pcv = pic->cs->pcv;
  if( !pcv )
  {
    return VVDEC_ERR_INITIALIZE;
  }
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
  // bits 12-14: mtDepth (cu->depth)
  v |= (uint16_t)( ( cu->depth & 0x7 ) << 12 );
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
  if( !pic || !pic->cs )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  if( !pcv )
  {
    return VVDEC_ERR_INITIALIZE;
  }
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
  if( !pic || !pic->cs )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  if( !pcv )
  {
    return VVDEC_ERR_INITIALIZE;
  }
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
        // mv[list][0] is the first (non-affine) control point MV.
        out_mv0_x[idx] = (int16_t) cu->mv[vvdec::REF_PIC_LIST_0][0].getHor();
        out_mv0_y[idx] = (int16_t) cu->mv[vvdec::REF_PIC_LIST_0][0].getVer();
        out_mv1_x[idx] = (int16_t) cu->mv[vvdec::REF_PIC_LIST_1][0].getHor();
        out_mv1_y[idx] = (int16_t) cu->mv[vvdec::REF_PIC_LIST_1][0].getVer();
        out_ref_idx0[idx] = (int16_t) cu->refIdx[vvdec::REF_PIC_LIST_0];
        out_ref_idx1[idx] = (int16_t) cu->refIdx[vvdec::REF_PIC_LIST_1];
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
  if( !pic || !pic->cs )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  if( !pcv )
  {
    return VVDEC_ERR_INITIALIZE;
  }
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
        out_intra_dir_y[idx] = (uint8_t) cu->intraDir[vvdec::CHANNEL_TYPE_LUMA];
        out_intra_dir_c[idx] = (uint8_t) cu->intraDir[vvdec::CHANNEL_TYPE_CHROMA];
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
  if( !pic || !pic->cs )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const vvdec::CodingStructure* cs = pic->cs.get();
  const vvdec::PreCalcValues*   pcv = cs->pcv;
  if( !pcv )
  {
    return VVDEC_ERR_INITIALIZE;
  }
  const int minCuLog2 = (int) pcv->minCUWidthLog2;
  const int widthInMinCUs  = ( (int) pcv->lumaWidth  + (int) pcv->minCUWidth  - 1 ) >> minCuLog2;
  const int heightInMinCUs = ( (int) pcv->lumaHeight + (int) pcv->minCUHeight - 1 ) >> minCuLog2;

  static int s_diagCount = 0;
  int cuCount = 0, tuCount = 0, splitCuCount = 0;
  int depthHist[8] = {};

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
        // Only count each CU once (at its origin).
        if( (int) cu->lwidth() > 0 && ( px % (int) cu->lwidth() == 0 ) && ( py % (int) cu->lheight() == 0 ) )
        {
          cuCount++;
          int nTU = 0;
          for( const vvdec::TransformUnit* tu = &cu->firstTU; tu != nullptr; tu = tu->next )
            nTU++;
          if( nTU > 1 )
            splitCuCount++;
        }

        const int cuSizeLog2 = (int) vvdec::getLog2( (int) cu->lwidth() );
        for( const vvdec::TransformUnit* tu = &cu->firstTU; tu != nullptr; tu = tu->next )
        {
          tuCount++;
          if( tu->Y().contains( pos ) )
          {
            const int tuSizeLog2 = (int) vvdec::getLog2( (int) tu->lwidth() );
            const int depth = cuSizeLog2 - tuSizeLog2;
            tuDepth = (uint8_t)( depth > 0 ? depth : 0 );
            break;
          }
        }
      }
      out[idx] = tuDepth;
      depthHist[tuDepth < 8 ? tuDepth : 7]++;
    }
  }

  if( s_diagCount < 3 )
  {
    fprintf( stderr, "[vvdec TU diag %d] CUs=%d splitCUs=%d totalTUs=%d depthHist: 0=%d 1=%d 2=%d 3=%d\n",
             s_diagCount, cuCount, splitCuCount, tuCount,
             depthHist[0], depthHist[1], depthHist[2], depthHist[3] );
    s_diagCount++;
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
  if( !slice )
  {
    return VVDEC_ERR_INITIALIZE;
  }
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
    // VVC: B_SLICE=0, P_SLICE=1, I_SLICE=2
    *sliceType = (int) slice->getSliceType();
  }
  if( isRAP )
  {
    *isRAP = slice->isIRAP() ? 1 : 0;
  }
  if( numRefPocL0 )
  {
    *numRefPocL0 = (int) slice->getNumRefIdx( vvdec::REF_PIC_LIST_0 );
  }
  if( numRefPocL1 )
  {
    *numRefPocL1 = (int) slice->getNumRefIdx( vvdec::REF_PIC_LIST_1 );
  }
  if( refPocL0 )
  {
    const int n = std::min( (int) slice->getNumRefIdx( vvdec::REF_PIC_LIST_0 ), 16 );
    for( int i = 0; i < 16; i++ )
    {
      refPocL0[i] = ( i < n ) ? slice->getRefPOC( vvdec::REF_PIC_LIST_0, i ) : 0;
    }
  }
  if( refPocL1 )
  {
    const int n = std::min( (int) slice->getNumRefIdx( vvdec::REF_PIC_LIST_1 ), 16 );
    for( int i = 0; i < 16; i++ )
    {
      refPocL1[i] = ( i < n ) ? slice->getRefPOC( vvdec::REF_PIC_LIST_1, i ) : 0;
    }
  }
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
                                             int refPocL1[16] )
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
  if( !slice )
  {
    vvdec::msg( vvdec::WARNING, "vvdec_internals_get_gop_info_headeronly: first slice is null\n" );
    return VVDEC_ERR_INITIALIZE;
  }
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
    // VVC: B_SLICE=0, P_SLICE=1, I_SLICE=2
    *sliceType = (int) slice->getSliceType();
  }
  if( nalUnitType )
  {
    *nalUnitType = (int) slice->getNalUnitType();
  }
  if( isRAP )
  {
    *isRAP = slice->isIRAP() ? 1 : 0;
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
  if( numRefPocL0 )
  {
    *numRefPocL0 = (int) slice->getNumRefIdx( vvdec::REF_PIC_LIST_0 );
  }
  if( numRefPocL1 )
  {
    *numRefPocL1 = (int) slice->getNumRefIdx( vvdec::REF_PIC_LIST_1 );
  }
  if( refPocL0 )
  {
    const int n = std::min( (int) slice->getNumRefIdx( vvdec::REF_PIC_LIST_0 ), 16 );
    for( int i = 0; i < 16; i++ )
    {
      refPocL0[i] = ( i < n ) ? slice->getRefPOC( vvdec::REF_PIC_LIST_0, i ) : 0;
    }
  }
  if( refPocL1 )
  {
    const int n = std::min( (int) slice->getNumRefIdx( vvdec::REF_PIC_LIST_1 ), 16 );
    for( int i = 0; i < 16; i++ )
    {
      refPocL1[i] = ( i < n ) ? slice->getRefPOC( vvdec::REF_PIC_LIST_1, i ) : 0;
    }
  }
  return VVDEC_OK;
}

/* ===========================================================================
 * Image DTS
 * =========================================================================== */

int64_t vvdec_internals_get_image_dts_dec( const vvdecFrame *frame )
{
  if( !frame )
  {
    return -1;
  }
  // The vvdecFrame stores the composition timestamp (cts) and a ctsValid flag.
  // We return cts as the DTS surrogate; consumers that set CTS will get a
  // monotonic frame ordering metric.
  return frame->ctsValid ? (int64_t) frame->cts : -1;
}

VVDEC_NAMESPACE_END

#ifdef __cplusplus
}
#endif /*__cplusplus */
