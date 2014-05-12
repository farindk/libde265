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

#include "image.h"
#include "decctx.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef HAVE_SSE4_1
#define MEMORY_PADDING  8
#else
#define MEMORY_PADDING  0
#endif

#ifdef HAVE___MINGW_ALIGNED_MALLOC
#define ALLOC_ALIGNED(alignment, size)         __mingw_aligned_malloc((size), (alignment))
#define FREE_ALIGNED(mem)                      __mingw_aligned_free((mem))
#elif _WIN32
#define ALLOC_ALIGNED(alignment, size)         _aligned_malloc((size), (alignment))
#define FREE_ALIGNED(mem)                      _aligned_free((mem))
#elif __APPLE__
static inline void *ALLOC_ALIGNED(size_t alignment, size_t size) {
    void *mem = NULL;
    if (posix_memalign(&mem, alignment, size) != 0) {
        return NULL;
    }
    return mem;
};
#define FREE_ALIGNED(mem)                      free((mem))
#else
#define ALLOC_ALIGNED(alignment, size)      memalign((alignment), (size))
#define FREE_ALIGNED(mem)                   free((mem))
#endif

#define ALLOC_ALIGNED_16(size)              ALLOC_ALIGNED(16, size)

static const int alignment = 16;


static int  de265_image_get_buffer(de265_image_spec* spec, de265_image* img)
{
  int luma_stride   = (spec->width   + spec->alignment-1) / spec->alignment * spec->alignment;
  int chroma_stride = (spec->width/2 + spec->alignment-1) / spec->alignment * spec->alignment;

  int luma_height   = spec->height;
  int chroma_height = (spec->height+1)/2;

  uint8_t* p[3] = { 0,0,0 };
  p[0] = (uint8_t *)ALLOC_ALIGNED_16(luma_stride   * luma_height   + MEMORY_PADDING);
  p[1] = (uint8_t *)ALLOC_ALIGNED_16(chroma_stride * chroma_height + MEMORY_PADDING);
  p[2] = (uint8_t *)ALLOC_ALIGNED_16(chroma_stride * chroma_height + MEMORY_PADDING);

  if (p[0]==NULL || p[1]==NULL || p[2]==NULL) {
    for (int i=0;i<3;i++)
      if (p[i]) {
        FREE_ALIGNED(p[i]);
      }

    return 0;
  }

  de265_image_set_image_plane(img, 0, p[0], luma_stride);
  de265_image_set_image_plane(img, 1, p[1], chroma_stride);
  de265_image_set_image_plane(img, 2, p[2], chroma_stride);

  return 1;
}

static void de265_image_release_buffer(de265_image* img)
{
  for (int i=0;i<3;i++) {
    uint8_t* p = (uint8_t*)img->get_image_plane(i);
    assert(p);
    FREE_ALIGNED(p);
  }
}


de265_image_allocation de265_image::default_image_allocation = {
  de265_image_get_buffer,
  de265_image_release_buffer
};


void de265_image::set_image_plane(int cIdx, uint8_t* mem, int stride)
{
  pixels[cIdx] = mem;

  if (cIdx==0) { this->stride        = stride; }
  else         { this->chroma_stride = stride; }
}


de265_image::de265_image()
{
  alloc_functions.get_buffer = NULL;
  alloc_functions.release_buffer = NULL;

  for (int c=0;c<3;c++) {
    pixels[c] = NULL;
    pixels_confwin[c] = NULL;
  }

  width=height=0;

  pts = 0;
  user_data = NULL;

  ctb_progress = NULL;

  tasks_pending = 0;
  integrity = INTEGRITY_NOT_DECODED;

  picture_order_cnt_lsb = -1; // undefined
  PicOrderCntVal = -1; // undefined
  PicState = UnusedForReference;
  PicOutputFlag = false;

  de265_mutex_init(&mutex);
  de265_cond_init(&finished_cond);
}



de265_error de265_image::alloc_image(int w,int h, enum de265_chroma c,
                                     const seq_parameter_set* sps,
                                     const de265_image_allocation* allocfunc)
{
  decctx = NULL;

  // --- allocate image buffer (or reuse old one) ---

  if (width != w || height != h || chroma_format != c) {

    chroma_format= c;

    width = w;
    height = h;
    chroma_width = w;
    chroma_height= h;

    de265_image_spec spec;

    switch (chroma_format) {
    case de265_chroma_420:
      spec.format = de265_image_format_YUV420P8;
      chroma_width  = (chroma_width +1)/2;
      chroma_height = (chroma_height+1)/2;
      break;

    case de265_chroma_422:
      spec.format = de265_image_format_YUV422P8;
      chroma_height = (chroma_height+1)/2;
      break;
    }

    spec.width  = w;
    spec.height = h;
    spec.alignment = 16;

    // TODO: conformance window
    spec.visible_width = w;
    spec.visible_height = h;

    int success = allocfunc->get_buffer(&spec, this);

    alloc_functions = *allocfunc;


    // check for memory shortage

    if (!success)
    {
      return DE265_ERROR_OUT_OF_MEMORY;
    }
  }

  // --- allocate decoding info arrays ---

  bool mem_alloc_success = true;

  if (sps) {
    // intra pred mode

    mem_alloc_success &= intraPredMode.alloc(sps->PicWidthInMinPUs, sps->PicHeightInMinPUs,
                                             sps->Log2MinPUSize);

    // cb info

    mem_alloc_success &= cb_info.alloc(sps->PicWidthInMinCbsY, sps->PicHeightInMinCbsY,
                                       sps->Log2MinCbSizeY);

    // pb info

    int puWidth  = sps->PicWidthInMinCbsY  << (sps->Log2MinCbSizeY -2);
    int puHeight = sps->PicHeightInMinCbsY << (sps->Log2MinCbSizeY -2);

    mem_alloc_success &= pb_info.alloc(puWidth,puHeight, 2);


    // tu info

    mem_alloc_success &= tu_info.alloc(sps->PicWidthInTbsY, sps->PicHeightInTbsY,
                                       sps->Log2MinTrafoSize);

    // deblk info

    int deblk_w = (sps->pic_width_in_luma_samples +3)/4;
    int deblk_h = (sps->pic_height_in_luma_samples+3)/4;

    mem_alloc_success &= deblk_info.alloc(deblk_w, deblk_h, 2);

    // CTB info

    if (ctb_info.data_size != sps->PicSizeInCtbsY)
      {
        for (int i=0;i<ctb_info.data_size;i++)
          { de265_progress_lock_destroy(&ctb_progress[i]); }

        free(ctb_progress);

        mem_alloc_success &= ctb_info.alloc(sps->PicWidthInCtbsY, sps->PicHeightInCtbsY,
                                            sps->Log2CtbSizeY);

        ctb_progress = (de265_progress_lock*)malloc( sizeof(de265_progress_lock)
                                                     * ctb_info.data_size);

        for (int i=0;i<ctb_info.data_size;i++)
          { de265_progress_lock_init(&ctb_progress[i]); }
      }


    // check for memory shortage

    if (!mem_alloc_success)
      {
        return DE265_ERROR_OUT_OF_MEMORY;
      }
  }

  return DE265_OK;
}


de265_image::~de265_image()
{
  if (alloc_functions.release_buffer) {
    alloc_functions.release_buffer(this);
  }

  for (int i=0;i<ctb_info.data_size;i++)
    { de265_progress_lock_destroy(&ctb_progress[i]); }


  for (int i=0;i<slices.size();i++) {
    delete slices[i];
  }
  slices.clear();

  free(ctb_progress);

  de265_cond_destroy(&finished_cond);
  de265_mutex_destroy(&mutex);
}


void de265_image::fill_image(int y,int cb,int cr)
{
  if (y>=0) {
    memset(pixels[0], y, stride * height);
  }

  if (cb>=0) {
    memset(pixels[1], cb, chroma_stride * chroma_height);
  }

  if (cr>=0) {
    memset(pixels[2], cr, chroma_stride * chroma_height);
  }
}


void de265_image::copy_image(const de265_image* src)
{
  alloc_image(src->width, src->height, src->chroma_format, NULL, &src->alloc_functions);

  assert(src->stride == stride &&
         src->chroma_stride == chroma_stride);


  if (src->stride == stride) {
    memcpy(pixels[0], src->pixels[0], src->height*src->stride);
  }
  else {
    for (int yp=0;yp<src->height;yp++) {
      memcpy(pixels[0]+yp*stride, src->pixels[0]+yp*src->stride, src->width);
    }
  }

  if (src->chroma_format != de265_chroma_mono) {
    if (src->chroma_stride == chroma_stride) {
      memcpy(pixels[1], src->pixels[1], src->chroma_height*src->chroma_stride);
      memcpy(pixels[2], src->pixels[2], src->chroma_height*src->chroma_stride);
    }
    else {
      for (int y=0;y<src->chroma_height;y++) {
        memcpy(pixels[1]+y*chroma_stride, src->pixels[1]+y*src->chroma_stride, src->chroma_width);
        memcpy(pixels[2]+y*chroma_stride, src->pixels[2]+y*src->chroma_stride, src->chroma_width);
      }
    }
  }
}


void de265_image::set_conformance_window()
{
  int left   = sps.conf_win_left_offset;
  int right  = sps.conf_win_right_offset;
  int top    = sps.conf_win_top_offset;
  int bottom = sps.conf_win_bottom_offset;

  int WinUnitX, WinUnitY;

  switch (chroma_format) {
  case de265_chroma_mono: WinUnitX=1; WinUnitY=1; break;
  case de265_chroma_420:  WinUnitX=2; WinUnitY=2; break;
  case de265_chroma_422:  WinUnitX=2; WinUnitY=1; break;
  case de265_chroma_444:  WinUnitX=1; WinUnitY=1; break;
  default:
    assert(0);
  }

  pixels_confwin[0] = pixels[0] + left*WinUnitX + top*WinUnitY*stride;
  pixels_confwin[1] = pixels[1] + left + top*chroma_stride;
  pixels_confwin[2] = pixels[2] + left + top*chroma_stride;

  width_confwin = width - (left+right)*WinUnitX;
  height_confwin= height- (top+bottom)*WinUnitY;
  chroma_width_confwin = chroma_width -left-right;
  chroma_height_confwin= chroma_height-top-bottom;
}

void de265_image::increase_pending_tasks(int n)
{
  de265_sync_add_and_fetch(&tasks_pending, n);
}

void de265_image::decrease_pending_tasks(int n)
{
  de265_mutex_lock(&mutex);

  int pending = de265_sync_sub_and_fetch(&tasks_pending, n);

  assert(pending >= 0);

  if (pending==0) {
    de265_cond_broadcast(&finished_cond, &mutex);
  }

  de265_mutex_unlock(&mutex);
}

void de265_image::wait_for_completion()
{
  de265_mutex_lock(&mutex);
  while (tasks_pending>0) {
    de265_cond_wait(&finished_cond, &mutex);
  }
  de265_mutex_unlock(&mutex);
}



void de265_image::clear_metadata()
{
  for (int i=0;i<slices.size();i++)
    delete slices[i];
  slices.clear();

  // TODO: maybe we could avoid the memset by ensuring that all data is written to
  // during decoding (especially log2CbSize), but it is unlikely to be faster than the memset.

  cb_info.clear();
  tu_info.clear();
  ctb_info.clear();
  deblk_info.clear();

  // --- reset CTB progresses ---

  for (int i=0;i<ctb_info.data_size;i++) {
    ctb_progress[i].progress = CTB_PROGRESS_NONE;
  }
}


void de265_image::set_mv_info(int x,int y, int nPbW,int nPbH, const PredVectorInfo* mv)
{
  int log2PuSize = 2;

  int xPu = x >> log2PuSize;
  int yPu = y >> log2PuSize;
  int wPu = nPbW >> log2PuSize;
  int hPu = nPbH >> log2PuSize;

  int stride = pb_info.width_in_units;

  for (int pby=0;pby<hPu;pby++)
    for (int pbx=0;pbx<wPu;pbx++)
      {
        pb_info[ xPu+pbx + (yPu+pby)*stride ].mvi = *mv;
      }
}


bool de265_image::available_zscan(int xCurr,int yCurr, int xN,int yN) const
{
  if (xN<0 || yN<0) return false;
  if (xN>=sps.pic_width_in_luma_samples ||
      yN>=sps.pic_height_in_luma_samples) return false;

  int minBlockAddrN = pps.MinTbAddrZS[ (xN>>sps.Log2MinTrafoSize) +
                                       (yN>>sps.Log2MinTrafoSize) * sps.PicWidthInTbsY ];
  int minBlockAddrCurr = pps.MinTbAddrZS[ (xCurr>>sps.Log2MinTrafoSize) +
                                          (yCurr>>sps.Log2MinTrafoSize) * sps.PicWidthInTbsY ];

  if (minBlockAddrN > minBlockAddrCurr) return false;

  int xCurrCtb = xCurr >> sps.Log2CtbSizeY;
  int yCurrCtb = yCurr >> sps.Log2CtbSizeY;
  int xNCtb = xN >> sps.Log2CtbSizeY;
  int yNCtb = yN >> sps.Log2CtbSizeY;

  if (get_SliceAddrRS(xCurrCtb,yCurrCtb) !=
      get_SliceAddrRS(xNCtb,   yNCtb)) {
    return false;
  }

  if (pps.TileIdRS[xCurrCtb + yCurrCtb*sps.PicWidthInCtbsY] !=
      pps.TileIdRS[xNCtb    + yNCtb   *sps.PicWidthInCtbsY]) {
    return false;
  }

  return true;
}


bool de265_image::available_pred_blk(int xC,int yC, int nCbS, int xP, int yP,
                                     int nPbW, int nPbH, int partIdx, int xN,int yN) const
{
  logtrace(LogMotion,"C:%d;%d P:%d;%d N:%d;%d size=%d;%d\n",xC,yC,xP,yP,xN,yN,nPbW,nPbH);

  int sameCb = (xC <= xN && xN < xC+nCbS &&
                yC <= yN && yN < yC+nCbS);

  bool availableN;

  if (!sameCb) {
    availableN = available_zscan(xP,yP,xN,yN);
  }
  else {
    availableN = !(nPbW<<1 == nCbS && nPbH<<1 == nCbS &&
                   partIdx==1 &&
                   yN >= yC+nPbH && xN < xC+nPbW);
  }

  if (availableN && get_pred_mode(xN,yN) == MODE_INTRA) {
    availableN = false;
  }

  return availableN;
}

