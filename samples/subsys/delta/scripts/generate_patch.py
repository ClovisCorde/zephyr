import argparse
import bsdiffhs
import logging
import os
import sys
 
# Configure logging
logging.basicConfig(filename='generate_patch.log', level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
 
def create_patch(src, dst, patch, window_sz2, lookahead_sz2, max_patch_size):
    """ generate a patch between the FW_source and the FW_target using bsdiff algorithm and heatshrink compression"""
    try:
        if bsdiffhs.file_diff(src, dst, patch, window_sz2, lookahead_sz2) is not None:
            logging.error("in the file_diff function from bsdiffhs")
            sys.exit(1)
    except Exception as e :
        logging.error(f"During patch generation: {e}")
        sys.exit(1)

    with open(patch, "rb") as f:
        original = f.read()

    header_16 = original[:16]        # first 16 bytes for magic and size of the new firmware
    rest = original[16:]             # all the patch after the header

    # Add the configuration used in the header
    extended = header_16 + bytes([window_sz2]) + bytes([lookahead_sz2]) + rest

    with open(patch, "wb") as f:
        f.write(extended)

    patch_size = len(extended)
    if patch_size > max_patch_size:
        logging.error("Patch is too big")
        sys.exit(1)  
 
 
 
def are_files_identical(file1, file2):
    """Compare if two files are identical."""
    CHUNK_SIZE = 65536  # 64 KB
 
    with open(file1, 'rb') as f1, open(file2, 'rb') as f2:
        while True:
            chunk1 = f1.read(CHUNK_SIZE)
            chunk2 = f2.read(CHUNK_SIZE)
 
            if chunk1 != chunk2:
                return False
 
            if not chunk1:  # end of files
                return True
 
 
def verify_patch(src, dst, new, patch, window_sz2, lookahead_sz2):
    """ Verify that the generated patch is"""
    try:
        if bsdiffhs.file_patch(src, new, patch, window_sz2, lookahead_sz2) is not None:
            logging.error("Failure when trying to rebuild the target firmware using source firmware and the patch")
            sys.exit(1)
        if not are_files_identical(dst, new):
            logging.error(f"Files '{dst}' and '{new}' are not identical after patching.")
            sys.exit(1)
    except Exception as e:
        logging.error(f"During patch verification: {e}")
        sys.exit(1)
   
    logging.info(f"Patch '{patch}' generated ok (size = {os.path.getsize(patch)})")
 
 
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Benchmark heatshrink configurations.')
    parser.add_argument('FW_source', type=str, help='The source firmware file.')
    parser.add_argument('FW_target', type=str, help='The target firmware file.')
    parser.add_argument('patch_file', type=str, help='The output patch file.')
    parser.add_argument('new_file', type=str, help='The new firmware to be created (should be the same as FW_target).')
    parser.add_argument('window_sz2', type=int, help='The window_sz2 configuration for heatshrink configuration.')
    parser.add_argument('lookahed_sz2', type=int, help='The lookahead_sz2 configuration for heatshrink configuration.')
    parser.add_argument('max_size_patch', type=int, help='The maximum size for the patch (in bytes)')
   
    args = parser.parse_args()
 
    # Log the initial arguments for future reference
    logging.info(f"Script executed with arguments: Source FW: '{args.FW_source}', Target FW: '{args.FW_target}'")
 
    create_patch(args.FW_source, args.FW_target, args.patch_file, args.window_sz2, args.lookahed_sz2, args.max_size_patch)
    verify_patch(args.FW_source, args.FW_target, args.new_file, args.patch_file, args.window_sz2, args.lookahed_sz2)