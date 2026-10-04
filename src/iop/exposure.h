/*
    Native exposure helpers used by the headless MCP bridge.
*/
#pragma once

#include "develop/imageop.h"

gboolean dt_iop_exposure_compute_deflicker(dt_iop_module_t *self, float *correction);
void dt_iop_exposure_set_manual(dt_iop_module_t *self, float exposure);

gboolean dt_exposure_compute_deflicker(dt_iop_module_t *self, float *correction);
void dt_exposure_set_manual(dt_iop_module_t *self, float exposure);
