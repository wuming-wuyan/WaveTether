#pragma once
// 周期闸门：按真实时间计时，而不是「循环次数 × Sleep(250)」。
// 一轮循环里夹着 adb IO（Probe 100~500ms、网段扫描可达 800ms）时，
// 按次数估出来的周期会越拉越长，60 秒的任务可能拖到 90 秒以上。
#include <windows.h>

struct PeriodicGate
{
    ULONGLONG next = 0;                 // 0 = 立刻到期

    bool Due(ULONGLONG periodMs)
    {
        ULONGLONG now = GetTickCount64();
        if (now < next) return false;
        next = now + periodMs;
        return true;
    }

    void Reset() { next = 0; }
};
