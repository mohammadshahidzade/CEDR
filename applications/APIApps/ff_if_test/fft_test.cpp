#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <unistd.h>
#include "dash.h"

#define SIZE 16
#define EPSILON 1e-5  // Acceptable numerical error

int main(void) {
  printf("[nk] Starting FFT + IFFT round-trip test\n");

  dash_re_flt_type* input = (dash_re_flt_type*) malloc(2 * SIZE * sizeof(dash_re_flt_type));
  dash_re_flt_type* fft_out = (dash_re_flt_type*) malloc(2 * SIZE * sizeof(dash_re_flt_type));
  dash_re_flt_type* ifft_out = (dash_re_flt_type*) malloc(2 * SIZE * sizeof(dash_re_flt_type));

  // Generate a real sine wave input: real = sin(2πf*t), imag = 0
  for (int i = 0; i < SIZE; i++) {
    input[2 * i] = sin(2 * M_PI * i / SIZE);  // real part
    input[2 * i + 1] = 0.0;                   // imag part
  }

  printf("[nk] Input signal prepared. Running FFT...\n");

  // FFT
  DASH_FFT_flt((dash_cmplx_flt_type*) input, (dash_cmplx_flt_type*) fft_out, SIZE, true);

  printf("[nk] FFT complete. Running IFFT...\n");
  // sleep(1);

  // IFFT
  DASH_FFT_flt((dash_cmplx_flt_type*) fft_out, (dash_cmplx_flt_type*) ifft_out, SIZE, false);

  printf("[nk] IFFT complete. Comparing results to original input...\n");

  // Normalize IFFT result (many implementations scale by N on IFFT)
  double max_error = 0.0;
  for (int i = 0; i < 2 * SIZE; i++) {
    // ifft_out[i] /= SIZE;

    double err = fabs(input[i] - ifft_out[i]);
    if (err > max_error) max_error = err;
  }

  if (max_error < EPSILON) {
    printf("[nk] ✅ PASS: IFFT output matches original input within tolerance (max error = %.8f)\n", max_error);
  } else {
    printf("[nk] ❌ FAIL: IFFT output differs from original input (max error = %.8f)\n", max_error);
    printf("Index | Input       | IFFT Output | Error\n");
    for (int i = 0; i < 2 * SIZE; i += 2) {
      printf("%5d | %10.6f %10.6f | %10.6f %10.6f | %10.6f %10.6f\n", i/2,
        input[i], input[i+1],
        ifft_out[i], ifft_out[i+1],
        fabs(input[i] - ifft_out[i]),
        fabs(input[i+1] - ifft_out[i+1]));
    }
  }

  free(input);
  free(fft_out);
  free(ifft_out);

  return 0;
}
