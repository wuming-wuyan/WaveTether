// 鼠标与键盘处理。由 main.cpp #include 引入。

// 倒序命中：后绘制的控件在上层，和原来的循环一致。
static const Hit* HitAt(POINT p)
{
    for (auto it = ::g.hits.rbegin(); it != ::g.hits.rend(); ++it)
        if (it->r.Contains((float)p.x, (float)p.y)) return &*it;
    return nullptr;
}

static LRESULT HandleMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_MOUSEMOVE:
        {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (::g.dragSlider)
            {
                for (auto& h : ::g.hits)
                    if (h.id == ID_SLIDER)
                    {
                        float t = (p.x - h.r.X) / h.r.Width;
                        t = std::clamp(t, 0.f, 1.f);
                        ::g.SetActiveVol(t);
                        if (::g.volMode == 0) ::g.ApplyVolumeAll(t);
                        else
                        {
                            auto k = ::g.SelectedKey();
                            for (auto& e : ::g.devVols)
                                if (e.key == k) { ::g.oa.SetSessionVolume(e.instanceId, t); break; }
                        }
                    }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            int oldHover = ::g.hover, oldIdx = ::g.hoverIdx;
            ::g.hover = ID_NONE; ::g.hoverIdx = -1;
            if (const Hit* hit = HitAt(p))
            {
                ::g.hover = hit->id; ::g.hoverIdx = hit->index;
            }
            if (oldHover != ::g.hover || oldIdx != ::g.hoverIdx) InvalidateRect(hwnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            ::g.hover = ID_NONE; ::g.hoverIdx = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
        {
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (const Hit* hit = HitAt(p))
            {
                ::g.pressed = hit->id; ::g.pressedIdx = hit->index;
                if (hit->id == ID_SLIDER)
                {
                    // 点一下就跳到该位置：原来只在 WM_MOUSEMOVE 里生效，
                    // 单击滑块不会改变数值，用户会以为滑块坏了。
                    ::g.dragSlider = true;
                    SetCapture(hwnd);
                    float t = (p.x - hit->r.X) / hit->r.Width;
                    t = std::clamp(t, 0.f, 1.f);
                    ::g.SetActiveVol(t);
                    if (::g.volMode == 0) ::g.ApplyVolumeAll(t);
                    else if (auto* e = ::g.SelectedEntry())
                    {
                        ::g.oa.SetSessionVolume(e->instanceId, t);
                        e->vol = t;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                // 优先级列表：点在复选框列 → 切换勾选；点其它位置 → 手动切换到该设备
                if (hit->id == ID_PRIO_LIST && hit->index >= 0)
                {
                    float cbX = hit->r.X + S(24);
                    if (p.x >= cbX - S(8) && p.x <= cbX + S(24)) TogglePrioCheck(hit->index);
                    else SwitchOutputTo(hit->index);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            PerfScope psClick("CLICKUP", 20.0, 0);
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (::g.dragSlider)
            {
                ::g.dragSlider = false;
                ReleaseCapture();
                SaveSettings();
            }
            if (const Hit* hit = HitAt(p))
            {
                if (hit->id == ::g.pressed && hit->index == ::g.pressedIdx)
                    DoClick(hit->id, hit->index);
            }
            // 帮助/设置只通过明确的关闭按钮关闭，避免误点面板外时丢失内容
            ::g.pressed = ID_NONE; ::g.pressedIdx = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &p);
            int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;

            // 设置面板：主体区域直接滚动（不能被控件命中抢走）
            if (::g.settingsOpen && ::g.settingsBody.Contains((float)p.x, (float)p.y))
            {
                ::g.settingsScroll = std::max(0.f, ::g.settingsScroll + delta * S(36));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            for (auto& h : ::g.hits)
            {
                if (!h.r.Contains((float)p.x, (float)p.y)) continue;
                if (h.id == ID_BT_LIST) ::g.scrollDev = std::max(0, ::g.scrollDev + delta);
                if (h.id == ID_PRIO_LIST) ::g.scrollPrio = std::max(0, ::g.scrollPrio + delta);
                if (h.id == ID_HELP_BODY) ::g.helpScroll = std::max(0.f, ::g.helpScroll + delta * S(36));
                if (h.id == ID_SET_BODY)  ::g.settingsScroll = std::max(0.f, ::g.settingsScroll + delta * S(36));
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            return 0;
        }

    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT HandleKeys(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_KEYDOWN:
            if (::g.pairOpen)
            {
                std::wstring* target = (::g.pairFocus == 0) ? &::g.pairAddr
                                     : (::g.pairFocus == 1) ? &::g.pairCode : &::g.pairPort;
                size_t limit = (::g.pairFocus == 0) ? 21 : (::g.pairFocus == 1 ? 6 : 5);
                if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))
                {
                    // 粘贴：地址/配对码可能来自别处，能贴就不用打
                    if (OpenClipboard(hwnd))
                    {
                        if (HANDLE hd = GetClipboardData(CF_UNICODETEXT))
                        {
                            if (auto* txt = (const wchar_t*)GlobalLock(hd))
                            {
                                for (const wchar_t* q = txt; *q && target->size() < limit; ++q)
                                {
                                    wchar_t c = *q;
                                    bool ok = (c >= L'0' && c <= L'9') ||
                                              (::g.pairFocus == 0 && (c == L'.' || c == L':'));
                                    if (ok) *target += c;
                                }
                                GlobalUnlock(hd);
                            }
                        }
                        CloseClipboard();
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wp == VK_BACK) { if (!target->empty()) target->pop_back(); }
                else if (wp == VK_TAB) { ::g.pairFocus = (::g.pairFocus + 1) % 3; }
                else if (wp == VK_ESCAPE) { ::g.pairOpen = false; }
                else if (wp == VK_RETURN)
                {
                    // 回车 = 直接开始配对
                    if (!::g.pairAddr.empty() && !::g.pairCode.empty())
                    {
                        std::string a = WideToUtf8(::g.pairAddr);
                        std::string c = WideToUtf8(::g.pairCode);
                        std::string p = WideToUtf8(::g.pairPort);
                        ::g.adb.PairWirelessAsync(a, c, p);
                        ::g.pairStatusLocal = L"正在配对…";
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_ESCAPE)
            {
                // Esc 只关浮层，不越权关窗口（否则会绕过「关闭=收进托盘」的设置）
                if (::g.showWizard)
                {
                    // 关掉向导 = 用户决定跳过，记下来，否则每次启动都再弹一遍
                    ::g.showWizard = false;
                    ::g.setupDone = true;
                    SaveSettings();
                }
                else if (::g.helpOpen) ::g.helpOpen = false;
                else if (::g.settingsOpen) ::g.settingsOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_CHAR:
            if (::g.pairOpen)
            {
                wchar_t c = (wchar_t)wp;
                std::wstring* target = (::g.pairFocus == 0) ? &::g.pairAddr
                                     : (::g.pairFocus == 1) ? &::g.pairCode : &::g.pairPort;
                size_t limit = (::g.pairFocus == 0) ? 21 : (::g.pairFocus == 1 ? 6 : 5);
                bool ok = (c >= L'0' && c <= L'9') || (::g.pairFocus == 0 && (c == L'.' || c == L':'));
                if (ok && target->size() < limit) *target += c;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;

    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
