import argparse
import bsdiffhs
from colored import fg
import os
import sys
 
RESULTS = 5 # Define the number of best results to display
 
# Heatshrink min and max configurations (default range)
CONFIG_MIN_LOOKAHEAD_SZ2 = 3
CONFIG_MIN_WINDOW_SZ2 = CONFIG_MIN_LOOKAHEAD_SZ2 + 1
CONFIG_MAX_WINDOW_SZ2 = 15
 
def print_table_header(header_format, src, dst, dst_size):
    """Display some information on the benchmark."""
    src_size = os.path.getsize(src) # Size of FW_source
    print("*" * len(header_format))
    print("** Test all the heatshrink configurations to create a patch between :   **")
    print(f"**   - source_FW = {src} ({str(src_size):<8} bytes) **")
    print(f"**   - target_FW = {dst} ({str(dst_size):<8} bytes) **")
    print("*" * len(header_format))
    print('-' * len(header_format))
    print(header_format)
    print_line(header_format)
 
def print_line(header):
    print('|' + '-' * (len(header) - 2) + '|')
 
def benchmark(src, dst, patch_path, debug=True, max_window_sz2=None):
    """
    Test all possible heatshrink configuration combinations.

    If debug=True : display all configurations.
    Si debug=False : doesn't print, return (best_window, best_lookahead).
    """
    patch_results = []
    dst_size = os.path.getsize(dst)  # Size of FW_target

    if max_window_sz2 is None:
        max_window = CONFIG_MAX_WINDOW_SZ2
    else:
        max_window = max_window_sz2

    header_format = f"| {'window_sz2':<15} | {'lookahead_sz2':<15} | {'Patch Size':<15} | {'Patch/FW_Target %':<15}|"

    if debug:
        print_table_header(header_format, src, dst, dst_size)

    for window in range(CONFIG_MIN_WINDOW_SZ2, max_window + 1):
        for lookahead in range(CONFIG_MIN_LOOKAHEAD_SZ2, window):
            try:
                bsdiffhs.file_diff(src, dst, patch_path, window, lookahead)
                patch_size = os.path.getsize(patch_path)
                patch_percentage = (patch_size / dst_size) * 100.0
            except Exception as e:
                print(f"Error while generating the patch: {e}")
                sys.exit(1)

            if patch_percentage < 100.0:
                patch_results.append(
                    (window, lookahead, patch_size, patch_percentage)
                )
                if debug:
                    print(f"| {window:<15} | {lookahead:<15} | {patch_size:<15} | "
                          f"{patch_percentage:>6.2f}%{'':<8}|")
            else:
                # The patch is bigger than the FW_target (DFOTA useless in this configuration)
                if debug:
                    print(f"|{fg('red')} {window:<15} | {lookahead:<15} | {patch_size:<15} | "
                          f"{'Patch too big':<15}  |")

        if debug:
            print_line(header_format)

    if not patch_results:
        # Aucune config utile trouvée
        if debug:
            print(f"{fg('red')}No valid heatshrink configuration produced a smaller patch than FW_target.")
        return None

    # Sort results by percentage of patch size, from smallest to largest
    sorted_patch_results = sorted(patch_results, key=lambda x: x[3])

    if debug:
        print(f"\n{fg('yellow')}These are the {RESULTS} best configurations : ")
        for result in sorted_patch_results[:RESULTS]:
            window, lookahead, patch_size, patch_percentage = result
            print(f"   - window_sz2: {window}, lookahead_sz2: {lookahead} = "
                  f"Patch size: {patch_size} bytes and Patch/FW_Target size percentage: {patch_percentage:.2f}%")

        # Display the configuration to generate the smallest patch
        best_patch_row_data = sorted_patch_results[0]
        window, lookahead, patch_size, patch_percentage = best_patch_row_data
        print(f"{fg('green')}Best result -> window_sz2: {window}, lookahead_sz2: {lookahead} = "
              f"Patch size: {patch_size} bytes and Patch/FW_Target size percentage: {patch_percentage:.2f}%")

    # Return the best config
    best = sorted_patch_results[0]
    best_window, best_lookahead, _, _ = best
    return best_window, best_lookahead


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Benchmark heatshrink configurations.')
    parser.add_argument('FW_source', type=str, help='The source firmware file.')
    parser.add_argument('FW_target', type=str, help='The target firmware file.')
    parser.add_argument('patch_file', type=str, help='The output patch file.')

    parser.add_argument(
        '--debug', '-d',
        action='store_true',
        help='Enable verbose output (table) in the shell.'
    )

    parser.add_argument(
        '--max-window-sz2',
        type=int,
        default=CONFIG_MAX_WINDOW_SZ2,
        help='Maximum window_sz2 to consider (to respect RAM constraints of the board).'
    )

    args = parser.parse_args()

    result = benchmark(
        args.FW_source,
        args.FW_target,
        args.patch_file,
        debug=args.debug,
        max_window_sz2=args.max_window_sz2,
    )

    # If not in debug mode, we display the best result
    if not args.debug and result is not None:
        w, l = result
        print(f"{w} {l}")
