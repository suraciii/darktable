/* Engine-owned automatic adjustment helpers shared by the native MCP bridge
   and the channelmixerrgb module.  This file deliberately has no GUI entry
   points so headless callers link the same detector implementation. */

#include "common/chromatic_adaptation.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/dttypes.h"
#include "common/illuminants.h"
#include "common/iop_profile.h"
#include "common/mipmap_cache.h"
#include "develop/develop.h"
#include "develop/openmp_maths.h"
#include "develop/pixelpipe_hb.h"
#include "imageio/imageio_module.h"
#include "iop/channelmixerrgb.h"
#include "common/histogram.h"
#include "common/image_cache.h"
#include "develop/imageop.h"


#include <stdint.h>
#include <string.h>

#define SHF(ii, jj, c) ((i + ii) * width + j + jj) * ch + c
#define OFF 4

/* The detector is also used by the headless MCP boundary.  Keep it compiled
   independently of GUI/experimental feature switches; callers opt in
   explicitly by requesting a qualified automatic operation. */

#if defined(__GNUC__) && defined(_WIN32)
  // On Windows there is a rounding issue making the image full
  // black. For a discussion about the issue and tested solutions see
  // PR #12382).
  #pragma GCC push_options
  #pragma GCC optimize ("-fno-finite-math-only")
#endif

void dt_channelmixerrgb_auto_detect_wb(const float *const restrict in,
                                   float *const restrict temp,
                                   dt_illuminant_t illuminant,
                                   const size_t width,
                                   const size_t height,
                                   const size_t ch,
                                   const dt_colormatrix_t RGB_to_XYZ,
                                   dt_aligned_pixel_t xyz)
{
   /* Detect the chromaticity of the illuminant based on the grey edges hypothesis.
      So we compute a laplacian filter and get the weighted average of its chromaticities

      Inspired by :
      A Fast White Balance Algorithm Based on Pixel Greyness, Ba Thai·Guang Deng·Robert Ross
      https://www.researchgate.net/profile/Ba_Son_Thai/publication/308692177_A_Fast_White_Balance_Algorithm_Based_on_Pixel_Greyness/

      Edge-Based Color Constancy, Joost van de Weijer, Theo Gevers, Arjan Gijsenij
      https://hal.inria.fr/inria-00548686/document
    */
    const float D50[2] = { D50xyY.x, D50xyY.y };
// Convert RGB to xy
  DT_OMP_FOR(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = (i * width + j) * ch;
      dt_aligned_pixel_t RGB;
      dt_aligned_pixel_t XYZ;

      // Clip negatives
      for_each_channel(c,aligned(in))
        RGB[c] = fmaxf(in[index + c], 0.0f);

      // Convert to XYZ
      dot_product(RGB, RGB_to_XYZ, XYZ);

      // Convert to xyY
      const float sum = fmaxf(XYZ[0] + XYZ[1] + XYZ[2], NORM_MIN);
      XYZ[0] /= sum;   // x
      XYZ[2] = XYZ[1]; // Y
      XYZ[1] /= sum;   // y

      // Shift the chromaticity plane so the D50 point (target) becomes the origin
      const float norm = dt_fast_hypotf(D50[0], D50[1]);

      temp[index    ] = (XYZ[0] - D50[0]) / norm;
      temp[index + 1] = (XYZ[1] - D50[1]) / norm;
      temp[index + 2] =  XYZ[2];
    }

  float elements = 0.f;
  dt_aligned_pixel_t xyY = { 0.f };

  if(illuminant == DT_ILLUMINANT_DETECT_SURFACES)
  {
    DT_OMP_FOR(reduction(+:xyY, elements))
    for(size_t i = 2 * OFF; i < height - 4 * OFF; i += OFF)
      for(size_t j = 2 * OFF; j < width - 4 * OFF; j += OFF)
      {
        float DT_ALIGNED_PIXEL central_average[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          // B-spline local average / blur
          central_average[c] = (temp[SHF(-OFF, -OFF, c)]
                                + 2.f * temp[SHF(-OFF, 0, c)]
                                + temp[SHF(-OFF, +OFF, c)]
                                + 2.f * temp[SHF(   0, -OFF, c)]
                                + 4.f * temp[SHF(   0, 0, c)]
                                + 2.f * temp[SHF(   0, +OFF, c)]
                                + temp[SHF(+OFF, -OFF, c)]
                                + 2.f * temp[SHF(+OFF, 0, c)]
                                + temp[SHF(+OFF, +OFF, c)]) / 16.0f;
          central_average[c] = fmaxf(central_average[c], 0.0f);
        }

        dt_aligned_pixel_t var = { 0.f };

        // compute patch-wise variance
        // If variance = 0, we are on a flat surface and want to discard that patch.
        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          var[c] = (  sqf(temp[SHF(-OFF, -OFF, c)] - central_average[c])
                    + sqf(temp[SHF(-OFF,    0, c)] - central_average[c])
                    + sqf(temp[SHF(-OFF, +OFF, c)] - central_average[c])
                    + sqf(temp[SHF(0,    -OFF, c)] - central_average[c])
                    + sqf(temp[SHF(0,       0, c)] - central_average[c])
                    + sqf(temp[SHF(0,    +OFF, c)] - central_average[c])
                    + sqf(temp[SHF(+OFF, -OFF, c)] - central_average[c])
                    + sqf(temp[SHF(+OFF,    0, c)] - central_average[c])
                    + sqf(temp[SHF(+OFF, +OFF, c)] - central_average[c])
                    ) / 9.0f;
        }

        // Compute the patch-wise chroma covariance.
        // If covariance = 0, chroma channels are not correlated and we either have noise or chromatic aberrations.
        // Both ways, we want to discard that patch from the chroma average.
        var[2] = ((temp[SHF(-OFF, -OFF, 0)] - central_average[0]) * (temp[SHF(-OFF, -OFF, 1)] - central_average[1]) +
                  (temp[SHF(-OFF,    0, 0)] - central_average[0]) * (temp[SHF(-OFF,    0, 1)] - central_average[1]) +
                  (temp[SHF(-OFF, +OFF, 0)] - central_average[0]) * (temp[SHF(-OFF, +OFF, 1)] - central_average[1]) +
                  (temp[SHF(   0, -OFF, 0)] - central_average[0]) * (temp[SHF(   0, -OFF, 1)] - central_average[1]) +
                  (temp[SHF(   0,    0, 0)] - central_average[0]) * (temp[SHF(   0,    0, 1)] - central_average[1]) +
                  (temp[SHF(   0, +OFF, 0)] - central_average[0]) * (temp[SHF(   0, +OFF, 1)] - central_average[1]) +
                  (temp[SHF(+OFF, -OFF, 0)] - central_average[0]) * (temp[SHF(+OFF, -OFF, 1)] - central_average[1]) +
                  (temp[SHF(+OFF,    0, 0)] - central_average[0]) * (temp[SHF(+OFF,    0, 1)] - central_average[1]) +
                  (temp[SHF(+OFF, +OFF, 0)] - central_average[0]) * (temp[SHF(+OFF, +OFF, 1)] - central_average[1])
          ) / 9.0f;

        // Compute the Minkowski p-norm for regularization
        const float p = 8.f;
        const float p_norm
            = powf(powf(fabsf(central_average[0]), p)
                   + powf(fabsf(central_average[1]), p), 1.f / p) + NORM_MIN;
        const float weight = var[0] * var[1] * var[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++) xyY[c] += central_average[c] * weight / p_norm;
        elements += weight / p_norm;
      }
  }
  else if(illuminant == DT_ILLUMINANT_DETECT_EDGES)
  {
    DT_OMP_FOR(reduction(+:xyY, elements))
    for(size_t i = 2 * OFF; i < height - 4 * OFF; i += OFF)
      for(size_t j = 2 * OFF; j < width - 4 * OFF; j += OFF)
      {
        float DT_ALIGNED_PIXEL dd[2];
        float DT_ALIGNED_PIXEL central_average[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          // B-spline local average / blur
          central_average[c] = (temp[SHF(-OFF, -OFF, c)]
                                + 2.f * temp[SHF(-OFF, 0, c)]
                                + temp[SHF(-OFF, +OFF, c)]
                                + 2.f * temp[SHF(   0, -OFF, c)]
                                + 4.f * temp[SHF(   0, 0, c)]
                                + 2.f * temp[SHF(   0, +OFF, c)]
                                + temp[SHF(+OFF, -OFF, c)]
                                + 2.f * temp[SHF(+OFF, 0, c)]
                                + temp[SHF(+OFF, +OFF, c)]) / 16.0f;

          // image - blur = laplacian = edges
          dd[c] = temp[SHF(0, 0, c)] - central_average[c];
        }

        // Compute the Minkowski p-norm for regularization
        const float p = 8.f;
        const float p_norm = powf(powf(fabsf(dd[0]), p)
                                  + powf(fabsf(dd[1]), p), 1.f / p) + NORM_MIN;

#pragma unroll
        for(size_t c = 0; c < 2; c++) xyY[c] -= dd[c] / p_norm;
        elements += 1.f;
      }
  }

  if(elements <= NORM_MIN || !isfinite(elements)
     || !isfinite(xyY[0]) || !isfinite(xyY[1]))
  {
    xyz[0] = xyz[1] = xyz[2] = NAN;
    return;
  }
  const float norm_D50 = dt_fast_hypotf(D50[0], D50[1]);
  for(size_t c = 0; c < 2; c++)
    xyz[c] = norm_D50 * (xyY[c] / elements) + D50[c];
  xyz[2] = 1.f;

#if defined(__GNUC__) && defined(_WIN32)
  #pragma GCC pop_options
#endif

}
/* end of the engine-owned detector */
void dt_channelmixerrgb_check_if_close_to_daylight(const float x,
                                        const float y,
                                        float *temperature,
                                        dt_illuminant_t *illuminant,
                                        dt_adaptation_t *adaptation)
{
  /* Check if a chromaticity x, y is close to daylight within 2.5 % error margin.
   * If so, we enable the daylight GUI for better ergonomics
   * Otherwise, we default to direct x, y control for better accuracy
   *
   * Note : The use of CCT is discouraged if dE > 5 % in CIE 1960 Yuv space
   *        reference : https://onlinelibrary.wiley.com/doi/abs/10.1002/9780470175637.ch3
   */

  // Get the correlated color temperature (CCT)
  float t = xy_to_CCT(x, y);

  // xy_to_CCT is valid only in 3000 - 25000 K. We need another model below
  if(t < 3000.f && t > 1667.f)
    t = CCT_reverse_lookup(x, y);

  if(temperature)
    *temperature = t;

  // Convert to CIE 1960 Yuv space
  const float xy_ref[2] = { x, y };
  float uv_ref[2];
  xy_to_uv(xy_ref, uv_ref);

  float xy_test[2] = { 0.f };
  float uv_test[2];

  // Compute the test chromaticity from the daylight model
  illuminant_to_xy(DT_ILLUMINANT_D, NULL, NULL, &xy_test[0], &xy_test[1], t,
                   DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);
  xy_to_uv(xy_test, uv_test);

  // Compute the error between the reference illuminant and the test
  // illuminant derivated from the CCT with daylight model
  const float delta_daylight = dt_fast_hypotf(uv_test[0] - uv_ref[0], uv_test[1] - uv_ref[1]);

  // Compute the test chromaticity from the blackbody model
  illuminant_to_xy(DT_ILLUMINANT_BB, NULL, NULL, &xy_test[0], &xy_test[1], t,
                   DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);
  xy_to_uv(xy_test, uv_test);

  // Compute the error between the reference illuminant and the test
  // illuminant derivated from the CCT with black body model
  const float delta_bb = dt_fast_hypotf(uv_test[0] - uv_ref[0], uv_test[1] - uv_ref[1]);

  // Check the error between original and test chromaticity
  if(delta_bb < 0.005f || delta_daylight < 0.005f)
  {
    if(illuminant)
    {
      if(delta_bb < delta_daylight)
        *illuminant = DT_ILLUMINANT_BB;
      else
        *illuminant = DT_ILLUMINANT_D;
    }
  }
  else
  {
    // error is too big to use a CCT-based model, we fall back to a
    // custom/freestyle chroma selection for the illuminant
    if(illuminant) *illuminant = DT_ILLUMINANT_CUSTOM;
  }

  // CAT16 is more accurate no matter the illuminant
  if(adaptation) *adaptation = DT_ADAPTATION_CAT16;
}

gboolean dt_iop_channelmixer_rgb_detect(const float *pixels, size_t width,
                                        size_t height, size_t channels,
                                        const dt_colormatrix_t RGB_to_XYZ,
                                        dt_illuminant_t requested,
                                        float *x, float *y, float *temperature,
                                        dt_illuminant_t *illuminant,
                                        dt_adaptation_t *adaptation)
{
  if(!pixels || !x || !y || !temperature || !illuminant || !adaptation
     || channels < 3 || width < 6 * OFF || height < 6 * OFF
     || (requested != DT_ILLUMINANT_DETECT_EDGES
         && requested != DT_ILLUMINANT_DETECT_SURFACES))
    return FALSE;

  dt_aligned_pixel_t xyz = { NAN, NAN, NAN, 0.f };
  /* The detector's scratch image is deliberately private to this call. */
  const size_t count = width * height * channels;
  float *scratch = dt_alloc_align_float(count);
  if(!scratch) return FALSE;
  dt_channelmixerrgb_auto_detect_wb(pixels, scratch, requested, width, height, channels,
                  RGB_to_XYZ, xyz);
  dt_free_align(scratch);
  if(!isfinite(xyz[0]) || !isfinite(xyz[1])
     || xyz[0] <= 0.f || xyz[1] <= 0.f || xyz[0] + xyz[1] >= 1.f)
    return FALSE;
  *x = xyz[0];
  *y = xyz[1];
  dt_channelmixerrgb_check_if_close_to_daylight(*x, *y, temperature, illuminant, adaptation);
  return isfinite(*temperature);
}


gboolean dt_iop_channelmixer_rgb_detect_dev(
    dt_develop_t *dev, dt_iop_module_t *module, dt_illuminant_t requested,
    float *x, float *y, float *temperature, dt_illuminant_t *illuminant,
    dt_adaptation_t *adaptation)
{
  if(!dev || !module) return FALSE;
  dt_mipmap_buffer_t mbuf = { 0 };
  dt_mipmap_cache_get(&mbuf, dev->image_storage.id, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  gboolean ok = mbuf.buf && mbuf.width >= 24 && mbuf.height >= 24;
  dt_dev_pixelpipe_t pipe;
  gboolean pipe_initialized = FALSE;
  dt_dev_pixelpipe_t *saved_pipe = dev->full.pipe;
  if(ok)
  {
    ok = dt_dev_pixelpipe_init_export(&pipe, mbuf.width, mbuf.height,
                                      IMAGEIO_FLOAT, FALSE);
    pipe_initialized = ok;
  }
  if(ok)
  {
    dev->full.pipe = &pipe;
    dt_ioppr_resync_modules_order(dev);
    dt_dev_pixelpipe_set_input(&pipe, dev, (float *)mbuf.buf,
                               mbuf.width, mbuf.height, mbuf.iscale);
    dt_dev_pixelpipe_create_nodes(&pipe, dev);
    dt_dev_pixelpipe_synch_all(&pipe, dev);
    GList *target = NULL;
    for(GList *nodes = pipe.nodes; nodes; nodes = g_list_next(nodes))
    {
      dt_dev_pixelpipe_iop_t *piece = nodes->data;
      if(piece->module == module)
      {
        target = nodes;
        break;
      }
    }
    if(!target)
      ok = FALSE;
    else
    {
      for(GList *nodes = target; nodes; nodes = g_list_next(nodes))
        ((dt_dev_pixelpipe_iop_t *)nodes->data)->enabled = FALSE;
      dt_dev_pixelpipe_get_dimensions(&pipe, dev, mbuf.width, mbuf.height,
                                      &pipe.processed_width, &pipe.processed_height);
      ok = !dt_dev_pixelpipe_process_no_gamma(&pipe, dev, 0, 0,
                                               pipe.processed_width,
                                               pipe.processed_height, 1.0f);
      const dt_iop_order_iccprofile_info_t *profile =
        dt_ioppr_get_pipe_current_profile_info(module, &pipe);
      dt_colormatrix_t matrix = { 0 };
      if(ok && profile) memcpy(matrix, profile->matrix_in, sizeof(matrix));
      ok = ok && profile && pipe.backbuf
        && dt_iop_channelmixer_rgb_detect((const float *)pipe.backbuf,
            pipe.backbuf_width, pipe.backbuf_height, 4, matrix, requested,
            x, y, temperature, illuminant, adaptation);
    }
  }
  dev->full.pipe = saved_pipe;
  if(pipe_initialized) dt_dev_pixelpipe_cleanup(&pipe);
  if(mbuf.buf) dt_mipmap_cache_release(&mbuf);
  return ok;
}

#define DEFLICKER_BINS_COUNT (UINT16_MAX + 1)

typedef struct dt_auto_exposure_params_t
{
  int mode;
  float black;
  float exposure;
  float deflicker_percentile;
  float deflicker_target_level;
  gboolean compensate_exposure_bias;
  gboolean compensate_hilite_pres;
} dt_auto_exposure_params_t;

static void _auto_exposure_prepare_histogram(dt_iop_module_t *self,
                                             uint32_t **histogram,
                                             dt_dev_histogram_stats_t *stats)
{
  const dt_image_t *img = dt_image_cache_get(self->dev->image_storage.id, 'r');
  if(!img) return;
  const dt_image_t image = *img;
  dt_image_cache_read_release(img);
  if(image.buf_dsc.channels != 1 || image.buf_dsc.datatype != TYPE_UINT16) return;

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, self->dev->image_storage.id, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf)
  {
    dt_mipmap_cache_release(&buf);
    return;
  }

  dt_dev_histogram_collection_params_t params = { 0 };
  dt_histogram_roi_t roi = {
    .width = image.width,
    .height = image.height,
    .crop_x = image.crop_x,
    .crop_y = image.crop_y,
    .crop_right = image.crop_right,
    .crop_bottom = image.crop_bottom
  };
  params.roi = &roi;
  params.bins_count = DEFLICKER_BINS_COUNT;
  dt_histogram_helper(&params, stats, IOP_CS_RAW, IOP_CS_NONE,
                      buf.buf, histogram, NULL, FALSE, NULL);
  dt_mipmap_cache_release(&buf);
}

static double _auto_exposure_raw_to_ev(uint32_t raw, uint32_t black_level,
                                       uint32_t white_level)
{
  const uint32_t raw_max = white_level - black_level;
  const int64_t raw_val = MAX((int64_t)raw - (int64_t)black_level, 1);
  return -log2(raw_max) + log2(raw_val);
}

gboolean dt_exposure_compute_deflicker(dt_iop_module_t *self, float *correction)
{
  if(!self || !self->dev || !correction
     || !dt_image_is_raw(&self->dev->image_storage)
     || self->dev->image_storage.buf_dsc.channels != 1
     || self->dev->image_storage.buf_dsc.datatype != TYPE_UINT16)
    return FALSE;

  const dt_auto_exposure_params_t *params = self->params;
  uint32_t *histogram = NULL;
  dt_dev_histogram_stats_t stats = { 0 };
  _auto_exposure_prepare_histogram(self, &histogram, &stats);
  if(!histogram || stats.bins_count == 0 || stats.pixels == 0) return FALSE;

  const double threshold = CLAMP(
      (double)stats.pixels * (double)params->deflicker_percentile / 100.0,
      1.0, (double)stats.pixels);
  size_t n = 0;
  uint32_t raw = 0;
  gboolean found = FALSE;
  for(size_t i = 0; i < stats.bins_count; i++)
  {
    n += histogram[i];
    if((double)n >= threshold)
    {
      raw = (uint32_t)i;
      found = TRUE;
      break;
    }
  }
  if(!found || self->dev->image_storage.raw_white_point
      <= self->dev->image_storage.raw_black_level)
  {
    dt_free_align(histogram);
    return FALSE;
  }

  const double ev = _auto_exposure_raw_to_ev(
      raw, self->dev->image_storage.raw_black_level,
      self->dev->image_storage.raw_white_point);
  const double value = (double)params->deflicker_target_level - ev;
  dt_free_align(histogram);
  if(!isfinite(value)) return FALSE;
  *correction = (float)value;
  return isfinite(*correction);
}

void dt_exposure_set_manual(dt_iop_module_t *self, float exposure)
{
  if(!self || !self->params) return;
  dt_auto_exposure_params_t *params = self->params;
  params->mode = 0;
  params->exposure = exposure;
}
