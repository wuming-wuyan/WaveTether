#!/usr/bin/env python3
"""许可文件一致性检查（CI 用）。

检查 THIRD-PARTY-NOTICES 引用的许可文件是否同时存在于 dist、内嵌清单和 zip，
避免两条打包路径出现悬空引用；同时验证 Apache 文件同文、LGPL/Zlib 文件齐全。

这个脚本把三处对起来：
  1. notices 里点名的每个文件，dist/ 下必须存在
  2. 同一批文件必须在 native/embed_manifest.txt 里（否则单文件版会悬空）
  3. zip 里必须有（否则便携版会悬空）
"""

import hashlib
import os
import re
import sys
import zipfile


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, 'dist')
NOTICES = os.path.join(DIST, 'THIRD-PARTY-NOTICES.txt')
MANIFEST = os.path.join(ROOT, 'native', 'embed_manifest.txt')


def main():
    if not os.path.exists(NOTICES):
        print('  [FAIL] 缺少 dist/THIRD-PARTY-NOTICES.txt')
        return 1
    if not os.path.exists(MANIFEST):
        print('  [FAIL] 缺少 native/embed_manifest.txt（先跑 native/gen_embed.py）')
        return 1

    text = open(NOTICES, encoding='utf-8').read()
    refs = sorted(set(re.findall(r'(LICENSE-[A-Za-z0-9.\-]+\.txt)', text)))
    if not refs:
        print('  [FAIL] notices 里没找到任何 LICENSE-*.txt 引用')
        return 1

    manifest = open(MANIFEST, encoding='utf-8').read()
    # 内嵌清单每行是「序号\t相对路径\t原大小\t压缩后大小」，取第 2 列才是文件名
    embed_names = set()
    for line in manifest.splitlines():
        parts = line.split('\t')
        if len(parts) >= 2 and parts[1]:
            embed_names.add(parts[1].split('/')[-1])

    zips = [f for f in os.listdir(DIST) if f.endswith('.zip')]
    if not zips:
        print('  [FAIL] dist/ 下没有 zip（先跑 make_dist.py）')
        return 1

    # 每个 zip 单独校：之前把所有 zip 的名单并成一个集合，
    # 结果是「A 包含、B 缺」也能通过。
    zip_sets = {}
    for z in zips:
        with zipfile.ZipFile(os.path.join(DIST, z)) as zf:
            zip_sets[z] = {n.split('/')[-1] for n in zf.namelist()}

    bad = 0
    for r in refs:
        in_dist = os.path.exists(os.path.join(DIST, r))
        in_embed = r in embed_names
        missing_zips = [z for z in zips if r not in zip_sets[z]]
        ok = in_dist and in_embed and not missing_zips
        if not ok:
            bad += 1
        print('  %s %-26s dist=%-5s embed=%-5s zip=%s'
              % ('ok  ' if ok else 'FAIL', r, in_dist, in_embed,
                 'all' if not missing_zips else ('MISSING in ' + ','.join(missing_zips))))

    # 反向检查：内嵌清单里的法律文件，dist 下也应该有（避免只进内嵌不进目录）
    # LICENSE.txt 是例外：它是 scrcpy 上游发行包的原文件名（Apache 全文），
    # dist/ 下那份叫 LICENSE-Apache-2.0.txt。这里按「同名同文」校验，
    # 不要求文件名一致，但不能容忍内容不一致。
    for base in sorted(embed_names):
        if base == 'LICENSE.txt':
            a = os.path.join(DIST, 'LICENSE-Apache-2.0.txt')
            if not os.path.exists(a):
                print('  FAIL LICENSE.txt                 dist/ 下没有对应的 LICENSE-Apache-2.0.txt')
                bad += 1
            elif sha256(a) != sha256(os.path.join(ROOT, 'native', 'tools', 'scrcpy',
                                                  'scrcpy-win64-v5.0.1', 'LICENSE.txt')):
                print('  FAIL LICENSE.txt                 与 LICENSE-Apache-2.0.txt 内容不一致')
                bad += 1
            else:
                print('  ok   LICENSE.txt                 = LICENSE-Apache-2.0.txt（同名同文）')
            continue
        if base.startswith('LICENSE') or 'NOTICES' in base:
            if not os.path.exists(os.path.join(DIST, base)):
                print('  FAIL %-26s 在内嵌清单里但 dist/ 下没有' % base)
                bad += 1

    print('许可一致性：%d 处引用，%d 处异常' % (len(refs), bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
