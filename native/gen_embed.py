# 把 tools/scrcpy 下运行必需的文件打成 Windows 资源，供单文件 exe 内嵌。
# 只收集程序真正会用到的文件（排除日志、图片、bat/vbs 等手册类文件）。
#
# 每个文件先用 Windows 自带的 XPRESS 算法压缩（ntdll!RtlCompressBuffer，
# 无需第三方库，解压端用同一族 API），实测 25MB → 约 14MB。
#
# 产出：
#   embed.rc           资源脚本（每个文件一个 RCDATA，序号为资源名）
#   embed_manifest.txt "序号 <TAB> 相对路径 <TAB> 原始大小 <TAB> 压缩后大小 <TAB> SHA-256"
#                      （压缩后大小 = 0 表示该文件未压缩，按原样写入）
#   embed_hash.txt     内嵌内容的 8 位哈希（决定运行时释放目录名）
#   embed_blobs\       实际嵌入的二进制（压缩后的，或未压缩的原始文件）
import ctypes
import hashlib
import os
import shutil
import sys

ROOT = os.path.join('tools', 'scrcpy')
OUT_RC = 'embed.rc'
OUT_MAN = 'embed_manifest.txt'
OUT_HASH = 'embed_hash.txt'
BLOBS = 'embed_blobs'

# 运行必需 + 法律必需。
# LICENSE.txt 是 scrcpy 官方发行包里的 Apache-2.0 全文：
# 单文件版把 scrcpy 内嵌进 exe，如果运行时目录里一份许可都没有，
# 就等于「随程序分发但没有许可文本」——Apache-2.0 第 4 条要求给接收者一份。
KEEP = {
    'scrcpy.exe', 'scrcpy-server', 'adb.exe',
    'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'libusb-1.0.dll',
    'SDL3.dll',
    'avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll', 'swresample-7.dll',
    'LICENSE.txt', 'LICENSE-LGPL-2.1.txt', 'LICENSE-Apache-2.0.txt',
    'LICENSE-Zlib.txt', 'THIRD-PARTY-NOTICES.txt',
}

# 注意：winnt.h 里 COMPRESSION_FORMAT_XPRESS = 0x0003（0x0002 是更老的 LZNT1）。
# 两边必须一致，否则解压端会直接失败。实测 XPRESS 比 LZNT1 小约 9 个百分点。
COMPRESSION_FORMAT_XPRESS = 0x0003
COMPRESSION_ENGINE_MAXIMUM = 0x0100
CHUNK = 4096
HEADROOM = 262144          # 输出缓冲余量：不可压缩的数据（scrcpy-server）需要足够大的空间


def _ntdll():
    n = ctypes.WinDLL('ntdll')
    n.RtlGetCompressionWorkSpaceSize.argtypes = [
        ctypes.c_ushort, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong)]
    n.RtlCompressBuffer.argtypes = [
        ctypes.c_ushort, ctypes.c_char_p, ctypes.c_ulong, ctypes.c_char_p,
        ctypes.c_ulong, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.c_char_p]
    n.RtlCompressBuffer.restype = ctypes.c_long
    n.RtlDecompressBuffer.argtypes = [
        ctypes.c_ushort, ctypes.c_char_p, ctypes.c_ulong, ctypes.c_char_p,
        ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]
    n.RtlDecompressBuffer.restype = ctypes.c_long
    return n


def compress(n, data):
    """返回 (压缩后字节, 0)；无法压缩（会变大）时返回 (None, 0) 让调用方存原始数据。"""
    fmt = COMPRESSION_FORMAT_XPRESS | COMPRESSION_ENGINE_MAXIMUM
    ws_size = ctypes.c_ulong()
    frag = ctypes.c_ulong()
    n.RtlGetCompressionWorkSpaceSize(ctypes.c_ushort(fmt), ctypes.byref(ws_size), ctypes.byref(frag))
    ws = ctypes.create_string_buffer(ws_size.value)
    out = ctypes.create_string_buffer(len(data) + HEADROOM)
    final = ctypes.c_ulong()
    st = n.RtlCompressBuffer(ctypes.c_ushort(fmt), data, len(data), out, len(out),
                             CHUNK, ctypes.byref(final), ws)
    if st != 0 or final.value == 0 or final.value >= len(data):
        return None
    blob = out.raw[:final.value]

    # 构建时自检往返：压坏了就在打包阶段报错，而不是等用户运行时才炸
    back = ctypes.create_string_buffer(len(data))
    fin2 = ctypes.c_ulong()
    st2 = n.RtlDecompressBuffer(ctypes.c_ushort(COMPRESSION_FORMAT_XPRESS),
                                back, len(data), blob, len(blob), ctypes.byref(fin2))
    if st2 != 0 or fin2.value != len(data) or back.raw[:len(data)] != data:
        raise SystemExit('!! 压缩往返自检失败: %s' % name if False else
                         '!! 压缩往返自检失败 (st=%d, size=%d/%d)' % (st2, fin2.value, len(data)))
    return blob


def main():
    base = None
    for d in sorted(os.listdir(ROOT)):
        if os.path.isdir(os.path.join(ROOT, d)) and d.startswith('scrcpy-win64-'):
            base = d
            break

    if not base:
        print('!! 没找到 tools/scrcpy/scrcpy-win64-* 目录')
        return 1

    srcdir = os.path.join(ROOT, base)
    files = [(f, os.path.join(srcdir, f))
             for f in sorted(os.listdir(srcdir))
             if f in KEEP and os.path.isfile(os.path.join(srcdir, f))]

    missing = KEEP - {f for f, _ in files}
    if missing:
        print('!! 缺少必需文件:', ', '.join(sorted(missing)))
        return 1

    if os.path.isdir(BLOBS):
        shutil.rmtree(BLOBS)
    os.makedirs(BLOBS)

    n = _ntdll()
    rc = ['#pragma code_page(65001)', '#include <windows.h>', '']
    manifest = []
    digest = hashlib.sha1()
    total = 0
    packed = 0

    for i, (name, path) in enumerate(files, start=1):
        with open(path, 'rb') as f:
            data = f.read()
        total += len(data)

        # 内容哈希按**原始**内容算：压缩算法的改动不应该导致重新释放
        digest.update(name.encode('utf-8'))
        digest.update(str(len(data)).encode('ascii'))
        digest.update(data)

        blob = compress(n, data)
        if blob is None:
            blob, comp_size = data, 0          # 压不动就原样存
        else:
            comp_size = len(blob)
        packed += len(blob)

        blob_path = os.path.join(BLOBS, '%d.bin' % i)
        with open(blob_path, 'wb') as f:
            f.write(blob)
        # 绝对路径：CMake 在 build 目录里调 rc.exe，相对路径会找不到 blob
        rc.append('%d RCDATA "%s"' % (i, os.path.abspath(blob_path).replace('\\', '\\\\')))
        manifest.append('%d\ttools/scrcpy/%s/%s\t%d\t%d\t%s'
                        % (i, base, name, len(data), comp_size,
                           hashlib.sha256(data).hexdigest()))

    short = digest.hexdigest()[:8]

    rc.append('')
    rc.append('IDR_EMBED_MANIFEST RCDATA "embed_manifest.txt"')
    rc.append('IDR_EMBED_HASH RCDATA "embed_hash.txt"')

    with open(OUT_RC, 'w', encoding='utf-8') as f:
        f.write('\n'.join(rc) + '\n')
    with open(OUT_MAN, 'w', encoding='utf-8') as f:
        f.write('\n'.join(manifest) + '\n')
    with open(OUT_HASH, 'w', encoding='ascii') as f:
        f.write(short)

    print('打包 %d 个文件，原始 %.1f MB → 内嵌 %.1f MB（%.0f%%），内容哈希 %s'
          % (len(files), total / 1048576.0, packed / 1048576.0,
             100.0 * packed / max(total, 1), short))
    return 0


if __name__ == '__main__':
    sys.exit(main())
