/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#define DEBUG_INSERT_STREAM_ERRORS 0


#include "de265.h"
#include "decctx.h"
#include "util.h"
#include "scan.h"
#include "image.h"
#include "sei.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>


// TODO: should be in some vps.c related header
de265_error read_vps(decoder_context* ctx, bitreader* reader, video_parameter_set* vps);

extern "C" {
LIBDE265_API const char *de265_get_version(void)
{
    return (LIBDE265_VERSION);
}

LIBDE265_API uint32_t de265_get_version_number(void)
{
    return (LIBDE265_NUMERIC_VERSION);
}

LIBDE265_API const char* de265_get_error_text(de265_error err)
{
  switch (err) {
  case DE265_OK: return "no error";
  case DE265_ERROR_NO_SUCH_FILE: return "no such file";
    //case DE265_ERROR_NO_STARTCODE: return "no startcode found";
  case DE265_ERROR_EOF: return "end of file";
  case DE265_ERROR_COEFFICIENT_OUT_OF_IMAGE_BOUNDS: return "coefficient out of image bounds";
  case DE265_ERROR_CHECKSUM_MISMATCH: return "image checksum mismatch";
  case DE265_ERROR_CTB_OUTSIDE_IMAGE_AREA: return "CTB outside of image area";
  case DE265_ERROR_OUT_OF_MEMORY: return "out of memory";
  case DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE: return "coded parameter out of range";
  case DE265_ERROR_IMAGE_BUFFER_FULL: return "DPB/output queue full";
  case DE265_ERROR_CANNOT_START_THREADPOOL: return "cannot start decoding threads";
  case DE265_ERROR_LIBRARY_INITIALIZATION_FAILED: return "global library initialization failed";
  case DE265_ERROR_LIBRARY_NOT_INITIALIZED: return "cannot free library data (not initialized";

  case DE265_ERROR_MAX_THREAD_CONTEXTS_EXCEEDED:
    return "internal error: maximum number of thread contexts exceeded";
  case DE265_ERROR_MAX_NUMBER_OF_SLICES_EXCEEDED:
    return "internal error: maximum number of slices exceeded";
    //case DE265_ERROR_SCALING_LIST_NOT_IMPLEMENTED:
    //return "scaling list not implemented";
  case DE265_ERROR_WAITING_FOR_INPUT_DATA:
    return "no more input data, decoder stalled";
  case DE265_ERROR_CANNOT_PROCESS_SEI:
    return "SEI data cannot be processed";

  case DE265_WARNING_NO_WPP_CANNOT_USE_MULTITHREADING:
    return "Cannot run decoder multi-threaded because stream does not support WPP";
  case DE265_WARNING_WARNING_BUFFER_FULL:
    return "Too many warnings queued";
  case DE265_WARNING_PREMATURE_END_OF_SLICE_SEGMENT:
    return "Premature end of slice segment";
  case DE265_WARNING_INCORRECT_ENTRY_POINT_OFFSET:
    return "Incorrect entry-point offset";
  case DE265_WARNING_CTB_OUTSIDE_IMAGE_AREA:
    return "CTB outside of image area (concealing stream error...)";
  case DE265_WARNING_SPS_HEADER_INVALID:
    return "sps header invalid";
  case DE265_WARNING_PPS_HEADER_INVALID:
    return "pps header invalid";
  case DE265_WARNING_SLICEHEADER_INVALID:
    return "slice header invalid";
  case DE265_WARNING_INCORRECT_MOTION_VECTOR_SCALING:
    return "impossible motion vector scaling";
  case DE265_WARNING_NONEXISTING_PPS_REFERENCED:
    return "non-existing PPS referenced";
  case DE265_WARNING_NONEXISTING_SPS_REFERENCED:
    return "non-existing SPS referenced";
  case DE265_WARNING_BOTH_PREDFLAGS_ZERO:
    return "both predFlags[] are zero in MC";
  case DE265_WARNING_NONEXISTING_REFERENCE_PICTURE_ACCESSED:
    return "non-existing reference picture accessed";
  case DE265_WARNING_NUMMVP_NOT_EQUAL_TO_NUMMVQ:
    return "numMV_P != numMV_Q in deblocking";
  case DE265_WARNING_NUMBER_OF_SHORT_TERM_REF_PIC_SETS_OUT_OF_RANGE:
    return "number of short-term ref-pic-sets out of range";
  case DE265_WARNING_SHORT_TERM_REF_PIC_SET_OUT_OF_RANGE:
    return "short-term ref-pic-set index out of range";
  case DE265_WARNING_FAULTY_REFERENCE_PICTURE_LIST:
    return "faulty reference picture list";
  case DE265_WARNING_EOSS_BIT_NOT_SET:
    return "end_of_sub_stream_one_bit not set to 1 when it should be";
  case DE265_WARNING_MAX_NUM_REF_PICS_EXCEEDED:
    return "maximum number of reference pictures exceeded";
  case DE265_WARNING_INVALID_CHROMA_FORMAT:
    return "invalid chroma format in SPS header";
  case DE265_WARNING_SLICE_SEGMENT_ADDRESS_INVALID:
    return "slice segment address invalid";
  case DE265_WARNING_DEPENDENT_SLICE_WITH_ADDRESS_ZERO:
    return "dependent slice with address 0";
  case DE265_WARNING_NUMBER_OF_THREADS_LIMITED_TO_MAXIMUM:
    return "number of threads limited to maximum amount";
  case DE265_NON_EXISTING_LT_REFERENCE_CANDIDATE_IN_SLICE_HEADER:
    return "non-existing long-term reference candidate specified in slice header";

  default: return "unknown error";
  }
}

LIBDE265_API int de265_isOK(de265_error err)
{
  return err == DE265_OK || err >= 1000;
}



ALIGNED_8(static de265_sync_int de265_init_count) = 0;

LIBDE265_API de265_error de265_init()
{
  int cnt = de265_sync_add_and_fetch(&de265_init_count,1);
  if (cnt>1) {
    // we are not the first -> already initialized

    return DE265_OK;
  }


  // do initializations

  init_scan_orders();

  if (!alloc_and_init_significant_coeff_ctxIdx_lookupTable()) {
    de265_sync_sub_and_fetch(&de265_init_count,1);
    return DE265_ERROR_LIBRARY_INITIALIZATION_FAILED;
  }

  return DE265_OK;
}

LIBDE265_API de265_error de265_free()
{
  int cnt = de265_sync_sub_and_fetch(&de265_init_count,1);
  if (cnt<0) {
    de265_sync_add_and_fetch(&de265_init_count,1);
    return DE265_ERROR_LIBRARY_NOT_INITIALIZED;
  }

  if (cnt==0) {
    free_significant_coeff_ctxIdx_lookupTable();
  }

  return DE265_OK;
}


LIBDE265_API de265_decoder_context* de265_new_decoder()
{
  de265_error init_err = de265_init();
  if (init_err != DE265_OK) {
    return NULL;
  }

  decoder_context* ctx = new decoder_context;
  if (!ctx) {
    de265_free();
    return NULL;
  }

  return (de265_decoder_context*)ctx;
}


LIBDE265_API de265_error de265_free_decoder(de265_decoder_context* de265ctx)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  ctx->stop_thread_pool();

  delete ctx;

  return de265_free();
}


LIBDE265_API de265_error de265_start_worker_threads(de265_decoder_context* de265ctx, int number_of_threads)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  if (number_of_threads > MAX_THREADS) {
    number_of_threads = MAX_THREADS;
  }

  if (number_of_threads>0) {
    de265_error err = ctx->start_thread_pool(number_of_threads);
    if (de265_isOK(err)) {
      err = DE265_OK;
    }
    return err;
  }
  else {
    return DE265_OK;
  }
}


#ifndef LIBDE265_DISABLE_DEPRECATED
LIBDE265_API de265_error de265_decode_data(de265_decoder_context* de265ctx,
                                           const void* data8, int len)
{
  //decoder_context* ctx = (decoder_context*)de265ctx;
  de265_error err;
  if (len > 0) {
    err = de265_push_data(de265ctx, data8, len, 0, NULL);
  } else {
    err = de265_flush_data(de265ctx);
  }
  if (err != DE265_OK) {
    return err;
  }

  int more = 0;
  do {
    err = de265_decode(de265ctx, &more);
    if (err != DE265_OK) {
        more = 0;
    }

    switch (err) {
    case DE265_ERROR_WAITING_FOR_INPUT_DATA:
      // ignore error (didn't exist in 0.4 and before)
      err = DE265_OK;
      break;
    default:
      break;
    }
  } while (more);
  return err;
}
#endif

LIBDE265_API de265_error de265_push_data(de265_decoder_context* de265ctx,
                                         const void* data8, int len,
                                         de265_PTS pts, void* user_data)
{
  decoder_context* ctx = (decoder_context*)de265ctx;
  uint8_t* data = (uint8_t*)data8;

  return ctx->nal_parser.push_data(data,len,pts,user_data);
}


LIBDE265_API de265_error de265_push_NAL(de265_decoder_context* de265ctx,
                                        const void* data8, int len,
                                        de265_PTS pts, void* user_data)
{
  decoder_context* ctx = (decoder_context*)de265ctx;
  uint8_t* data = (uint8_t*)data8;

  return ctx->nal_parser.push_NAL(data,len,pts,user_data);
}


LIBDE265_API de265_error de265_decode(de265_decoder_context* de265ctx, int* more)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  return ctx->decode(more);
}


LIBDE265_API void        de265_push_end_of_NAL(de265_decoder_context* de265ctx)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  ctx->nal_parser.flush_data();
}


LIBDE265_API de265_error de265_flush_data(de265_decoder_context* de265ctx)
{
  de265_push_end_of_NAL(de265ctx);

  decoder_context* ctx = (decoder_context*)de265ctx;

  ctx->nal_parser.flush_data();
  ctx->nal_parser.mark_end_of_stream();

  return DE265_OK;
}


LIBDE265_API void de265_reset(de265_decoder_context* de265ctx)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  ctx->reset();
}


LIBDE265_API const struct de265_image* de265_get_next_picture(de265_decoder_context* de265ctx)
{
  const struct de265_image* img = de265_peek_next_picture(de265ctx);
  if (img) {
    de265_release_next_picture(de265ctx);
  }

  return img;
}


LIBDE265_API const struct de265_image* de265_peek_next_picture(de265_decoder_context* de265ctx)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  if (ctx->num_pictures_in_output_queue()>0) {
    de265_image* img = ctx->get_next_picture_in_output_queue();
    return img;
  }
  else {
    return NULL;
  }
}


LIBDE265_API void de265_release_next_picture(de265_decoder_context* de265ctx)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  // no active output picture -> ignore release request

  if (ctx->num_pictures_in_output_queue()==0) { return; }

  de265_image* next_image = ctx->get_next_picture_in_output_queue();

  loginfo(LogDPB, "release DPB with POC=%d\n",next_image->PicOrderCntVal);

  next_image->PicOutputFlag = false;

  // pop output queue

  ctx->pop_next_picture_in_output_queue();
}


LIBDE265_API de265_error de265_get_warning(de265_decoder_context* de265ctx)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  return ctx->get_warning();
}

LIBDE265_API void de265_set_parameter_bool(de265_decoder_context* de265ctx, enum de265_param param, int value)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  switch (param)
    {
    case DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH:
      ctx->param_sei_check_hash = !!value;
      break;

    case DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES:
      ctx->param_suppress_faulty_pictures = !!value;
      break;

    default:
      assert(false);
      break;
    }
}


LIBDE265_API void de265_set_parameter_int(de265_decoder_context* de265ctx, enum de265_param param, int value)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  switch (param)
    {
    case DE265_DECODER_PARAM_DUMP_SPS_HEADERS:
      ctx->param_sps_headers_fd = value;
      break;

    case DE265_DECODER_PARAM_DUMP_VPS_HEADERS:
      ctx->param_vps_headers_fd = value;
      break;

    case DE265_DECODER_PARAM_DUMP_PPS_HEADERS:
      ctx->param_pps_headers_fd = value;
      break;

    case DE265_DECODER_PARAM_DUMP_SLICE_HEADERS:
      ctx->param_slice_headers_fd = value;
      break;

    case DE265_DECODER_PARAM_ACCELERATION_CODE:
      ctx->set_acceleration_functions((enum de265_acceleration)value);
      break;

    default:
      assert(false);
      break;
    }
}




LIBDE265_API int de265_get_parameter_bool(de265_decoder_context* de265ctx, enum de265_param param)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  switch (param)
    {
    case DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH:
      return ctx->param_sei_check_hash;

    case DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES:
      return ctx->param_suppress_faulty_pictures;

    default:
      assert(false);
      return false;
    }
}


LIBDE265_API int de265_get_number_of_input_bytes_pending(de265_decoder_context* de265ctx)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  return ctx->nal_parser.bytes_in_input_queue();
}


LIBDE265_API int de265_get_number_of_NAL_units_pending(de265_decoder_context* de265ctx)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  return ctx->nal_parser.number_of_NAL_units_pending();
}


LIBDE265_API int de265_get_image_width(const struct de265_image* img,int channel)
{
  switch (channel) {
  case 0:
    return img->width_confwin;
  case 1:
  case 2:
    return img->chroma_width_confwin;
  default:
    return 0;
  }
}

LIBDE265_API int de265_get_image_height(const struct de265_image* img,int channel)
{
  switch (channel) {
  case 0:
    return img->height_confwin;
  case 1:
  case 2:
    return img->chroma_height_confwin;
  default:
    return 0;
  }
}

LIBDE265_API enum de265_chroma de265_get_chroma_format(const struct de265_image* img)
{
  return img->get_chroma_format();
}

LIBDE265_API const uint8_t* de265_get_image_plane(const de265_image* img, int channel, int* stride)
{
  assert(channel>=0 && channel <= 2);

  uint8_t* data = img->pixels_confwin[channel];

  if (stride) *stride = img->get_image_stride(channel);

  return data;
}

LIBDE265_API void de265_image_set_image_plane(de265_image* img, int cIdx, void* mem, int stride)
{
  img->set_image_plane(cIdx, (uint8_t*)mem, stride);
}

LIBDE265_API void de265_set_image_allocation_functions(de265_decoder_context* de265ctx,
                                                       de265_image_allocation* allocfunc)
{
  decoder_context* ctx = (decoder_context*)de265ctx;

  ctx->set_image_allocation_functions(allocfunc);
}

LIBDE265_API de265_PTS de265_get_image_PTS(const struct de265_image* img)
{
  return img->pts;
}

LIBDE265_API void* de265_get_image_user_data(const struct de265_image* img)
{
  return img->user_data;
}

LIBDE265_API void de265_get_image_NAL_header(const struct de265_image* img,
                                             int* nal_unit_type,
                                             const char** nal_unit_name,
                                             int* nuh_layer_id,
                                             int* nuh_temporal_id)
{
  if (nal_unit_type)   *nal_unit_type   = img->nal_hdr.nal_unit_type;
  if (nal_unit_name)   *nal_unit_name   = get_NAL_name(img->nal_hdr.nal_unit_type);
  if (nuh_layer_id)    *nuh_layer_id    = img->nal_hdr.nuh_layer_id;
  if (nuh_temporal_id) *nuh_temporal_id = img->nal_hdr.nuh_temporal_id;
}
}

