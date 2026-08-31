#include "image.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <nsgif.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/demux.h>

#define PIXEL_BUDGET (96u * 1024u * 1024u)
#define MAX_FRAMES 512u

static bool img_push_frame(img_image *img, uint32_t w, uint32_t h,
                           uint32_t delay_ms) {
  if (w == 0 || h == 0 || img->nframes >= MAX_FRAMES) return false;

  uint64_t bytes = (uint64_t)w * (uint64_t)h * 4u;
  uint64_t total = bytes;
  for (size_t i = 0; i < img->nframes; i++)
    total += (uint64_t)img->frames[i].width * img->frames[i].height * 4u;
  if (total > PIXEL_BUDGET) return false;

  img_frame *nf = realloc(img->frames, (img->nframes + 1) * sizeof(*nf));
  if (nf == nullptr) return false;
  img->frames = nf;

  img_frame *f = &img->frames[img->nframes];
  f->width = w;
  f->height = h;
  f->delay_ms = delay_ms;
  f->rgba = malloc((size_t)bytes);
  if (f->rgba == nullptr) return false;
  img->nframes++;
  return true;
}

void img_free(img_image *img) {
  if (img == nullptr) return;
  for (size_t i = 0; i < img->nframes; i++) free(img->frames[i].rgba);
  free(img->frames);
  img->frames = nullptr;
  img->nframes = 0;
}

typedef struct {
  const uint8_t *data;
  size_t len;
  size_t off;
} png_reader;

static void png_read_fn(png_structp pngp, png_bytep out, png_size_t n) {
  png_reader *r = (png_reader *)png_get_io_ptr(pngp);
  if (r->len - r->off < n) {
    png_error(pngp, "short read");
    return;
  }
  memcpy(out, r->data + r->off, n);
  r->off += n;
}

static void png_silent(png_structp pngp, png_const_charp msg) {
  (void)pngp;
  (void)msg;
}

static int img_decode_png(const uint8_t *data, size_t len, img_image *img) {
  png_reader r = {data, len, 0};
  png_structp pngp = png_create_read_struct(
      PNG_LIBPNG_VER_STRING, nullptr, png_silent, png_silent);
  if (pngp == nullptr) return -1;
  png_infop info = png_create_info_struct(pngp);
  if (info == nullptr) {
    png_destroy_read_struct(&pngp, nullptr, nullptr);
    return -1;
  }
  if (setjmp(png_jmpbuf(pngp))) {
    png_destroy_read_struct(&pngp, &info, nullptr);
    return -1;
  }

  png_set_read_fn(pngp, &r, png_read_fn);
  png_read_info(pngp, info);

  png_uint_32 w = png_get_image_width(pngp, info);
  png_uint_32 h = png_get_image_height(pngp, info);
  int bit_depth = png_get_bit_depth(pngp, info);
  int color_type = png_get_color_type(pngp, info);

  if (bit_depth == 16) png_set_strip_16(pngp);
  png_set_expand(pngp);
  png_set_gray_to_rgb(pngp);
  if (!(color_type & PNG_COLOR_MASK_ALPHA))
    png_set_filler(pngp, 0xFF, PNG_FILLER_AFTER);

  png_read_update_info(pngp, info);
  if (png_get_channels(pngp, info) != 4) {
    png_destroy_read_struct(&pngp, &info, nullptr);
    return -1;
  }
  if ((uint64_t)w * h * 4u > PIXEL_BUDGET) {
    png_destroy_read_struct(&pngp, &info, nullptr);
    return -1;
  }
  if (!img_push_frame(img, w, h, 0)) {
    png_destroy_read_struct(&pngp, &info, nullptr);
    return -1;
  }
  img_frame *f = &img->frames[img->nframes - 1];

  png_bytep *rows = malloc((size_t)h * sizeof(*rows));
  if (rows == nullptr) {
    png_destroy_read_struct(&pngp, &info, nullptr);
    return -1;
  }
  for (png_uint_32 y = 0; y < h; y++) rows[y] = f->rgba + (size_t)y * w * 4;

  png_read_image(pngp, rows);
  free(rows);
  png_read_end(pngp, nullptr);
  png_destroy_read_struct(&pngp, &info, nullptr);
  return 0;
}

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} png_sink;

static void png_write_fn(png_structp pngp, png_bytep buf, png_size_t n) {
  png_sink *s = (png_sink *)png_get_io_ptr(pngp);
  if (s->len + n > s->cap) {
    size_t nc = s->cap ? s->cap : 4096;
    while (nc < s->len + n) nc *= 2;
    uint8_t *nd = realloc(s->data, nc);
    if (nd == nullptr) png_error(pngp, "out of memory");
    s->data = nd;
    s->cap = nc;
  }
  memcpy(s->data + s->len, buf, n);
  s->len += n;
}

int img_encode_png(const img_frame *f, uint8_t **out,
                  size_t *outlen) {
  if (f == nullptr || f->rgba == nullptr || f->width == 0 || f->height == 0)
    return -1;
  if ((uint64_t)f->width * f->height * 4u > PIXEL_BUDGET) return -1;

  png_structp pngp = png_create_write_struct(
      PNG_LIBPNG_VER_STRING, nullptr, png_silent, png_silent);
  if (pngp == nullptr) return -1;
  png_infop info = png_create_info_struct(pngp);
  if (info == nullptr) {
    png_destroy_write_struct(&pngp, nullptr);
    return -1;
  }
  if (setjmp(png_jmpbuf(pngp))) {
    png_destroy_write_struct(&pngp, &info);
    return -1;
  }

  png_sink sink = {0};
  png_set_write_fn(pngp, &sink, png_write_fn, nullptr);
  png_set_IHDR(pngp, info, f->width, f->height, 8, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(pngp, info);

  png_bytep *rows = malloc((size_t)f->height * sizeof(*rows));
  if (rows == nullptr) {
    png_destroy_write_struct(&pngp, &info);
    return -1;
  }
  for (uint32_t y = 0; y < f->height; y++)
    rows[y] = (png_bytep)(f->rgba + (size_t)y * f->width * 4);

  png_write_image(pngp, rows);
  free(rows);
  png_write_end(pngp, nullptr);
  png_destroy_write_struct(&pngp, &info);

  *out = sink.data;
  *outlen = sink.len;
  return 0;
}

typedef struct {
  struct jpeg_error_mgr pub;
  jmp_buf jb;
} jpeg_err;

static void jpeg_err_exit(j_common_ptr cinfo) {
  jpeg_err *err = (jpeg_err *)cinfo->err;
  longjmp(err->jb, 1);
}

static int img_decode_jpeg(const uint8_t *data, size_t len, img_image *img) {
  struct jpeg_decompress_struct cinfo;
  jpeg_err err;
  cinfo.err = jpeg_std_error(&err.pub);
  err.pub.error_exit = jpeg_err_exit;
  if (setjmp(err.jb)) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, (unsigned long)len);
  jpeg_read_header(&cinfo, TRUE);

  cinfo.out_color_space = JCS_EXT_RGBA;
  jpeg_start_decompress(&cinfo);

  JDIMENSION w = cinfo.output_width;
  JDIMENSION h = cinfo.output_height;
  if (cinfo.output_components != 4 || (uint64_t)w * h * 4u > PIXEL_BUDGET) {
    jpeg_abort_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }
  if (!img_push_frame(img, w, h, 0)) {
    jpeg_abort_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }
  img_frame *f = &img->frames[img->nframes - 1];

  JSAMPLE *row = malloc((size_t)w * 4u);
  if (row == nullptr) {
    jpeg_abort_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }
  while (cinfo.output_scanline < h) {
    JDIMENSION y = cinfo.output_scanline;
    JSAMPROW out_row = row;
    jpeg_read_scanlines(&cinfo, &out_row, 1);
    memcpy(f->rgba + (size_t)y * w * 4u, row, (size_t)w * 4u);
  }
  free(row);
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return 0;
}

typedef struct {
  uint8_t *buf;
} nsgif_bitmap;

static nsgif_bitmap_t *nsgif_bm_create(int width, int height) {
  nsgif_bitmap *b = calloc(1, sizeof(*b));
  if (b == nullptr) return nullptr;
  b->buf = calloc((size_t)width * (size_t)height * 4u, 1);
  if (b->buf == nullptr) {
    free(b);
    return nullptr;
  }
  return (nsgif_bitmap_t *)b;
}

static void nsgif_bm_destroy(nsgif_bitmap_t *bm) {
  nsgif_bitmap *b = (nsgif_bitmap *)bm;
  if (b == nullptr) return;
  free(b->buf);
  free(b);
}

static uint8_t *nsgif_bm_get_buffer(nsgif_bitmap_t *bm) {
  return ((nsgif_bitmap *)bm)->buf;
}

static const nsgif_bitmap_cb_vt nsgif_vt = {
    nsgif_bm_create,
    nsgif_bm_destroy,
    nsgif_bm_get_buffer,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

static int img_decode_gif(const uint8_t *data, size_t len, img_image *img) {
  nsgif_t *gif = nullptr;
  nsgif_error e = nsgif_create(&nsgif_vt, NSGIF_BITMAP_FMT_R8G8B8A8, &gif);
  if (e != NSGIF_OK || gif == nullptr) return -1;

  nsgif_data_scan(gif, len, data);
  nsgif_data_complete(gif);
  const nsgif_info_t *info = nsgif_get_info(gif);
  if (info == nullptr || info->width == 0 || info->height == 0) {
    nsgif_destroy(gif);
    return -1;
  }

  for (uint32_t i = 0; i < info->frame_count; i++) {
    nsgif_bitmap_t *bm = nullptr;
    e = nsgif_frame_decode(gif, i, &bm);
    if (e != NSGIF_OK || bm == nullptr) continue;
    uint8_t *src = nsgif_bm_get_buffer(bm);
    if (!img_push_frame(img, info->width, info->height, 0)) break;
    img_frame *f = &img->frames[img->nframes - 1];
    memcpy(f->rgba, src, (size_t)info->width * (size_t)info->height * 4u);
  }

  if (img->nframes > 0) {
    for (uint32_t i = 0; i < (uint32_t)img->nframes; i++) {
      const nsgif_frame_info_t *ff = nsgif_get_frame_info(gif, i);
      if (ff != nullptr && ff->delay != UINT32_MAX)
        img->frames[i].delay_ms = ff->delay * 10u; /* centis -> ms */
    }
    img->loop_count = (uint32_t)info->loop_max; /* 0 = loop forever */
  }
  nsgif_destroy(gif);
  return img->nframes > 0 ? 0 : -1;
}

static int img_decode_webp(const uint8_t *data, size_t len, img_image *img) {
  WebPData wd = {data, len};

  /* Animated WebP: full canvas RGBA frames plus per-frame timestamps. */
  WebPDemuxer *dmux = WebPDemux(&wd);
  if (dmux != nullptr) {
    uint32_t flags = WebPDemuxGetI(dmux, WEBP_FF_FORMAT_FLAGS);
    if (flags & ANIMATION_FLAG) {
      WebPAnimDecoderOptions opts;
      if (WebPAnimDecoderOptionsInit(&opts)) {
        opts.color_mode = MODE_RGBA;
        WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opts);
        WebPDemuxDelete(dmux);
        if (dec == nullptr) return -1;

        WebPAnimInfo ai;
        if (!WebPAnimDecoderGetInfo(dec, &ai) || ai.canvas_width == 0 ||
            ai.canvas_height == 0) {
          WebPAnimDecoderDelete(dec);
          return -1;
        }

        /* Frame delay: each frame shows until the next one's timestamp. */
        int *ts_list = calloc(ai.frame_count, sizeof(*ts_list));
        while (WebPAnimDecoderHasMoreFrames(dec)) {
          uint8_t *buf = nullptr;
          int ts = 0;
          if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) break;
          if (!img_push_frame(img, ai.canvas_width, ai.canvas_height, 0))
            break;
          img_frame *f = &img->frames[img->nframes - 1];
          memcpy(f->rgba, buf,
                 (size_t)ai.canvas_width * (size_t)ai.canvas_height * 4u);
          if (ts_list != nullptr) ts_list[img->nframes - 1] = ts;
        }
        if (ts_list != nullptr) {
          for (size_t i = 0; i + 1 < img->nframes; i++)
            img->frames[i].delay_ms =
                (uint32_t)(ts_list[i + 1] - ts_list[i]);
          if (img->nframes > 1)
            img->frames[img->nframes - 1].delay_ms =
                img->frames[img->nframes - 2].delay_ms;
          free(ts_list);
        }
        WebPAnimDecoderDelete(dec);
        if (img->nframes > 0) {
          img->loop_count = ai.loop_count; /* 0 = loop forever */
          return 0;
        }
        img_free(img);
        return -1;
      }
    }
    WebPDemuxDelete(dmux);
  }

  /* Static WebP (or demux failed): single RGBA frame. */
  int w = 0, h = 0;
  uint8_t *buf = WebPDecodeRGBA(data, len, &w, &h);
  if (buf == nullptr || w <= 0 || h <= 0) {
    if (buf != nullptr) WebPFree(buf);
    return -1;
  }
  if ((uint64_t)w * (uint64_t)h * 4u > PIXEL_BUDGET ||
      !img_push_frame(img, (uint32_t)w, (uint32_t)h, 0)) {
    WebPFree(buf);
    return -1;
  }
  img_frame *f = &img->frames[img->nframes - 1];
  memcpy(f->rgba, buf, (size_t)w * (size_t)h * 4u);
  WebPFree(buf);
  return 0;
}

int img_decode(const uint8_t *data, size_t len, img_image *out) {
  if (data == nullptr || out == nullptr || len < 8) return -1;
  memset(out, 0, sizeof(*out));

  const uint8_t *d = data;
  if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
    return img_decode_png(data, len, out);
  if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
    return img_decode_jpeg(data, len, out);
  if (memcmp(d, "GIF8", 4) == 0)
    return img_decode_gif(data, len, out);
  if (len >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0)
    return img_decode_webp(data, len, out);
  return -1;
}

void img_fit_aspect(const img_image *im, unsigned cmax, unsigned rmax,
                    unsigned cell_w, unsigned cell_h, unsigned *cols,
                    unsigned *rows) {
  if (cols == nullptr || rows == nullptr) return;
  *cols = cmax > 0 ? cmax : 1;
  *rows = rmax > 0 ? rmax : 1;
  if (im == nullptr || im->nframes == 0 || cmax == 0 || rmax == 0 ||
      cell_w == 0 || cell_h == 0) {
    return;
  }
  double pxw = (double)im->frames[0].width;
  double pxh = (double)im->frames[0].height;
  if (pxw <= 0 || pxh <= 0) return;

  /* Height in cells a given width needs: cols * (pxh/pxw) * (cell_w/cell_h). */
  double h_at_cmax =
      (double)cmax * (pxh / pxw) * ((double)cell_w / (double)cell_h);
  if (h_at_cmax <= (double)rmax) {
    *cols = cmax > 0 ? cmax : 1;
    *rows = (unsigned)(h_at_cmax > 1.0 ? h_at_cmax : 1.0);
  } else {
    *rows = rmax > 0 ? rmax : 1;
    double c_at_rmax =
        (double)rmax * (pxw / pxh) * ((double)cell_h / (double)cell_w);
    *cols = (unsigned)(c_at_rmax > 1.0 ? c_at_rmax : 1.0);
  }
}