#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the FSR 3.1 upscaler's shader permutation headers for Linux (issue #139).

AMD's FidelityFX SDK (third_party/fidelityfx-sdk, MIT) compiles its shaders with FidelityFX_SC, a Windows-only tool
that writes one header per pass and variant: the SPIR-V of every permutation, the resources each binds, and an
indirection table from the permutation key to its entry. The SDK's own code (blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp)
includes those headers by name. This writes the same headers with glslangValidator and a small SPIR-V reader, so the
SDK's code builds unchanged.

Only the permutations the client asks for are compiled (gfx/fsr.cpp: LDR colour, render-size motion vectors without
jitter, inverted depth); the key's other options map to the nearest compiled entry and fsr.cpp never sets them.

    fsr_shaders.py SDK_DIR OUT_DIR [--glslang PATH] [--jobs N]
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys

PASSES = ["autogen_reactive", "accumulate", "luma_pyramid", "prepare_reactivity", "prepare_inputs", "shading_change",
          "rcas", "shading_change_pyramid", "luma_instability", "debug_view"]
# The key's options, in the order of their bits (the generated union's bit fields; any order works, since the SDK
# sets them by name).
OPTIONS = ["FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE", "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT",
           "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS", "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS",
           "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH", "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING"]
# What the client compiles for each option: the values gfx/fsr.cpp can ask for.
COMPILED = {
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE": (0, 1),  # 1 on GPUs with wave32 and wave64 (AMD)
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT": (0,),
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS": (1,),
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS": (0,),
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH": (1,),
    "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING": (0, 1),
}
# CMakeCompileFSR3UpscalerShaders.txt's fixed defines, and the Vulkan back end's (CMakeShadersFSR3Upscaler.txt).
BASE_DEFINES = ["FFX_GPU=1", "FFX_GLSL=1",
                "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
                "FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
                "FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
                "FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
                "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2"]
# The variants the SDK's accessor includes. On Vulkan the SDK never forces wave64 (its shader model stays 5.1), so the
# wave64 headers repeat the plain ones; the 16-bit ones are compiled with FFX_HALF.
VARIANTS = {"": (False,), "_wave64": (False,), "_16bit": (True,), "_wave64_16bit": (True,)}

# SPIR-V
OP_NAME, OP_DECORATE, OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY = 5, 71, 25, 26, 28, 29
OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_CONSTANT, OP_VARIABLE = 30, 32, 43, 59
DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_NON_WRITABLE, DEC_BINDING, DEC_SET = 2, 3, 24, 33, 34
SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER = 0, 2, 12


def reflect(spirv):
    """The resources a SPIR-V module binds: {kind: [(name, binding, count, set)]}, kind one of cb, srv_tex, uav_tex,
    srv_buf, uav_buf (samplers are left out: the SDK's Vulkan back end creates its static samplers itself)."""
    words = struct.unpack(f"<{len(spirv) // 4}I", spirv)
    names, decos, types, consts, variables = {}, {}, {}, {}, []
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        args = words[i + 1:i + count]
        if op == OP_NAME:
            raw = struct.pack(f"<{len(args) - 1}I", *args[1:])
            names[args[0]] = raw.split(b"\0", 1)[0].decode()
        elif op == OP_DECORATE:
            decos.setdefault(args[0], {})[args[1]] = args[2] if len(args) > 2 else True
        elif op in (OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER):
            types[args[0]] = (op, args[1:])
        elif op == OP_CONSTANT:
            consts[args[1]] = args[2] if len(args) > 2 else 0
        elif op == OP_VARIABLE:
            variables.append((args[0], args[1], args[2]))
        i += count
    out = {"cb": [], "srv_tex": [], "uav_tex": [], "srv_buf": [], "uav_buf": []}
    for ptr_type, var, storage in variables:
        if storage not in (SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER):
            continue
        d = decos.get(var, {})
        if DEC_BINDING not in d:
            continue
        t = types[ptr_type][1][1]  # the pointee
        n = 1
        while types.get(t, (None,))[0] in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
            op, a = types[t]
            n *= consts.get(a[1], 1) if op == OP_TYPE_ARRAY else 1
            t = a[0]
        op, a = types.get(t, (None, ()))
        entry = (names.get(var, f"_{var}"), d[DEC_BINDING], n, d.get(DEC_SET, 0))
        if op == OP_TYPE_IMAGE:
            out["uav_tex" if a[5] == 2 else "srv_tex"].append(entry)  # Sampled: 1 = sampled image, 2 = storage
        elif op == OP_TYPE_STRUCT:
            td = decos.get(t, {})
            if storage == SC_UNIFORM and DEC_BLOCK in td:
                out["cb"].append(entry)
            else:  # a storage buffer (StorageBuffer + Block, or Uniform + BufferBlock)
                ro = DEC_NON_WRITABLE in d
                out["srv_buf" if ro else "uav_buf"].append(entry)
    for v in out.values():
        v.sort(key=lambda e: e[1])
    return out


def key_index(values):
    return sum(v << b for b, v in enumerate(values))


def permutations():
    """Every compiled option tuple, in OPTIONS order."""
    combos = [()]
    for o in OPTIONS:
        combos = [c + (v,) for c in combos for v in COMPILED[o]]
    return combos


def nearest(values, compiled):
    """The compiled tuple for a key the client never asks for: each option's value if compiled, else the first."""
    pick = tuple(v if v in COMPILED[o] else COMPILED[o][0] for o, v in zip(OPTIONS, values))
    return compiled.index(pick)


def compile_one(glslang, sdk, src, defines, half):
    cmd = [glslang, "-V", "--target-env", "vulkan1.2", "-S", "comp", "-Os", "-o", "/dev/stdout",
           "-I" + os.path.join(sdk, "include", "FidelityFX", "gpu"),
           "-I" + os.path.join(sdk, "include", "FidelityFX", "gpu", "fsr3upscaler")]
    cmd += ["-D" + d for d in BASE_DEFINES + defines + (["FFX_HALF=1"] if half else [])]
    r = subprocess.run(cmd + [src], capture_output=True)
    if r.returncode != 0:
        sys.exit(f"fsr_shaders: glslangValidator failed on {os.path.basename(src)} ({' '.join(defines)}):\n"
                 f"{r.stdout.decode(errors='replace')[-3000:]}")
    # -o /dev/stdout: the module is all of stdout except glslang's own file-name line in front.
    data = r.stdout
    magic = data.find(b"\x03\x02\x23\x07")
    return data[magic:]


def c_array(name, items, ctype):
    if not items:
        return f"static const {ctype}* const {name} = 0;\n"
    return f"static const {ctype} {name}[] = {{ {', '.join(items)} }};\n"


def write_header(path, pass_name, variant, blobs):
    stem = f"g_ffx_fsr3upscaler_{pass_name}_pass{variant}"
    key = f"ffx_fsr3upscaler_{pass_name}_pass_PermutationKey"
    lines = [f"// Generated by tools/fsr_shaders.py (issue #139): FSR 3.1 upscaler {pass_name} pass{variant}.",
             "#pragma once", "#include <stdint.h>", '#include "ffx_fsr3upscaler_permutation_info.h"', "",
             f"#ifndef {key.upper()}_DEFINED", f"#define {key.upper()}_DEFINED", f"typedef union {key}", "{",
             "    struct", "    {"]
    lines += [f"        uint32_t {o} : 1;" for o in OPTIONS]
    lines += ["    };", "    uint32_t index;", f"}} {key};", "#endif", ""]
    infos = []
    for n, (_values, spirv, res) in enumerate(blobs):
        p = f"{stem}_{n}"
        words = ", ".join(f"0x{b:02x}" for b in spirv)
        lines.append(f"static const unsigned char {p}_data[] = {{ {words} }};")
        fields = {}
        for kind in ("cb", "srv_tex", "uav_tex", "srv_buf", "uav_buf"):
            ents = res[kind]
            lines.append(c_array(f"{p}_{kind}_names", [f'"{e[0]}"' for e in ents], "char*").rstrip())
            lines.append(c_array(f"{p}_{kind}_bindings", [str(e[1]) for e in ents], "uint32_t").rstrip())
            lines.append(c_array(f"{p}_{kind}_counts", [str(e[2]) for e in ents], "uint32_t").rstrip())
            lines.append(c_array(f"{p}_{kind}_spaces", [str(e[3]) for e in ents], "uint32_t").rstrip())
            fields[kind] = (len(ents), f"{p}_{kind}")
        infos.append((p, len(spirv), fields))
    lines.append(f"static const FfxXiPermutationInfo {stem}_PermutationInfo[] = {{")
    def grp(fields, kind):
        base = fields[kind][1]
        return f"(const char**){base}_names, {base}_bindings, {base}_counts, {base}_spaces"

    for p, size, f in infos:
        lines.append(f"    {{ {p}_data, {size}, {f['cb'][0]}, {f['srv_tex'][0]}, {f['uav_tex'][0]}, {f['srv_buf'][0]}, "
                     f"{f['uav_buf'][0]}, 0, 0, {grp(f, 'cb')}, {grp(f, 'srv_tex')}, {grp(f, 'uav_tex')}, "
                     f"{grp(f, 'srv_buf')}, {grp(f, 'uav_buf')}, 0, 0, 0, 0, 0, 0, 0, 0 }},")
    lines.append("};")
    compiled = [b[0] for b in blobs]
    table = []
    for idx in range(1 << len(OPTIONS)):
        values = tuple((idx >> b) & 1 for b in range(len(OPTIONS)))
        table.append(str(nearest(values, compiled)))
    lines.append(f"static const int32_t {stem}_IndirectionTable[] = {{ {', '.join(table)} }};")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


INFO_HEADER = """// Generated by tools/fsr_shaders.py (issue #139): the entry type of the permutation tables (the fields
// POPULATE_SHADER_BLOB_FFX in ffx_types.h reads).
#pragma once
#include <stdint.h>
typedef struct FfxXiPermutationInfo
{
    const unsigned char* blobData;
    uint32_t             blobSize;
    uint32_t numConstantBuffers, numSRVTextures, numUAVTextures, numSRVBuffers, numUAVBuffers, numSamplers,
        numRTAccelerationStructures;
    const char** constantBufferNames;
    const uint32_t *constantBufferBindings, *constantBufferCounts, *constantBufferSpaces;
    const char** srvTextureNames;
    const uint32_t *srvTextureBindings, *srvTextureCounts, *srvTextureSpaces;
    const char** uavTextureNames;
    const uint32_t *uavTextureBindings, *uavTextureCounts, *uavTextureSpaces;
    const char** srvBufferNames;
    const uint32_t *srvBufferBindings, *srvBufferCounts, *srvBufferSpaces;
    const char** uavBufferNames;
    const uint32_t *uavBufferBindings, *uavBufferCounts, *uavBufferSpaces;
    const char** samplerNames;
    const uint32_t *samplerBindings, *samplerCounts, *samplerSpaces;
    const char** rtAccelerationStructureNames;
    const uint32_t *rtAccelerationStructureBindings, *rtAccelerationStructureCounts, *rtAccelerationStructureSpaces;
} FfxXiPermutationInfo;
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sdk", help="the SDK's sdk/ folder (third_party/fidelityfx-sdk/sdk)")
    ap.add_argument("out", help="where the headers go")
    ap.add_argument("--glslang", default="glslangValidator")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("XI_JOBS") or 0) or max(1, (os.cpu_count() or 3) - 2))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    shaders = os.path.join(a.sdk, "src", "backends", "vk", "shaders", "fsr3upscaler")
    combos = permutations()
    work = {}
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for p in PASSES:
            src = os.path.join(shaders, f"ffx_fsr3upscaler_{p}_pass.glsl")
            for half in (False, True):
                for values in combos:
                    defines = [f"{o}={v}" for o, v in zip(OPTIONS, values)]
                    work[(p, half, values)] = pool.submit(compile_one, a.glslang, a.sdk, src, defines, half)
        spirv = {k: f.result() for k, f in work.items()}
    with open(os.path.join(a.out, "ffx_fsr3upscaler_permutation_info.h"), "w") as f:
        f.write(INFO_HEADER)
    for p in PASSES:
        for variant, (half,) in VARIANTS.items():
            blobs = [(values, spirv[(p, half, values)], reflect(spirv[(p, half, values)])) for values in combos]
            write_header(os.path.join(a.out, f"ffx_fsr3upscaler_{p}_pass{variant}_permutations.h"), p, variant, blobs)
    print(f"fsr_shaders: {len(PASSES) * len(combos) * 2} modules into {a.out}")


if __name__ == "__main__":
    main()
