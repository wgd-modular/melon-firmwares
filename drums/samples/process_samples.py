import wave
import struct
import os

def process_audio(filepath, target_sr=22050):
    if not os.path.exists(filepath): return None
    print(f"  Processing {os.path.basename(filepath)}...")
    
    if filepath.endswith(".raw"):
        try:
            with open(filepath, 'rb') as f:
                data = f.read()
                return list(struct.unpack(f"<{len(data)//2}h", data))
        except: return None

    try:
        with wave.open(filepath, 'rb') as w:
            sr = w.getframerate()
            n_channels = w.getnchannels()
            sampwidth = w.getsampwidth()
            n_frames = w.getnframes()
            data = w.readframes(n_frames)
            
            if sampwidth == 2:
                fmt = f"<{n_frames * n_channels}h"
                audio = list(struct.unpack(fmt, data))
            elif sampwidth == 1:
                fmt = f"<{n_frames * n_channels}B"
                audio = [(x - 128) * 256 for x in struct.unpack(fmt, data)]
            else:
                return None
                
            if n_channels > 1:
                audio = [sum(audio[i:i+n_channels]) // n_channels for i in range(0, len(audio), n_channels)]
                
            if sr != target_sr:
                ratio = sr / target_sr
                new_len = int(len(audio) / ratio)
                audio = [int(audio[int(i * ratio)]) for i in range(new_len)]
                
            return audio
    except:
        return None

def generate_header_array(name, audio, max_len=17640):
    if len(audio) > max_len: audio = audio[:max_len]
    length = len(audio)
    header = f"// {name}\n#define {name.upper()}_LEN {length}\n"
    header += f"const int16_t {name.lower()}[{length}] PROGMEM = {{\n  "
    lines = [", ".join(map(str, audio[i:i+12])) for i in range(0, len(audio), 12)]
    header += ",\n  ".join(lines)
    header += "\n};\n\n"
    return header

# Configuration: (Kit Name Prefix, Folder, [Kick, Snare, Hihat filenames])
kit_configs = [
    ("kick_808s", "snare_808s", "hihat_808s", "tr808", ["808_kick.wav", "808_snare.wav", "808_hihat.wav"]),
    ("tr909_k", "tr909_s", "tr909_h", "tr909", ["kick.wav", "snare.wav", "hihat.wav"]),
    ("tr606_k", "tr606_s", "tr606_h", "tr606", ["kick.wav", "snare.wav", "hihat.wav"]),
    ("linn_k", "linn_s", "linn_h", "linndrum", ["kick.wav", "snare.wav", "hihat.wav"]),
    ("simmons_k", "simmons_s", "simmons_h", "simmons_sds5", ["kick.wav", "snare.wav", "hihat.wav"]),
    ("kick_rock", "snare_rock", "hihat_rock", "rock", ["rock_kick.wav", "rock_snare.wav", "rock_hihat.wav"]),
    ("kick_grit", "snare_grit", "hihat_grit", "grit", ["lmms_kick4.wav", "nasty_snare.wav", "hihat.wav"]),
    ("user_k", "user_s", "user_h", "user", ["user_kick.wav", "user_snare.wav", "user_hihat.wav"])
]

base_dir = os.path.dirname(__file__)
output_file = os.path.join(base_dir, "..", "drum_samples.h")

print("Building Master Drum Header...")
with open(output_file, "w") as f:
    f.write("// Master Drum Samples Header\n#pragma once\n#include <stdint.h>\n\n#define SAMPLE_RATE 22050\n\n")
    
    for k_name, s_name, h_name, folder, files in kit_configs:
        print(f"Processing Kit: {folder}...")
        for i, name in enumerate([k_name, s_name, h_name]):
            path = os.path.join(base_dir, folder, files[i])
            # Fallback to .raw if .wav is missing
            if not os.path.exists(path):
                path = path.replace(".wav", ".raw")
            
            audio = process_audio(path)
            if audio:
                f.write(generate_header_array(name, audio))
            else:
                f.write(f"// {name} (File not found: {path})\n")
                f.write(f"#define {name.upper()}_LEN 1\nconst int16_t {name.lower()}[1] PROGMEM = {{0}};\n\n")

print(f"\nDone! Full header written to {output_file}")
