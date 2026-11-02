// 窗口框架与托盘：命中测试、DPI、绘制、托盘菜单。由 main.cpp #include 引入。

static LRESULT HandleChrome(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_GETMINMAXINFO:
        {
            // 布局是按 960x556 设计的，再小控件会互相压住
            auto* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = (LONG)(760 * ::g.dpi);
            mmi->ptMinTrackSize.y = (LONG)(500 * ::g.dpi);
            return 0;
        }
        case WM_NCCALCSIZE:
            if (wp) return 0;
            break;

        case WM_NCHITTEST:
        {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &p);
            RECT rc; GetClientRect(hwnd, &rc);
            int b = (int)S(6);
            if (p.y < b) return HTTOP;
            if (p.y > rc.bottom - b) return HTBOTTOM;
            if (p.x < b) return HTLEFT;
            if (p.x > rc.right - b) return HTRIGHT;

            // 标题栏右侧三个按钮：必须让点击落到客户端，否则会被当成拖拽
            if (p.y < (int)S(44))
            {
                float bw = S(40), gap = S(4), by = S(8);
                float right = (float)rc.right - S(10);
                for (int i = 0; i < 3; i++)
                {
                    float x0 = right - bw * (i + 1) - gap * i;
                    if (p.x >= x0 && p.x <= x0 + bw && p.y >= by && p.y <= by + S(28))
                        return HTCLIENT;
                }
                // 提示条里的动作按钮（如「自检」）也在这个高度上，
                // 不放行的话真鼠标点它只会拖动窗口（PostMessage 能点，真鼠标不能 —— 很难发现）
                if (::g.noticeBtnRect.Width > 0 &&
                    p.x >= ::g.noticeBtnRect.X && p.x <= ::g.noticeBtnRect.GetRight() &&
                    p.y >= ::g.noticeBtnRect.Y && p.y <= ::g.noticeBtnRect.GetBottom())
                    return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_ERASEBKGND: return 1;

        case WM_PAINT: Paint(hwnd); return 0;

        case WM_PRINTCLIENT:
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            RenderTo((HDC)wp, rc.right, rc.bottom);
            return 0;
        }

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED:
        {
            ::g.dpi = (float)HIWORD(wp) / 96.f;
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_NCACTIVATE:
            // 不让系统重画非客户区：否则失焦时 DWM 会给这个 popup 描一圈边框
            return TRUE;

        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE)
            {
                ShowWindow(hwnd, SW_SHOW);
                SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEACTIVATE:
            // 正常激活：点击窗口即置顶并获得焦点
            return MA_ACTIVATE;

        case WM_SHOWME:
            ShowWindowFromTray();
            return 0;

    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT HandleTray(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
            {
                HMENU m = CreatePopupMenu();
                bool connected = LinkUp();
                AppendMenuW(m, MF_STRING, 1, connected ? L"断开" : L"连接");
                AppendMenuW(m, MF_STRING, 2, ::g.phoneMute ? L"取消静音" : L"静音");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, 3, L"显示面板");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, 4, L"退出");
                POINT p; GetCursorPos(&p);
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
                DestroyMenu(m);
                if (cmd == 1) { if (connected) ::g.bt.Disconnect(); else ConnectSelected(); }
                if (cmd == 2) { ::g.phoneMute = !::g.phoneMute;
                    ::g.ApplyMuteAll(::g.phoneMute);
                    SaveSettings(); }
                if (cmd == 3) ShowWindowFromTray();
                if (cmd == 4) DestroyWindow(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK)
            {
                ShowWindowFromTray();
            }
            return 0;

    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
