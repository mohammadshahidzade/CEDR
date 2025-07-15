#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <unistd.h>
#include "dash.h"

#define SIZE 16

int main(void) {
  printf("[nk] Starting simple FFT test (writing outputs to files)\n");

  dash_re_flt_type* input = (dash_re_flt_type*) malloc(2 * SIZE * sizeof(dash_re_flt_type));
  dash_re_flt_type* output = (dash_re_flt_type*) malloc(2 * SIZE * sizeof(dash_re_flt_type));
  bool forwardTrans = true;

  // Prepare a sine wave input: real = sin(2πf*t), imag = 0
  for (int i = 0; i < SIZE; i++) {
    input[2 * i] = sin(2 * M_PI * i / SIZE);  // real part
    input[2 * i + 1] = 0.0;                   // imag part
  }

  printf("[nk] Input signal prepared\n");

  for (int run = 0; run < 10; run++) {
    printf("[nk] Running FFT #%d\n", run);
    DASH_FFT_flt((dash_cmplx_flt_type*) input, (dash_cmplx_flt_type*) output, SIZE, forwardTrans);
    sleep(1);

    // Create filename like "fft_output_0.txt"
    char filename[64];
    snprintf(filename, sizeof(filename), "fft_output_%d.txt", run);

    FILE* fout = fopen(filename, "w");
    if (!fout) {
      perror("Error opening output file");
      free(input);
      free(output);
      return 1;
    }

    for (int i = 0; i < SIZE; i++) {
      fprintf(fout, "%f %f\n", output[2 * i], output[2 * i + 1]);
    }

    fclose(fout);
  }

  free(input);
  free(output);

  printf("[nk] FFT test complete. All outputs written to fft_output_*.txt files.\n");
  return 0;
}
