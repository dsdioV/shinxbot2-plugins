#!/usr/bin/env python3
"""把 data/0.png 与 data/1.png 转成可随 .so 一起分发 C++ 源文件。

插件部署时只会被复制 ./lib/functions/libnikohug.so，插件源码目录里的
data/ 不会跟着走。为了让单个 .so 自包含，这里把两张叠加图以 base64
字面量内嵌进 nikohug_assets.cpp。

用法（在 functions/nikohug 目录下）:
    python gen_assets.py
"""

import base64
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
DATA = HERE / "data"
OUT_H = HERE / "nikohug_assets.h"
OUT_CPP = HERE / "nikohug_assets.cpp"

CHUNK = 120  # 每个字符串字面量的字节数，避免单行过长

HEADER = """#pragma once

// 本文件由 gen_assets.py 自动生成，请勿手工编辑。
// 素材来源: player233lol/qq_nikohugapi (data/0.png, data/1.png)
// 素材许可: CC-BY-NC 4.0，作者 player233lol

/// 叠加图 0.png 的 base64 文本
extern const char *const kNikohugOverlay0B64;
/// 叠加图 1.png 的 base64 文本
extern const char *const kNikohugOverlay1B64;
"""


def emit_symbol(name: str, payload: bytes) -> str:
    b64 = base64.b64encode(payload).decode("ascii")
    lines = [b64[i : i + CHUNK] for i in range(0, len(b64), CHUNK)]
    body = "\n".join('    "%s"' % line for line in lines)
    return "const char *const %s =\n%s;\n" % (name, body)


def main() -> int:
    blobs = []
    for name in ("0.png", "1.png"):
        p = DATA / name
        if not p.is_file():
            raise SystemExit("missing asset: %s" % p)
        blobs.append((name, p.read_bytes()))

    OUT_H.write_text(HEADER, encoding="utf-8")

    parts = [
        "// 本文件由 gen_assets.py 自动生成，请勿手工编辑。",
        '#include "nikohug_assets.h"',
        "",
    ]
    for idx, (name, payload) in enumerate(blobs):
        parts.append("// data/%s (%d bytes)" % (name, len(payload)))
        parts.append(emit_symbol("kNikohugOverlay%dB64" % idx, payload))

    OUT_CPP.write_text("\n".join(parts), encoding="utf-8")

    total = sum(len(p) for _, p in blobs)
    print("generated %s (%d bytes of assets)" % (OUT_CPP.name, total))
    print("generated %s" % OUT_H.name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
