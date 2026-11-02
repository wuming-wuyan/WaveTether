#!/usr/bin/env python3
"""静态分析告警基线检查（CI 用）。

为什么不直接卡「0 告警」：MSVC /analyze 在现有代码上有历史遗留告警，
一次性清零既拖时间又容易改出行为差异。

基线按 (文件, 告警码) 分栏记账，而不是只存一个总数。只比总数时，
把 5 条 C6244 改标成 C4459 就能让总数和类型集合都不变；分栏后同样的
替换会表现为两项计数一升一降，直接报红。

基线文件每行：<文件名>\t<告警码>\t<条数>
用 --update 从当前日志重新生成。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, 'analyze_baseline.txt')

WARN_RE = re.compile(r'^(.*?)\((\d+)\)\s*:\s*warning C(\d+)')
CODE_RE = re.compile(r'warning C(\d+)')


def parse_log(text):
    pairs = {}
    for line in text.splitlines():
        m = WARN_RE.match(line)
        if m:
            name = os.path.basename(m.group(1).replace('\\', '/').strip())
            key = (name, 'C' + m.group(3))
        else:
            m2 = CODE_RE.search(line)
            if not m2:
                continue
            key = ('<unknown>', 'C' + m2.group(1))
        pairs[key] = pairs.get(key, 0) + 1
    return pairs


def total(pairs):
    return sum(pairs.values())


def read_baseline(path):
    if not os.path.exists(path):
        return None
    pairs = {}
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split('\t')
        if len(parts) != 3:
            return 'bad-format'
        try:
            pairs[(parts[0], parts[1])] = int(parts[2])
        except ValueError:
            return 'bad-format'
    return pairs


def write_baseline(path, pairs):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# WaveTether static-analysis baseline\n')
        f.write('# <file>\\t<code>\\t<count>; regenerate with --update\n')
        for (name, code), n in sorted(pairs.items(), key=lambda kv: (-kv[1], kv[0])):
            f.write('%s\t%s\t%d\n' % (name, code, n))


def main():
    update = '--update' in sys.argv[1:]
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print('用法: check_analyze.py <cl 输出日志> [--update]')
        return 2
    log = args[0]
    if not os.path.exists(log):
        print('  [FAIL] 找不到分析日志 %s' % log)
        return 1

    text = open(log, 'rb').read().decode('utf-8', 'replace')
    cur = parse_log(text)

    if update:
        write_baseline(BASELINE, cur)
        print('静态分析基线已更新：%d 条 / %d 项' % (total(cur), len(cur)))
        return 0

    base = read_baseline(BASELINE)
    if base is None:
        print('  [FAIL] 缺少 tools/analyze_baseline.txt；先跑 --update')
        return 1
    if base == 'bad-format':
        print('  [FAIL] 基线格式不对；用 --update 重新生成')
        return 1

    print('静态分析告警：%d 条 / %d 项（基线 %d 条 / %d 项）'
          % (total(cur), len(cur), total(base), len(base)))

    increases = []
    for key, n in cur.items():
        b = base.get(key, 0)
        if n > b:
            increases.append((key, b, n))

    if total(cur) > total(base) or increases:
        for (name, code), b, n in sorted(increases, key=lambda x: -(x[2] - x[1]))[:10]:
            print('  [FAIL] %s %s：%d → %d' % (name, code, b, n))
        print('  [FAIL] %d 项超过基线' % len(increases))
        return 1

    print('  ok')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
