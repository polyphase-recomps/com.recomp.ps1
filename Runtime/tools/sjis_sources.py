"""Rewrites game sources whose string literals hold non-ASCII text.

The decomp keeps its Japanese text as UTF-8 and builds with -fexec-charset=CP932;
clang only emits UTF-8, so every non-ASCII character inside a string or character
literal is replaced by the octal escapes of its CP932 bytes. Comments are left
alone. Usage: sjis_sources.py <decomp_dir> <out_dir> <source>...
Prints "original|generated" for each rewritten file.
"""
import os
import sys


def convert(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(text[i:j])
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(text[i:j])
            i = j
        elif c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n and text[i] != quote:
                ch = text[i]
                if ch == "\\":
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                if ord(ch) > 127:
                    out.append("".join("\\%03o" % b for b in ch.encode("cp932")))
                else:
                    out.append(ch)
                i += 1
            if i < n:
                out.append(quote)
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main():
    decomp, out_dir = sys.argv[1], sys.argv[2]
    for src in sys.argv[3:]:
        raw = open(src, "rb").read()
        if all(b < 128 for b in raw):
            continue
        text = raw.decode("utf-8")
        rel = os.path.relpath(src, decomp)
        if rel.startswith(".."):
            # an already generated copy (e.g. patched): keep its path from src/ on
            norm = src.replace("\\", "/")
            rel = norm[norm.rfind("/src/") + 1:] if "/src/" in norm else os.path.basename(src)
        dst = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        body = '#line 1 "%s"\n' % src.replace("\\", "/") + convert(text)
        data = body.encode("utf-8", "surrogateescape")
        if not os.path.exists(dst) or open(dst, "rb").read() != data:
            with open(dst, "wb") as f:
                f.write(data)
        print("%s|%s" % (src.replace("\\", "/"), dst.replace("\\", "/")))


if __name__ == "__main__":
    main()
