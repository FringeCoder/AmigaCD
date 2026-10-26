/* stand-in: the prefs struct holds GUI colours; the CPU never reads them */
#pragma once
namespace gcn { struct Color { int r = 0, g = 0, b = 0, a = 255; Color() {} Color(int r_, int g_, int b_, int a_ = 255) : r(r_), g(g_), b(b_), a(a_) {} }; }
