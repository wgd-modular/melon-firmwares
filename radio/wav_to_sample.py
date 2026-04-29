#!/usr/bin/env python3
"""
WAV to sample.h converter for Vocal Snippet Sampler
Converts a single WAV file to a C header file for the RP2040

Usage:
    python wav_to_sample.py input.wav [--target-rate 36600]

Features:
    - Converts to mono if stereo
    - Resamples to target sample rate (default: 36600 Hz)
    - Converts to 16-bit PCM format
    - Generates sample.h file with byte array
    - Shows estimated playback time and memory usage

Requirements:
    pip install numpy scipy

Maximum Length:
    - RP2040 Flash: 2MB total
    - Available for samples: ~1.5-1.8MB (rest for firmware)
    - At 36.6kHz, 16-bit: ~73KB per second
    - Maximum: ~130 seconds of audio
"""

import wave
import numpy as np
import sys
import argparse
from scipy import signal

def resample_audio(audio_data, orig_rate, target_rate):
    """Resample audio to target sample rate using high-quality resampling"""
    if orig_rate == target_rate:
        return audio_data
    
    # Calculate the number of samples in the resampled audio
    num_samples = int(len(audio_data) * target_rate / orig_rate)
    
    # Use scipy's resample for high-quality resampling
    resampled = signal.resample(audio_data, num_samples)
    
    return resampled

def normalize_audio(audio_data, target_peak=0.95):
    """Normalize audio to target peak level"""
    max_val = np.max(np.abs(audio_data))
    if max_val > 0:
        audio_data = audio_data * (target_peak / max_val)
    return audio_data

def convert_wav_to_header(input_wav, output_header="sample.h", target_rate=36600, normalize=True):
    """Convert WAV file to C header file"""
    
    print(f"Converting {input_wav} to {output_header}...")
    print(f"Target sample rate: {target_rate} Hz")
    
    # Open WAV file
    with wave.open(input_wav, 'rb') as wav:
        # Get WAV parameters
        n_channels = wav.getnchannels()
        sampwidth = wav.getsampwidth()
        framerate = wav.getframerate()
        n_frames = wav.getnframes()
        
        print(f"\nInput WAV info:")
        print(f"  Channels: {n_channels}")
        print(f"  Sample width: {sampwidth} bytes")
        print(f"  Frame rate: {framerate} Hz")
        print(f"  Duration: {n_frames / framerate:.2f} seconds")
        
        # Read audio data
        audio_bytes = wav.readframes(n_frames)
        
        # Convert to numpy array based on sample width
        if sampwidth == 1:
            audio_data = np.frombuffer(audio_bytes, dtype=np.uint8)
            audio_data = (audio_data.astype(np.float32) - 128) / 128.0
        elif sampwidth == 2:
            audio_data = np.frombuffer(audio_bytes, dtype=np.int16)
            audio_data = audio_data.astype(np.float32) / 32768.0
        elif sampwidth == 3:
            # 24-bit audio - need to unpack manually
            audio_data = np.frombuffer(audio_bytes, dtype=np.uint8)
            audio_data = audio_data.reshape(-1, 3)
            # Convert to int32
            audio_int = np.zeros(len(audio_data), dtype=np.int32)
            for i in range(len(audio_data)):
                audio_int[i] = (audio_data[i][0] | 
                               (audio_data[i][1] << 8) | 
                               (audio_data[i][2] << 16))
                # Sign extend
                if audio_int[i] & 0x800000:
                    audio_int[i] |= 0xFF000000
            audio_data = audio_int.astype(np.float32) / 8388608.0
        else:
            raise ValueError(f"Unsupported sample width: {sampwidth}")
        
        # Convert stereo to mono by averaging channels
        if n_channels == 2:
            audio_data = audio_data.reshape(-1, 2).mean(axis=1)
            print("  Converted stereo to mono")
        elif n_channels > 2:
            audio_data = audio_data.reshape(-1, n_channels).mean(axis=1)
            print(f"  Converted {n_channels} channels to mono")
    
    # Normalize audio
    if normalize:
        audio_data = normalize_audio(audio_data, target_peak=0.95)
        print("  Normalized audio to 95% peak")
    
    # Resample to target rate
    if framerate != target_rate:
        print(f"  Resampling from {framerate} Hz to {target_rate} Hz...")
        audio_data = resample_audio(audio_data, framerate, target_rate)
    
    # Convert to 16-bit PCM
    audio_int16 = np.clip(audio_data * 32767.0, -32768, 32767).astype(np.int16)
    
    # Convert to unsigned bytes for storage
    audio_bytes_out = audio_int16.tobytes()
    
    # Calculate sizes
    sample_length = len(audio_int16)
    byte_length = len(audio_bytes_out)
    duration = sample_length / target_rate
    
    print(f"\nOutput info:")
    print(f"  Sample length: {sample_length} samples")
    print(f"  Byte length: {byte_length} bytes ({byte_length / 1024:.1f} KB)")
    print(f"  Duration: {duration:.2f} seconds")
    print(f"  Memory usage: {byte_length / 1024 / 1024:.2f} MB")
    
    # Check if it fits in flash memory
    MAX_FLASH_BYTES = 1800000  # ~1.8MB available for samples
    if byte_length > MAX_FLASH_BYTES:
        print(f"\n⚠️  WARNING: Sample too large!")
        print(f"   Maximum size: {MAX_FLASH_BYTES / 1024 / 1024:.2f} MB")
        print(f"   Your sample: {byte_length / 1024 / 1024:.2f} MB")
        print(f"   Please use a shorter audio file or lower sample rate")
        sys.exit(1)
    
    # Write header file
    with open(output_header, 'w') as f:
        f.write("/*\n")
        f.write(" * Auto-generated sample.h file\n")
        f.write(f" * Source: {input_wav}\n")
        f.write(f" * Sample rate: {target_rate} Hz\n")
        f.write(f" * Duration: {duration:.2f} seconds\n")
        f.write(f" * Size: {byte_length} bytes ({byte_length / 1024:.1f} KB)\n")
        f.write(" */\n\n")
        f.write("#ifndef SAMPLE_H\n")
        f.write("#define SAMPLE_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define SAMPLE_LENGTH {sample_length}UL\n")
        f.write(f"#define SAMPLE_RATE {target_rate}UL\n\n")
        
        # Write sample data
        f.write("const uint8_t vocalSample[] PROGMEM = {\n")
        
        # Write bytes in rows of 16
        bytes_per_row = 16
        for i in range(0, len(audio_bytes_out), bytes_per_row):
            chunk = audio_bytes_out[i:i+bytes_per_row]
            hex_values = ', '.join(f'0x{b:02X}' for b in chunk)
            f.write(f"  {hex_values}")
            if i + bytes_per_row < len(audio_bytes_out):
                f.write(",")
            f.write("\n")
            
            # Progress indicator for large files
            if i % (bytes_per_row * 1000) == 0:
                progress = (i / len(audio_bytes_out)) * 100
                print(f"  Writing header: {progress:.1f}%", end='\r')
        
        f.write("};\n\n")
        f.write("#endif // SAMPLE_H\n")
    
    print(f"\n✓ Successfully created {output_header}")
    print(f"\nNext steps:")
    print(f"  1. Copy {output_header} to your Arduino project folder")
    print(f"  2. Compile and upload vocal_snippet.ino to your RP2040")
    print(f"  3. Use POT3 to select start position in the sample")
    print(f"  4. Use POT2 to switch between forward/reverse playback")
    print(f"  5. Use POT1 to control playback speed")

def main():
    parser = argparse.ArgumentParser(
        description='Convert WAV file to sample.h for Vocal Snippet Sampler',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic conversion
  python wav_to_sample.py podcast.wav
  
  # Custom target sample rate
  python wav_to_sample.py speech.wav --target-rate 32000
  
  # Custom output filename
  python wav_to_sample.py vocals.wav --output my_sample.h
  
  # Skip normalization
  python wav_to_sample.py audio.wav --no-normalize

Maximum Audio Length:
  At 36.6kHz, 16-bit: ~130 seconds maximum
  At 32kHz, 16-bit: ~140 seconds maximum
  At 22.05kHz, 16-bit: ~200 seconds maximum
        """
    )
    
    parser.add_argument('input', help='Input WAV file')
    parser.add_argument('--output', '-o', default='sample.h',
                       help='Output header file (default: sample.h)')
    parser.add_argument('--target-rate', '-r', type=int, default=36600,
                       help='Target sample rate in Hz (default: 36600)')
    parser.add_argument('--no-normalize', action='store_true',
                       help='Skip audio normalization')
    
    args = parser.parse_args()
    
    try:
        convert_wav_to_header(
            args.input,
            args.output,
            args.target_rate,
            normalize=not args.no_normalize
        )
    except FileNotFoundError:
        print(f"Error: File '{args.input}' not found")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
