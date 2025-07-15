#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <unistd.h>
#include "dash.h"

#define SIZE 16

int main(void) {
  printf("[nk] Starting simple IFFT test (writing outputs to files)\n");

  dash_re_flt_type* input = (dash_re_flt_type*) malloc(2 * SIZE * sizeof(dash_re_flt_type));
  dash_re_flt_type* output = (dash_re_flt_type*) malloc(2 * SIZE * sizeof(dash_re_flt_type));
  bool forwardTrans = false;  // Set to false for IFFT

  // Prepare a sine wave input in frequency domain
  // For demonstration: only one bin has energy (e.g., bin 1)
  for (int i = 0; i < SIZE; i++) {
    input[2 * i] = 0.0;      // real
    input[2 * i + 1] = 0.0;  // imag
  }
  input[2 * 1] = 8.0;        // Example: impulse in freq domain (real part)
  input[2 * 1 + 1] = 0.0;

  printf("[nk] Input frequency-domain signal prepared for IFFT\n");

  for (int run = 0; run < 10; run++) {
    printf("[nk] Running IFFT #%d\n", run);
    DASH_FFT_flt((dash_cmplx_flt_type*) input, (dash_cmplx_flt_type*) output, SIZE, forwardTrans);
    sleep(1);

    // Create filename like "ifft_output_0.txt"
    char filename[64];
    snprintf(filename, sizeof(filename), "ifft_output_%d.txt", run);

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

  printf("[nk] IFFT test complete. All outputs written to ifft_output_*.txt files.\n");
  return 0;
}
