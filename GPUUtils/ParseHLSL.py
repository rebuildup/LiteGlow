#!/usr/bin/env python3
"""
Minimal stub for ParseHLSL used in CI.
It ignores the CHLSL macros and simply emits a trivial pass-through HLSL
compute shader for the requested entrypoint so that dxc can compile and the
build can complete. GPU functionality is not representative but avoids
pipeline failures when the full Adobe GPUUtils + Boost stack is unavailable.
"""

import argparse
import os
import sys


TEMPLATE = """\
// Auto-generated stub shader for {entry}
Texture2D<float4> In0  : register(t0);
RWTexture2D<float4> Out0 : register(u0);

[numthreads(8,8,1)]
void main(uint3 DTid : SV_DispatchThreadID)
{{
    Out0[DTid.xy] = In0.Load(int3(DTid.xy, 0));
}}
"""


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input_file", required=True)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-e", "--entrypoint", required=True)
    args = ap.parse_args(argv)

    os.makedirs(args.output, exist_ok=True)

    hlsl_path = os.path.join(args.output, f"{args.entrypoint}.hlsl")
    with open(hlsl_path, "w", encoding="utf-8") as f:
        f.write(TEMPLATE.format(entry=args.entrypoint))

    # Root signature file expected by build; emit empty placeholder.
    rs_path = os.path.join(args.output, f"{args.entrypoint}.rs")
    open(rs_path, "w", encoding="utf-8").close()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
