# 打包可分发形态。
# 用法：python tools/make_dist.py
#
# 产出（dist/）：
#   WaveTether.exe                  完整版（单文件，内嵌 adb/scrcpy，14 MB）——直接发这个
#   WaveTether-BT-only.exe          极简版（671 KB，只有蓝牙，不含 ADB 工具）
#   WaveTether-<版本>-portable.zip   便携版（exe + tools\，不写临时文件）——有文件夹，必须打包
#   使用说明.txt                    内容可直接复制粘贴到聊天里
#   发布说明-<版本>.txt              收件人第一眼看的：三个版本怎么选、SmartScreen、怎么反馈
#   SHA256SUMS.txt
#
# 为什么不把单文件版也压成 zip：
#   它设计上就是一个文件，再套一层只是多一步解压。
#   例外：邮箱附件（QQ 邮箱 / 163）会拦 .exe，那种情况才需要 zip，
#   届时用 `python tools/make_dist.py --zip-single` 额外生成一份。
import hashlib
import os
import re
import subprocess
import sys
import zipfile


def read_version():
    """版本号只从 native/version.h 读取，避免源码与 exe 的版本信息不一致。"""
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               '..', 'native', 'version.h'), encoding='utf-8') as f:
            m = re.search(r'APP_VERSION_A\s+"([0-9][0-9.]*)"', f.read())
            if m:
                return m.group(1)
    except OSError:
        pass
    raise SystemExit('version.h 里没找到 APP_VERSION_A')


VERSION = read_version()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIVE = os.path.join(ROOT, 'native')
DIST = os.path.join(ROOT, 'dist')

EXE_SEP = os.path.join(NATIVE, 'WaveTether.exe')
EXE_BT = os.path.join(NATIVE, 'WaveTether-BT-only.exe')
EXE_SINGLE = os.path.join(NATIVE, 'WaveTether-single.exe')
NOTE = os.path.join(DIST, '使用说明.txt')
# 收件人第一眼看的文件：版本、怎么选、SmartScreen 怎么过、出问题发什么
RELEASE_NOTE = os.path.join(DIST, '发布说明-%s.txt' % VERSION)
# 第三方许可：LGPL 要求随分发附许可全文 + 声明，不能只放一个 Apache 的 LICENSE.txt
LEGAL = ['LICENSE-WaveTether.txt',
         'THIRD-PARTY-NOTICES.txt',
         'LICENSE-Apache-2.0.txt',
         'LICENSE-LGPL-2.1.txt',
         'LICENSE-Zlib.txt']

SCRCPY_VER = 'scrcpy-win64-v5.0.1'
SCRCPY_SRC = os.path.join(NATIVE, 'tools', 'scrcpy', SCRCPY_VER)

# 运行必需（和 gen_embed.py 的 KEEP 保持一致）+ 许可证
RUNTIME = [
    'scrcpy.exe', 'scrcpy-server', 'adb.exe',
    'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'libusb-1.0.dll',
    'SDL3.dll',
    'avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll', 'swresample-7.dll',
    'LICENSE.txt',          # Apache 2.0 要求随分发保留
]

DIST_EXE = 'WaveTether.exe'
DIST_EXE_LITE = 'WaveTether-BT-only.exe'
ZIP_PORTABLE = 'WaveTether-%s-portable.zip' % VERSION
ZIP_SINGLE = 'WaveTether-%s-single.zip' % VERSION


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def add_zip_entry(zf, src, arcname):
    """固定时间戳写 zip 条目。

    zipfile.write 会把源文件的 mtime 写进每个条目，于是每次重打包（哪怕源码和
    exe 一字未改）zip 的 sha256 都会变，提交过的 SHA256SUMS 立刻变脏 —— 发布核对
    时会被当成「产物不一样」。时间戳固定后，同一批输入打出的 zip 字节级一致。
    """
    zi = zipfile.ZipInfo(arcname, date_time=(1980, 1, 1, 0, 0, 0))
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = 0o644 << 16
    with open(src, 'rb') as f:
        zf.writestr(zi, f.read())


def add_sums_entry(zf, items, arcname='SHA256SUMS.txt'):
    """在压缩包内写一份校验清单（针对解压后的文件路径）。"""
    lines = ['%s  %s\n' % (sha256(src), name) for src, name in items]
    zi = zipfile.ZipInfo(arcname, date_time=(1980, 1, 1, 0, 0, 0))
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = 0o644 << 16
    zf.writestr(zi, ''.join(lines).encode('utf-8'))


def copy_exe(src, dst_name):
    dst = os.path.join(DIST, dst_name)
    if os.path.exists(dst):
        os.remove(dst)
    with open(src, 'rb') as fi, open(dst, 'wb') as fo:
        for chunk in iter(lambda: fi.read(1 << 20), b''):
            fo.write(chunk)
    print('  %-34s %6.1f MB' % (dst_name, os.path.getsize(dst) / 1048576.0))
    return dst


def build_portable():
    path = os.path.join(DIST, ZIP_PORTABLE)
    if os.path.exists(path):
        os.remove(path)
    items = [
        (EXE_SEP, 'WaveTether.exe'),
        (NOTE, '使用说明.txt'),
        (RELEASE_NOTE, os.path.basename(RELEASE_NOTE)),
    ]
    items += [(os.path.join(DIST, f), f) for f in LEGAL]
    items += [(os.path.join(SCRCPY_SRC, f),
               'tools/scrcpy/%s/%s' % (SCRCPY_VER, f)) for f in RUNTIME]
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as zf:
        for src, name in items:
            add_zip_entry(zf, src, name)
        # 包里也放一份校验值：解压后 `sha256sum -c` 能对上，
        # 否则用户拿包里同名但不同哈希的 WaveTether.exe 去对比 dist 的清单，
        # 第一眼就像被篡改过（我们发的单文件版和包里的 exe 本来就不是同一个文件）。
        add_sums_entry(zf, items)
    print('  %-34s %6.1f MB' % (ZIP_PORTABLE, os.path.getsize(path) / 1048576.0))
    return path


def build_single_zip():
    """只在需要走邮箱附件时才生成：邮箱普遍拦 .exe。"""
    path = os.path.join(DIST, ZIP_SINGLE)
    if os.path.exists(path):
        os.remove(path)
    items = [
        (EXE_SINGLE, 'WaveTether.exe'),
        (NOTE, '使用说明.txt'),
        (RELEASE_NOTE, os.path.basename(RELEASE_NOTE)),
    ]
    # 单文件版把工具和许可嵌在 exe 里，但「随分发附许可全文」不能靠
    # 用户先运行再解压拿到，所以这个 zip 里也放一份明文。
    items += [(os.path.join(DIST, f), f) for f in LEGAL]
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as zf:
        for src, name in items:
            add_zip_entry(zf, src, name)
        add_sums_entry(zf, items)
    print('  %-34s %6.1f MB' % (ZIP_SINGLE, os.path.getsize(path) / 1048576.0))
    return path


def verify_dist():
    """校 SHA256SUMS.txt + 跑一遍 dist 里的 exe。CI 里用。

    跑 exe 这部分放在 python 里而不是 .bat：省得在 cmd 里处理路径和退出码。
    """
    bad = 0

    # 先校版本号一致性：rc.exe 不会把「逗号宏」展开到 FILEVERSION，
    # 所以 app.rc 里那份只能写死 —— 那就必须有东西盯着它别和 version.h 跑偏。
    rc = os.path.join(ROOT, 'native', 'app.rc')
    if os.path.exists(rc):
        txt = open(rc, encoding='utf-8').read()
        want = VERSION.split('.')
        want4 = ','.join((want + ['0'] * 4)[:4])
        if ('FILEVERSION     %s' % want4) not in txt:
            print('  [FAIL] app.rc 的 FILEVERSION 与 version.h (%s) 不一致' % want4)
            bad += 1
        else:
            print('  ok   app.rc FILEVERSION = %s' % want4)

    sums = os.path.join(DIST, 'SHA256SUMS.txt')
    if not os.path.exists(sums):
        print('!! 没有 SHA256SUMS.txt')
        return 1
    n = 0
    listed = set()
    with open(sums, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            want, name = line.split('  ', 1)
            listed.add(name)
            p = os.path.join(DIST, name)
            n += 1
            if not os.path.exists(p):
                print('  MISSING  %s' % name)
                bad += 1
            elif sha256(p) != want:
                print('  MISMATCH %s' % name)
                bad += 1
    # 反向检查：dist 里除了清单本身，不该有没被校验的文件
    # （旧的 --zip-single 产物就这么并排躺着，发的时候容易被一起发出去）
    for name in sorted(os.listdir(DIST)):
        if name == 'SHA256SUMS.txt' or name in listed:
            continue
        print('  UNLISTED %s' % name)
        bad += 1
    print('校验 %d 项，异常 %d 项' % (n, bad))
    if n == 0:
        print('  [FAIL] SHA256SUMS.txt 里一条有效条目都没有')
        return 1

    # dist 里的 exe 也要能跑起来（--selftest 退出码 0）
    for exe in sorted(f for f in os.listdir(DIST) if f.endswith('.exe')):
        p = os.path.join(DIST, exe)
        rc = subprocess.run([p, '--selftest'], capture_output=True).returncode
        print('  %s %s --selftest -> %d' % ('ok  ' if rc == 0 else 'FAIL', exe, rc))
        if rc != 0:
            bad += 1

    return 0 if bad == 0 else 1


def main():
    if '--verify' in sys.argv:
        return verify_dist()

    for p in (EXE_SEP, EXE_BT, EXE_SINGLE, NOTE, RELEASE_NOTE):
        if not os.path.exists(p):
            hint = '（蓝牙版要先跑 native\\build.bat bt）' if p == EXE_BT else ''
            raise SystemExit('!! 缺少 %s%s，请先构建/补齐文档' % (p, hint))
    missing = [f for f in RUNTIME if not os.path.exists(os.path.join(SCRCPY_SRC, f))]
    if missing:
        raise SystemExit('!! tools 目录缺少: %s' % ', '.join(missing))

    os.makedirs(DIST, exist_ok=True)
    print('打包 %s：' % VERSION)

    # 上次用 --zip-single 生成的包如果留着，既不会被重新打包也不会进校验清单，
    # 却和本次产物并排放着 —— 发的时候很容易连旧包一起发出去。
    zip_single = os.path.join(DIST, ZIP_SINGLE)
    if '--zip-single' not in sys.argv and os.path.exists(zip_single):
        os.remove(zip_single)
        print('  删除上次的 %s（本次不生成）' % ZIP_SINGLE)

    out = [
        copy_exe(EXE_SINGLE, DIST_EXE),        # 完整版：裸 exe，直接发
        copy_exe(EXE_BT, DIST_EXE_LITE),       # 蓝牙版：裸 exe（单独编译，OriginalFilename 正确）
        build_portable(),
    ]
    if '--zip-single' in sys.argv:
        out.append(build_single_zip())

    with open(os.path.join(DIST, 'SHA256SUMS.txt'), 'w', encoding='utf-8', newline='\n') as f:
        # 说明文档也算交付物，一并列进来，保证 zip 与清单中的文档始终同步
        f.write('# WaveTether %s 交付校验：sha256sum -c SHA256SUMS.txt\n' % VERSION)
        f.write('# 注意：这里的 WaveTether.exe 是「单文件完整版」；\n')
        f.write('#       压缩包里的 WaveTether.exe 是同一个程序（配 tools 用），不是同一个文件。\n')
        for p in out + [NOTE, RELEASE_NOTE] + [os.path.join(DIST, f) for f in LEGAL]:
            f.write('%s  %s\n' % (sha256(p), os.path.basename(p)))
    print('\nSHA256 已写入 dist/SHA256SUMS.txt')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
