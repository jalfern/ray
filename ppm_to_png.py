#!/usr/bin/env python3
"""Convert PPM P6 file to PNG using pure Python (no external dependencies)."""

import struct
import math

def zlib_crc32(data):
    """Compute CRC32 checksum."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
    return crc ^ 0xFFFFFFFF

def deflate_simple(data):
    """Simple deflate compression (naive implementation for small files)."""
    # For simplicity, we'll use a very basic RLE-like compression
    # This is not optimal but works for small images
    import zlib
    return zlib.compress(data, 9)

def read_ppm(filename):
    """Read P6 PPM file."""
    with open(filename, 'rb') as f:
        # Read magic number
        magic = f.readline().decode().strip()
        assert magic == 'P6', f"Expected P6, got {magic}"
        
        # Skip comments
        line = f.readline().decode().strip()
        while line.startswith('#'):
            line = f.readline().decode().strip()
        
        # Read dimensions
        width, height = map(int, line.split())
        
        # Read max value
        maxval = int(f.readline().decode().strip())
        assert maxval == 255, f"Expected 255, got {maxval}"
        
        # Read pixel data
        data = f.read()
        assert len(data) == width * height * 3, f"Wrong data size: {len(data)} vs {width*height*3}"
        
        return width, height, data

def create_png(width, height, pixel_data):
    """Create PNG from PPM data."""
    # PNG signature
    png_sig = b'\x89PNG\r\n\x1a\n'
    
    # IHDR chunk
    ihdr_data = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    ihdr_crc = zlib_crc32(b'IHDR' + ihdr_data)
    ihdr_chunk = struct.pack('>I', 13) + b'IHDR' + ihdr_data + struct.pack('>I', ihdr_crc)
    
    # Compress pixel data (add filter byte per row)
    compressed_data = bytearray()
    for y in range(height):
        compressed_data.append(0)  # Filter type: None
        compressed_data.extend(pixel_data[y * width * 3:(y + 1) * width * 3])
    
    compressed = deflate_simple(bytes(compressed_data))
    
    # IDAT chunk
    idat_crc = zlib_crc32(b'IDAT' + compressed)
    idat_chunk = struct.pack('>I', len(compressed)) + b'IDAT' + compressed + struct.pack('>I', idat_crc)
    
    # IEND chunk
    iend_crc = zlib_crc32(b'IEND')
    iend_chunk = struct.pack('>I', 0) + b'IEND' + struct.pack('>I', iend_crc)
    
    return png_sig + ihdr_chunk + idat_chunk + iend_chunk

def main():
    width, height, data = read_ppm('output.ppm')  # Assumes running from /Users/jon/Dev/ray
    png_data = create_png(width, height, data)
    
    with open('output.png', 'wb') as f:
        f.write(png_data)
    
    print(f"Created output.png ({width}x{height})")

if __name__ == '__main__':
    main()
