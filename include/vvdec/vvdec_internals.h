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
/** \file     vvdec_internals.h
    \brief    Internals C API for coding-structure analysis (CTU/CU/PU/TU/IntraDir/TransformDepth).
              This API exposes VVC coding-structure statistics for external analysis tools
              (e.g. YUView) without requiring trace text parsing. It is optional: consumers
              detect support by resolving the vvdec_internals_get_ctb_info_layout_dec symbol.
*/

#pragma once

#include "vvdec/vvdecDecl.h"
#include "vvdec/vvdec.h"

#ifdef __cplusplus
extern "C" {
#endif

VVDEC_NAMESPACE_BEGIN

/* --------------------------------------------------------------------------
 * CTB (CTU) layout & slice index
 * --------------------------------------------------------------------------
 * Two-call pattern (mirrors libde265-internals):
 *   1) vvdec_internals_get_ctb_info_layout_dec(dec, frame, &w, &h, &log2CtuSize)
 *      -> query dimensions; allocate buffers of size w*h (uint16_t).
 *   2) vvdec_internals_get_ctb_slice_idx_dec(dec, frame, out)
 *      -> fill out[0..w*h-1] with the per-CTU slice index.
 */

/* Get CTB grid layout for a decoded frame.
 * \param[in]  dec           decoder handle (from vvdec_decoder_open)
 * \param[in]  frame         decoded frame (from vvdec_decode / vvdec_flush)
 * \param[out] widthInCtbs   number of CTUs per row (may be NULL)
 * \param[out] heightInCtbs   number of CTUs per column (may be NULL)
 * \param[out] log2CtuSize    base-2 log of CTU size (e.g. 6 for 64x64, may be NULL)
 * \retval VVDEC_OK on success, error code otherwise
 */
VVDEC_DECL int vvdec_internals_get_ctb_info_layout_dec( vvdecDecoder *dec,
                                                        const vvdecFrame *frame,
                                                        int *widthInCtbs,
                                                        int *heightInCtbs,
                                                        int *log2CtuSize );

/* Fill per-CTU slice index buffer.
 * \param[in]  dec   decoder handle
 * \param[in]  frame decoded frame
 * \param[out] out   mutable output buffer, must not be NULL,
 *                   length widthInCtus*heightInCtus uint16_t values (CTU slice index)
 */
VVDEC_DECL int vvdec_internals_get_ctb_slice_idx_dec( vvdecDecoder *dec,
                                                     const vvdecFrame *frame,
                                                     uint16_t *out );

/* --------------------------------------------------------------------------
 * CB (CU) info — min-CU grid (4x4 luma samples)
 * --------------------------------------------------------------------------
 * The CB grid has widthInMinCUs = lumaWidth / 4, heightInMinCUs = lumaHeight / 4.
 * Each min-CU cell maps to the CodingUnit covering that position.
 */

/* Get CB grid layout.
 * \param[out] widthInMinCUs   lumaWidth / 4 (may be NULL)
 * \param[out] heightInMinCUs  lumaHeight / 4 (may be NULL)
 * \param[out] log2MinCuSize    base-2 log of min CU size (always 2 => 4x4, may be NULL)
 */
VVDEC_DECL int vvdec_internals_get_cb_info_layout_dec( vvdecDecoder *dec,
                                                       const vvdecFrame *frame,
                                                       int *widthInMinCUs,
                                                       int *heightInMinCUs,
                                                       int *log2MinCuSize );

/* Fill CB info buffer. Each min-CU cell stores a packed uint16_t bitfield:
 *   bits 0-2:   log2(cuWidth) - 2   (0=4, 1=8, 2=16, 3=32, 4=64)
 *   bits 3-5:   log2(cuHeight) - 2  (0=4, 1=8, 2=16, 3=32, 4=64)
 *   bit  6:     origin flag (1 = this cell is the CU's top-left; 0 otherwise)
 *   bits 7-8:   predMode      (0=INTER, 1=INTRA, 2=IBC)
 *   bits 9-11:  qtDepth       (0..7)
 *   bits 12-14: MT depth      (0..7, BT/TT splits below the QT leaf only,
 *                              i.e. cu->depth - cu->qtDepth)
 *   bit  15:    skip flag
 * Non-origin cells (covered by a CU but not its top-left) store 0.
 * VVC CUs can be non-square; both width and height are encoded.
 * \param[in]  dec   decoder handle
 * \param[in]  frame decoded frame
 * \param[out] out   mutable output buffer, must not be NULL,
 *                   length widthInMinCUs*heightInMinCUs uint16_t values
 */
VVDEC_DECL int vvdec_internals_get_cb_info_dec( vvdecDecoder *dec,
                                                const vvdecFrame *frame,
                                                uint16_t *out );

/* --------------------------------------------------------------------------
 * PB (PU) info — min-CU grid (4x4 luma samples)
 * --------------------------------------------------------------------------
 * Motion of the covering 4x4 block (effective PB motion: affine, ATMVP, GEO,
 * DMVR). Intra and IBC blocks report refIdx = -1 and mv = 0.
 *
 * Known limitation: the source motion field is per-frame decoder scratch, so
 * only the newest decoded frame is guaranteed to be valid.
 *
 * Six int16_t arrays, each widthInMinCus*heightInMinCus:
 *   out_mv0_x, out_mv0_y : L0 motion vector (1/16 pel)
 *   out_mv1_x, out_mv1_y : L1 motion vector (1/16 pel)
 *   out_ref_idx0          : L0 reference index (-1 if unused)
 *   out_ref_idx1          : L1 reference index (-1 if unused)
 */

/* Get PB grid layout (same as CB grid). */
VVDEC_DECL int vvdec_internals_get_pb_info_layout_dec( vvdecDecoder *dec,
                                                       const vvdecFrame *frame,
                                                       int *widthInMinCUs,
                                                       int *heightInMinCUs,
                                                       int *log2MinCuSize );

/* Fill PB info buffers.
 * All six output arrays are mutable output buffers, they must not be NULL and
 * each has to hold widthInMinCUs*heightInMinCUs int16_t values.
 */
VVDEC_DECL int vvdec_internals_get_pb_info_dec( vvdecDecoder *dec,
                                                const vvdecFrame *frame,
                                                int16_t *out_mv0_x,
                                                int16_t *out_mv0_y,
                                                int16_t *out_mv1_x,
                                                int16_t *out_mv1_y,
                                                int16_t *out_ref_idx0,
                                                int16_t *out_ref_idx1 );

/* --------------------------------------------------------------------------
 * Intra direction info — min-CU grid (4x4 luma samples)
 * --------------------------------------------------------------------------
 * Two uint8_t arrays of widthInMinCus*heightInMinCus:
 *   out_intra_dir_y : luma mode, 0=planar, 1=DC, 2..66=angular. MIP CUs store
 *                     the raw MIP index 0..31, which collides with that range.
 *   out_intra_dir_c : chroma mode, 0..66 as luma, plus 67=LM_CHROMA,
 *                     68=MDLM_L, 69=MDLM_T, 70=DM_CHROMA. In a dual tree the
 *                     chroma CU's own mode is reported.
 */

/* Get intra dir grid layout (same as CB grid). */
VVDEC_DECL int vvdec_internals_get_intra_dir_info_layout_dec( vvdecDecoder *dec,
                                                              const vvdecFrame *frame,
                                                              int *widthInMinCUs,
                                                              int *heightInMinCUs,
                                                              int *log2MinCuSize );

/* Fill intra direction buffers.
 * \param[out] out_intra_dir_y mutable output buffer, must not be NULL,
 *                             length widthInMinCUs*heightInMinCUs uint8_t values
 * \param[out] out_intra_dir_c mutable output buffer, must not be NULL,
 *                             length widthInMinCUs*heightInMinCUs uint8_t values
 */
VVDEC_DECL int vvdec_internals_get_intra_dir_info_dec( vvdecDecoder *dec,
                                                       const vvdecFrame *frame,
                                                       uint8_t *out_intra_dir_y,
                                                       uint8_t *out_intra_dir_c );

/* --------------------------------------------------------------------------
 * TU (transform unit) info — min-CU grid (4x4 luma samples)
 * --------------------------------------------------------------------------
 * One uint8_t array of widthInMinCus*heightInMinCus:
 *   out_tu_info : transform depth (0..7) in bits 0-2.
 *                 Bits 3+ reserved (future: mtsIdx, lfnst, jointCbCr).
 *                 Derived from the TU/CU size ratio in both dimensions;
 *                 TT side TUs are reported one level too deep.
 */

/* Get TU grid layout (same as CB grid). */
VVDEC_DECL int vvdec_internals_get_tu_info_layout_dec( vvdecDecoder *dec,
                                                       const vvdecFrame *frame,
                                                       int *widthInMinCUs,
                                                       int *heightInMinCUs,
                                                       int *log2MinCuSize );

/* Fill TU info buffer.
 * \param[out] out mutable output buffer, must not be NULL,
 *                 length widthInMinCUs*heightInMinCUs uint8_t values
 */
VVDEC_DECL int vvdec_internals_get_tu_info_dec( vvdecDecoder *dec,
                                                const vvdecFrame *frame,
                                                uint8_t *out );

/* --------------------------------------------------------------------------
 * GOP info — per-frame picture order, references, temporal layer
 * --------------------------------------------------------------------------
 * \param[out] poc          picture order count of this frame (may be NULL)
 * \param[out] temporalLayer temporal layer id (may be NULL)
 * \param[out] sliceType    0=B, 1=P, 2=I (may be NULL)
 * \param[out] isRAP        1 if the picture is a CLVSS picture (IDR/CRA/GDR),
 *                          0 otherwise (may be NULL)
 * \param[out] numRefPocL0  number of L0 reference pictures (filled into refPocL0, may be NULL)
 * \param[out] numRefPocL1  number of L1 reference pictures (filled into refPocL1, may be NULL)
 * \param[out] refPocL0     array of 16 L0 reference POCs (may be NULL). The
 *                         implementation always writes all 16 entries and
 *                         pads the unused ones with 0.
 * \param[out] refPocL1     array of 16 L1 reference POCs (may be NULL). The
 *                         implementation always writes all 16 entries and
 *                         pads the unused ones with 0.
 */
VVDEC_DECL int vvdec_internals_get_gop_info_dec( vvdecDecoder *dec,
                                                 const vvdecFrame *frame,
                                                 int *poc,
                                                 int *temporalLayer,
                                                 int *sliceType,
                                                 int *isRAP,
                                                 int *numRefPocL0,
                                                 int *numRefPocL1,
                                                 int refPocL0[16],
                                                 int refPocL1[16] );

/* Get GOP info for the most recently completed header-only-parsed picture.
 * Requires the decoder to be in header-only mode (vvdec_decode_headeronly).
 * No vvdecFrame is needed — the metadata is read directly from the parser's
 * internal picture state.
 * \param[out] poc            picture order count of this frame (may be NULL)
 * \param[out] temporalLayer  temporal layer id (may be NULL)
 * \param[out] sliceType      0=B, 1=P, 2=I (of the first slice, may be NULL)
 * \param[out] nalUnitType    NAL unit type of the first slice (may be NULL)
 * \param[out] isRAP          1 if the picture is a CLVSS picture (IDR/CRA/GDR),
 *                            0 otherwise (may be NULL)
 * \param[out] allSlicesIntra 1 iff every slice in the picture is an I-slice (may be NULL)
 * \param[out] numRefPocL0    number of L0 reference pictures (filled into refPocL0, may be NULL)
 * \param[out] numRefPocL1    number of L1 reference pictures (filled into refPocL1, may be NULL)
 * \param[out] refPocL0       array of 16 L0 reference POCs (may be NULL). The
 *                            implementation always writes all 16 entries and
 *                            pads the unused ones with 0.
 * \param[out] refPocL1       array of 16 L1 reference POCs (may be NULL). The
 *                            implementation always writes all 16 entries and
 *                            pads the unused ones with 0.
 * \param[out] picSequence    monotonically increasing sequence number of the
 *                            parsed picture (may be NULL). Callers should use
 *                            this to detect that a new picture completed; do
 *                            not use the POC for that, an all-IDR stream has
 *                            POC 0 for every picture.
 * \retval VVDEC_OK on success, VVDEC_ERR_INITIALIZE if no picture has been parsed yet
 */
VVDEC_DECL int vvdec_internals_get_gop_info_headeronly( vvdecDecoder *dec,
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
                                                        int64_t *picSequence );

/* Get a timestamp of a frame (for GOP plot alignment).
 * Despite its name and its "dts" symbol this returns the coding timestamp
 * (CTS), because vvdecFrame does not carry a DTS.
 * \retval int64_t CTS, or -1 if unavailable
 */
VVDEC_DECL int64_t vvdec_internals_get_image_dts_dec( const vvdecFrame *frame );

VVDEC_NAMESPACE_END

#ifdef __cplusplus
}
#endif /*__cplusplus */
