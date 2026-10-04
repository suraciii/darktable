#pragma once

#include "common/colorspaces_inline_conversions.h"
#include "common/dttypes.h"
#include "common/chromatic_adaptation.h"
#include "common/illuminants.h"

/* Engine-owned, GUI-independent illuminant detection.  The input is a full
 * image in the module's linear RGBA working space. */
gboolean dt_iop_channelmixer_rgb_detect(const float *pixels, size_t width,
                                        size_t height, size_t channels,
                                        const dt_colormatrix_t RGB_to_XYZ,
                                        dt_illuminant_t requested,
                                        float *x, float *y, float *temperature,
                                        dt_illuminant_t *illuminant,
                                        dt_adaptation_t *adaptation);
void dt_channelmixerrgb_auto_detect_wb(const float *const restrict in,
                                       float *const restrict temp,
                                       dt_illuminant_t illuminant,
                                       size_t width, size_t height, size_t channels,
                                       const dt_colormatrix_t RGB_to_XYZ,
                                       dt_aligned_pixel_t xyz);

void dt_channelmixerrgb_check_if_close_to_daylight(
    float x, float y, float *temperature, dt_illuminant_t *illuminant,
    dt_adaptation_t *adaptation);


struct dt_develop_t;
struct dt_iop_module_t;
gboolean dt_iop_channelmixer_rgb_detect_dev(
    struct dt_develop_t *dev, struct dt_iop_module_t *module,
    dt_illuminant_t requested, float *x, float *y, float *temperature,
    dt_illuminant_t *illuminant, dt_adaptation_t *adaptation);
