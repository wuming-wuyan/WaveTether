"""从 WPF 版 Themes/*.xaml 生成 native/theme.h，保证两版逐值同源。"""
import re
def read(p):
    return dict(re.findall(r'x:Key="([A-Za-z]+)" Color="(#[0-9A-Fa-f]{6})"', open(p,encoding='utf-8').read()))
d = read('Themes/Dark.xaml'); l = read('Themes/Light.xaml')
order = ['Bg','TitleBg','FooterBg','WindowBorder','Hairline','HoverFill','SelectFill',
         'Text','TextMuted','TextFaint','Accent','AccentText','AccentSoft','OnAccent',
         'Ok','Warn','Err','ControlBg','ControlBorder','TrackBg']
def rgb(c): return f'RGB(0x{c[1:3]}, 0x{c[3:5]}, 0x{c[5:7]})'
# 少数 token 需要说明用途，生成时一并写出，避免手改 theme.h 后与生成器不同步
NOTES = {'Warn': '   // 琥珀黄：连接中/等待'}
def fn(t, name, light):
    out = [f'inline Theme {name}()', '{', '    Theme t{};']
    for k in order: out.append(f'    t.{k} = {rgb(t[k])};{NOTES.get(k, "")}')
    out.append(f'    t.isLight = {"true" if light else "false"};')
    out += ['    return t;', '}']
    return '\n'.join(out)
hdr = '''// 设计 token —— 自动生成自 WPF 版 Themes/Dark.xaml / Light.xaml（方案 D），逐值同源
// 重新生成：python tools/gen-theme.py
#pragma once
#include <windows.h>

struct Theme
{
    COLORREF Bg, TitleBg, FooterBg, WindowBorder;
    COLORREF Hairline, HoverFill, SelectFill;
    COLORREF Text, TextMuted, TextFaint;
    COLORREF Accent, AccentText, AccentSoft, OnAccent;
    COLORREF Ok, Warn, Err;
    COLORREF ControlBg, ControlBorder, TrackBg;
    bool isLight;
};

inline COLORREF Hex(const wchar_t* h)
{
    auto v = wcstoul(h + 1, nullptr, 16);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

'''
open('native/theme.h','w',encoding='utf-8').write(hdr + fn(d,'DarkTheme',False) + '\n\n' + fn(l,'LightTheme',True) + '\n')
print('theme.h generated')
