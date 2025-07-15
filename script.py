def sum_actual_exe_times(file_path):
    total_time = 0

    with open(file_path, 'r') as f:
        for line in f:
            # Find the substring starting with 'actual_exe_time:'
            parts = line.split(',')
            for part in parts:
                part = part.strip()
                if part.startswith("actual_exe_time:"):
                    time_str = part.split(":")[1].strip()
                    total_time += int(time_str)
                    break  # No need to look at other parts in this line

    print(f"Total actual_exe_time: {total_time}")

# Example usage
sum_actual_exe_times("results_only_FFT_fence")
