// SPDX-License-Identifier: Apache-2.0
#include "nyadsp.h"
#include "nyadsp_clock.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
  struct nyadsp_config_s config;
  struct nyadsp_s *dsp;
  float input[512], output[768], mono[264];
  int sign;
  size_t i;
  nyadsp_default_config(&config);
  config.master_gain_db = 0;
  dsp = nyadsp_create_stereo(&config, NULL);
  assert(dsp != NULL);
  for (i = 0; i < 512; i++) input[i] = 0.1f;
  assert(nyadsp_process(dsp, input, output, 256) == 0);
  for (i = 0; i < 256; i++) {
    assert(fabsf(output[3*i] - 0.1f) < 1e-6f);
    assert(fabsf(output[3*i+1] - 0.1f) < 1e-6f);
    assert(output[3*i+2] == 0.0f);
  }
  nyadsp_destroy(dsp);
  for (i = 0; i < 256; i++) output[3*i+2] = 0.2f;
  for (sign = -1; sign <= 1; sign += 2) {
    struct nyadsp_clock_s clock;
    double queued = 512;
    nyadsp_clock_init(&clock);
    for (i = 0; i < 12000; i++) {
      size_t n = nyadsp_clock_process(&clock, output, 256, mono, 264,
                                      (size_t)queued, 512);
      size_t j;
      assert(n >= 254 && n <= 258);
      for (j = 0; j < n; j++) assert(fabsf(mono[j] - 0.2f) < 1e-6f);
      queued += n - 256.0 * (1.0 + sign * 0.0015);
      assert(queued > 0 && queued < 1024);
    }
    assert(fabs(clock.ratio - (1.0 + sign * 0.0015)) < 0.00005);
    printf("CLOCK_DRIFT_PASS ppm=%d queue=%.2f ratio=%.8f\n", sign*1500, queued, clock.ratio);
  }
  puts("NY_DSP_STEREO_CLOCK_PASS");
  return 0;
}
