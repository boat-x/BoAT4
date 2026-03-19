#!/usr/bin/env python3
"""
abi2c.py — ABI-to-C codec generator for BoAT v4

Reads a Solidity ABI JSON file and generates C encode/decode functions
that call boat_evm_abi_encode_* / boat_evm_abi_decode_* primitives.

Output is pure codec — no I/O, no wallet, no RPC calls.

When --output foo.c is given, also writes foo.h with declarations.
When no --output, prints both .h and .c to stdout separated by a marker.

Supports static types (address, bool, uintN, bytes32) and dynamic types
(bytes, string).

Usage:
    python3 abi2c.py <abi.json> [--output <output.c>]
"""

import json
import sys
import argparse
from pathlib import Path

# Solidity type → C encode/decode mapping
# For static types, encode produces a 32-byte slot.
# For dynamic types, encode appends to a BoatBuf tail.
TYPE_MAP = {
    "uint256":  {"c_type": "uint8_t",  "c_arg": "const uint8_t {name}[32]",
                 "encode": "boat_evm_abi_encode_uint256({name}, slot)",
                 "decode": "boat_evm_abi_decode_uint256(data, {offset}, {name})",
                 "dynamic": False},
    "uint64":   {"c_type": "uint64_t", "c_arg": "uint64_t {name}",
                 "encode": "boat_evm_abi_encode_uint64({name}, slot)",
                 "decode": "boat_evm_abi_decode_uint256(data, {offset}, tmp); /* extract u64 from u256 */",
                 "dynamic": False},
    "address":  {"c_type": "uint8_t",  "c_arg": "const uint8_t {name}[20]",
                 "encode": "boat_evm_abi_encode_address({name}, slot)",
                 "decode": "boat_evm_abi_decode_address(data, {offset}, {name})",
                 "dynamic": False},
    "bool":     {"c_type": "bool",     "c_arg": "bool {name}",
                 "encode": "boat_evm_abi_encode_bool({name}, slot)",
                 "decode": "boat_evm_abi_decode_bool(data, {offset}, &{name})",
                 "decode_out": "boat_evm_abi_decode_bool(data, {offset}, {name})",
                 "dynamic": False},
    "bytes":    {"c_type": "uint8_t",  "c_arg": "const uint8_t *{name}, size_t {name}_len",
                 "encode": "boat_evm_abi_encode_bytes({name}, {name}_len, &tail)",
                 "decode": "boat_evm_abi_decode_bytes(data, data_len, {offset}, &{name}, &{name}_len)",
                 "dynamic": True},
    "string":   {"c_type": "char",     "c_arg": "const char *{name}",
                 "encode": "boat_evm_abi_encode_string({name}, &tail)",
                 "decode": "boat_evm_abi_decode_bytes(data, data_len, {offset}, (uint8_t **)&{name}, &{name}_len)",
                 "dynamic": True},
}

def sol_type_to_c(sol_type):
    """Map Solidity type to C type info."""
    if sol_type in TYPE_MAP:
        return TYPE_MAP[sol_type]
    if sol_type.startswith("uint"):
        return TYPE_MAP["uint256"]  # treat all uintN as uint256 slot
    if sol_type == "bytes32":
        return TYPE_MAP["uint256"]  # same 32-byte encoding
    return None

def is_dynamic(sol_type):
    """Check if a Solidity type is dynamic (requires offset+tail encoding)."""
    info = sol_type_to_c(sol_type)
    return info is not None and info.get("dynamic", False)

def func_signature(name, input_types):
    """Build Solidity function signature string."""
    return f"{name}({','.join(input_types)})"

def has_dynamic_args(inputs):
    """Check if any input is a dynamic type."""
    return any(is_dynamic(inp["type"]) for inp in inputs)

def encoder_params(inputs):
    """Build C parameter list for an encoder function."""
    params = []
    for inp in inputs:
        info = sol_type_to_c(inp["type"])
        if not info:
            params.append(f"const uint8_t *{inp['name']}, size_t {inp['name']}_len /* {inp['type']} unsupported, raw */")
        else:
            params.append(info["c_arg"].format(name=inp["name"]))
    params.append("uint8_t **calldata_out")
    params.append("size_t *calldata_len_out")
    return params

def generate_encoder_static(func):
    """Generate encoder for a function with only static args (uses boat_evm_abi_encode_func)."""
    name = func["name"]
    inputs = func.get("inputs", [])
    input_types = [inp["type"] for inp in inputs]
    sig = func_signature(name, input_types)
    params = encoder_params(inputs)

    lines = []
    lines.append(f"/* Encode {sig} */")
    lines.append(f"BoatResult encode_{name}({', '.join(params)})")
    lines.append("{")
    lines.append(f'    const char *func_sig = "{sig}";')
    lines.append(f"    const size_t n_args = {len(inputs)};")

    if inputs:
        n = len(inputs)
        lines.append(f"    uint8_t slots[{n}][32];")
        lines.append(f"    const uint8_t *args[{n}];")
        lines.append(f"    size_t arg_lens[{n}];")
        lines.append("")

        for i, inp in enumerate(inputs):
            info = sol_type_to_c(inp["type"])
            if info:
                encode_call = info["encode"].format(name=inp["name"])
                encode_call = encode_call.replace("slot)", f"slots[{i}])")
                lines.append(f"    {encode_call};")
            else:
                lines.append(f"    memcpy(slots[{i}], {inp['name']}, 32); /* raw */")
            lines.append(f"    args[{i}] = slots[{i}];")
            lines.append(f"    arg_lens[{i}] = 32;")

        lines.append("")
        lines.append("    return boat_evm_abi_encode_func(func_sig, args, arg_lens, n_args, calldata_out, calldata_len_out);")
    else:
        lines.append("    return boat_evm_abi_encode_func(func_sig, NULL, NULL, 0, calldata_out, calldata_len_out);")

    lines.append("}")
    return "\n".join(lines)

def generate_encoder_dynamic(func):
    """Generate encoder for a function with dynamic args (manual calldata assembly)."""
    name = func["name"]
    inputs = func.get("inputs", [])
    input_types = [inp["type"] for inp in inputs]
    sig = func_signature(name, input_types)
    params = encoder_params(inputs)
    n = len(inputs)

    lines = []
    lines.append(f"/* Encode {sig} */")
    lines.append(f"BoatResult encode_{name}({', '.join(params)})")
    lines.append("{")
    lines.append(f'    const char *func_sig = "{sig}";')
    lines.append(f"    uint8_t sig_hash[32];")
    lines.append(f"    keccak_256((const uint8_t *)func_sig, strlen(func_sig), sig_hash);")
    lines.append("")
    lines.append(f"    const size_t n_args = {n};")
    lines.append(f"    const size_t head_size = n_args * 32;")
    lines.append(f"    uint8_t head[{n}][32];")
    lines.append("")
    lines.append("    /* Tail buffer for dynamic args */")
    lines.append("    BoatBuf tail;")
    lines.append("    boat_buf_init(&tail, 256);")
    lines.append("")

    for i, inp in enumerate(inputs):
        info = sol_type_to_c(inp["type"])
        if not info:
            lines.append(f"    /* Arg {i}: {inp['type']} (unsupported, raw 32 bytes) */")
            lines.append(f"    memcpy(head[{i}], {inp['name']}, 32);")
        elif info.get("dynamic"):
            lines.append(f"    /* Arg {i}: {inp['type']} (dynamic) — offset pointer in head, data in tail */")
            lines.append(f"    {{")
            lines.append(f"        size_t tail_offset = head_size + tail.len;")
            lines.append(f"        boat_evm_abi_encode_uint64((uint64_t)tail_offset, head[{i}]);")
            encode_call = info["encode"].format(name=inp["name"])
            lines.append(f"        {encode_call};")
            lines.append(f"    }}")
        else:
            lines.append(f"    /* Arg {i}: {inp['type']} (static) */")
            encode_call = info["encode"].format(name=inp["name"])
            encode_call = encode_call.replace("slot)", f"head[{i}])")
            lines.append(f"    {encode_call};")
        lines.append("")

    lines.append("    /* Assemble: selector + head + tail */")
    lines.append("    size_t total = 4 + head_size + tail.len;")
    lines.append("    uint8_t *out = (uint8_t *)boat_malloc(total);")
    lines.append("    if (!out) { boat_buf_free(&tail); return BOAT_ERROR_MEM_ALLOC; }")
    lines.append("")
    lines.append("    memcpy(out, sig_hash, 4);")
    lines.append("    for (size_t i = 0; i < n_args; i++)")
    lines.append("        memcpy(out + 4 + i * 32, head[i], 32);")
    lines.append("    if (tail.len > 0)")
    lines.append("        memcpy(out + 4 + head_size, tail.data, tail.len);")
    lines.append("    boat_buf_free(&tail);")
    lines.append("")
    lines.append("    *calldata_out = out;")
    lines.append("    *calldata_len_out = total;")
    lines.append("    return BOAT_SUCCESS;")
    lines.append("}")
    return "\n".join(lines)

def generate_encoder(func):
    """Generate C encode function for a contract method."""
    inputs = func.get("inputs", [])
    if has_dynamic_args(inputs):
        return generate_encoder_dynamic(func)
    else:
        return generate_encoder_static(func)

def decoder_params(func):
    """Build C parameter list for a decoder function."""
    outputs = func.get("outputs", [])
    params = ["const uint8_t *data", "size_t data_len"]
    for out in outputs:
        info = sol_type_to_c(out["type"])
        if info:
            out_name = out.get("name", "ret") or "ret"
            if info.get("dynamic"):
                if out["type"] == "string":
                    params.append(f"char **{out_name}")
                    params.append(f"size_t *{out_name}_len")
                else:
                    params.append(f"uint8_t **{out_name}")
                    params.append(f"size_t *{out_name}_len")
            elif info["c_type"] == "uint8_t":
                params.append(f"uint8_t {out_name}[32]")
            elif info["c_type"] == "bool":
                params.append(f"bool *{out_name}")
            else:
                params.append(f"{info['c_type']} *{out_name}")
    return params

def generate_decoder(func):
    """Generate C decode function for a contract method's outputs."""
    name = func["name"]
    outputs = func.get("outputs", [])
    if not outputs:
        return ""

    params = decoder_params(func)

    lines = []
    lines.append(f"/* Decode {name} return values */")
    lines.append(f"BoatResult decode_{name}({', '.join(params)})")
    lines.append("{")

    offset = 0
    for out in outputs:
        info = sol_type_to_c(out["type"])
        out_name = out.get("name", "ret") or "ret"
        if info:
            # Use decode_out template if available (for pointer params like bool *)
            template = info.get("decode_out", info["decode"])
            decode_call = template.format(name=out_name, offset=offset)
            lines.append(f"    {decode_call};")
        offset += 32

    lines.append("    return BOAT_SUCCESS;")
    lines.append("}")
    return "\n".join(lines)

def needs_dynamic_includes(abi):
    """Check if any function in the ABI has dynamic args (needs sha3.h)."""
    for item in abi:
        if item.get("type") != "function":
            continue
        inputs = item.get("inputs", [])
        if has_dynamic_args(inputs):
            return True
    return False

def generate_header(abi, guard_name):
    """Generate .h file content with include guard and function declarations."""
    lines = []
    lines.append("/* Auto-generated by abi2c.py — do not edit */")
    lines.append(f"#ifndef {guard_name}")
    lines.append(f"#define {guard_name}")
    lines.append("")
    lines.append('#include "boat_evm.h"')
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append('extern "C" {')
    lines.append("#endif")
    lines.append("")

    for item in abi:
        if item.get("type") != "function":
            continue
        name = item["name"]
        inputs = item.get("inputs", [])
        outputs = item.get("outputs", [])

        # Encoder declaration
        params = encoder_params(inputs)
        lines.append(f"BoatResult encode_{name}({', '.join(params)});")

        # Decoder declaration
        if outputs:
            dparams = decoder_params(item)
            lines.append(f"BoatResult decode_{name}({', '.join(dparams)});")

        lines.append("")

    lines.append("#ifdef __cplusplus")
    lines.append("}")
    lines.append("#endif")
    lines.append("")
    lines.append(f"#endif /* {guard_name} */")
    return "\n".join(lines)

def generate_source(abi, header_filename):
    """Generate .c file content."""
    lines = []
    lines.append("/* Auto-generated by abi2c.py — do not edit */")
    lines.append(f'#include "{header_filename}"')
    if needs_dynamic_includes(abi):
        lines.append('#include "sha3.h"')
    lines.append('#include <string.h>')
    lines.append("")

    for item in abi:
        if item.get("type") != "function":
            continue
        lines.append(generate_encoder(item))
        lines.append("")
        decoder = generate_decoder(item)
        if decoder:
            lines.append(decoder)
            lines.append("")

    return "\n".join(lines)

def stem_to_guard(stem):
    """Convert a file stem to an include guard name."""
    return stem.upper().replace("-", "_").replace(".", "_") + "_H"

def main():
    parser = argparse.ArgumentParser(description="BoAT v4 ABI-to-C codec generator")
    parser.add_argument("abi_json", help="Path to ABI JSON file")
    parser.add_argument("--output", "-o", help="Output C file path (also generates .h)")
    args = parser.parse_args()

    with open(args.abi_json) as f:
        abi = json.load(f)

    if args.output:
        c_path = Path(args.output)
        h_path = c_path.with_suffix(".h")
        stem = c_path.stem
        guard = stem_to_guard(stem)

        header = generate_header(abi, guard)
        source = generate_source(abi, h_path.name)

        with open(h_path, "w") as f:
            f.write(header + "\n")
        with open(c_path, "w") as f:
            f.write(source + "\n")
        print(f"Generated {h_path} and {c_path}")
    else:
        # Print both to stdout with marker
        stem = Path(args.abi_json).stem + "_codec"
        guard = stem_to_guard(stem)
        header = generate_header(abi, guard)
        source = generate_source(abi, f"{stem}.h")
        print(header)
        print("")
        print("/* ---- end of .h / start of .c ---- */")
        print("")
        print(source)

if __name__ == "__main__":
    main()
