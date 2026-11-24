#include "ui_icons.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <new>
#include <vector>

namespace viewery::ui {
namespace {

// All geometry uses a 48x48 logical grid with a 3-unit outline. A small
// supersampled coverage mask keeps strokes smooth at fractional display scales.
constexpr int grid=48, samples=4;

struct Dib {
    HDC dc=nullptr;
    HBITMAP bitmap=nullptr;
    HGDIOBJ previous=nullptr;
    std::uint32_t* pixels=nullptr;
    Dib(HDC reference,int side) {
        dc=CreateCompatibleDC(reference);
        if(!dc) return;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=side;info.bmiHeader.biHeight=-side;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* bits=nullptr;
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!bitmap||!bits) return;
        pixels=static_cast<std::uint32_t*>(bits);
        previous=SelectObject(dc,bitmap);
        std::fill_n(pixels,static_cast<std::size_t>(side)*side,0U);
    }
    ~Dib() {
        if(previous) SelectObject(dc,previous);
        if(bitmap) DeleteObject(bitmap);
        if(dc) DeleteDC(dc);
    }
    Dib(const Dib&)=delete;
    Dib& operator=(const Dib&)=delete;
    explicit operator bool() const {return dc&&bitmap&&pixels&&previous&&previous!=HGDI_ERROR;}
};

struct Paths {
    HDC dc;
    void line(std::initializer_list<POINT> points) const {Polyline(dc,points.begin(),static_cast<int>(points.size()));}
    void box(int l,int t,int r,int b,int radius=0) const {
        if(radius) RoundRect(dc,l,t,r,b,radius,radius);else Rectangle(dc,l,t,r,b);
    }
    void circle(int l,int t,int r,int b) const {Ellipse(dc,l,t,r,b);}
    void solid(std::initializer_list<POINT> points) const {
        const auto old=SelectObject(dc,GetStockObject(DC_BRUSH));
        Polygon(dc,points.begin(),static_cast<int>(points.size()));SelectObject(dc,old);
    }
    void dot(int x,int y,int radius=2) const {
        const auto old=SelectObject(dc,GetStockObject(DC_BRUSH));
        circle(x-radius,y-radius,x+radius+1,y+radius+1);SelectObject(dc,old);
    }
    void arrow(int left,int y,int right) const {
        line({{left,y},{right,y}});line({{right-6,y-6},{right,y},{right-6,y+6}});
    }
    void person(int x,int y) const {
        circle(x-6,y-12,x+6,y);
        Arc(dc,x-12,y+3,x+12,y+21,x+12,y+12,x-12,y+12);
        line({{x-12,y+12},{x-12,y+15},{x+12,y+15},{x+12,y+12}});
    }
};

void geometry(HDC dc,Icon icon)
{
    const Paths p{dc};
    switch(icon) {
    case Icon::folder:
        p.line({{6,38},{6,11},{18,11},{23,16},{39,16},{39,21}});
        p.line({{6,38},{11,21},{42,21},{37,38},{6,38}});break;
    case Icon::catalog:
        for(int y:{13,24,35}) {p.dot(9,y,1);p.line({{17,y},{40,y}});}break;
    case Icon::info:
        p.circle(7,7,41,41);p.dot(24,15,1);p.line({{24,22},{24,34}});break;
    case Icon::exportFile:
        p.line({{24,40},{9,40},{9,8},{28,8},{36,16},{36,23}});
        p.line({{28,8},{28,16},{36,16}});p.arrow(22,34,41);break;
    case Icon::more:
        for(int x:{11,24,37}) p.dot(x,24,2);break;
    case Icon::first:
        p.line({{10,10},{10,38}});p.line({{35,11},{19,24},{35,37}});break;
    case Icon::chevronLeft:p.line({{30,10},{16,24},{30,38}});break;
    case Icon::play:p.solid({{15,8},{38,24},{15,40}});break;
    case Icon::pause:
        p.solid({{13,9},{19,9},{19,39},{13,39}});
        p.solid({{29,9},{35,9},{35,39},{29,39}});break;
    case Icon::chevronRight:p.line({{18,10},{32,24},{18,38}});break;
    case Icon::last:
        p.line({{38,10},{38,38}});p.line({{13,11},{29,24},{13,37}});break;
    case Icon::fit:
        p.box(6,10,42,38,4);p.line({{15,16},{11,16},{11,21}});
        p.line({{33,32},{37,32},{37,27}});p.line({{11,16},{19,24}});
        p.line({{37,32},{29,24}});break;
    case Icon::fullscreen:
        p.line({{7,18},{7,7},{18,7}});p.line({{30,7},{41,7},{41,18}});
        p.line({{41,30},{41,41},{30,41}});p.line({{18,41},{7,41},{7,30}});break;
    case Icon::restore:
        p.box(7,16,33,41,2);p.line({{16,16},{16,7},{41,7},{41,32},{33,32}});break;
    case Icon::pin:
        p.line({{18,7},{35,7},{31,12},{31,23},{37,29},{13,29},{19,23},{19,12},{18,7}});
        p.line({{25,29},{25,42}});break;
    case Icon::openWindow:
        p.line({{24,9},{9,9},{9,39},{39,39},{39,24}});
        p.line({{24,7},{41,7},{41,24}});p.line({{41,7},{23,25}});break;
    case Icon::close:
        p.line({{10,10},{38,38}});p.line({{38,10},{10,38}});break;
    case Icon::catalogBottom:
        p.box(6,8,42,40,3);p.line({{6,30},{42,30}});
        for(int x:{15,24,33}) p.line({{x,34},{x,37}});break;
    case Icon::catalogLeft:
        p.box(6,8,42,40,3);p.line({{18,8},{18,40}});
        for(int y:{16,24,32}) p.line({{10,y},{14,y}});break;
    case Icon::volume:
    case Icon::muted:
        p.line({{6,19},{14,19},{24,11},{24,37},{14,29},{6,29},{6,19}});
        if(icon==Icon::muted) {
            p.line({{31,18},{41,30}});p.line({{41,18},{31,30}});
        } else {
            Arc(dc,19,15,37,33,31,33,31,15);Arc(dc,15,8,45,40,34,40,34,8);
        }
        break;
    case Icon::camera:
        p.line({{7,38},{7,14},{16,14},{20,8},{30,8},{34,14},{41,14},{41,38},{7,38}});
        p.circle(17,18,33,34);p.dot(36,19,1);break;
    case Icon::timestamp:
        p.circle(6,6,37,37);p.line({{21,12},{21,22},{28,26}});
        p.arrow(28,37,43);break;
    case Icon::rename:
        p.line({{9,38},{11,28},{30,9},{39,18},{20,37},{9,38}});
        p.line({{27,12},{36,21}});p.line({{9,38},{17,36},{11,30}});break;
    case Icon::settings:
        p.line({{19,6},{29,6},{30,12},{34,15},{40,13},{45,22},{40,26},{39,31},{43,36},{36,43},
            {31,39},{26,40},{22,45},{13,40},{15,34},{12,30},{6,29},{6,19},{12,18},{15,14},{13,8},{19,6}});
        p.circle(17,17,33,33);break;
    case Icon::checkbox:
    case Icon::checkboxChecked:
        p.box(8,8,40,40,5);
        if(icon==Icon::checkboxChecked) p.line({{15,24},{22,31},{34,17}});
        break;
    case Icon::revealFile:
        p.line({{6,36},{6,12},{18,12},{22,17},{37,17},{37,22}});
        p.line({{6,36},{11,22},{38,22}});p.circle(24,25,40,41);
        p.line({{37,38},{43,44}});break;
    case Icon::artist:p.person(24,20);break;
    case Icon::none:break;
    }
}

bool paintAt(HDC dc,Icon icon,RECT area,COLORREF color)
{
    const int state=SaveDC(dc);
    if(!state) return false;
    LOGBRUSH brush{BS_SOLID,color,0};
    HPEN pen=ExtCreatePen(PS_GEOMETRIC|PS_SOLID|PS_ENDCAP_ROUND|PS_JOIN_ROUND,3,&brush,0,nullptr);
    if(!pen) {RestoreDC(dc,state);return false;}
    IntersectClipRect(dc,area.left,area.top,area.right,area.bottom);
    SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,grid,grid,nullptr);
    SetViewportExtEx(dc,area.right-area.left,area.bottom-area.top,nullptr);
    SetWindowOrgEx(dc,0,0,nullptr);SetViewportOrgEx(dc,area.left,area.top,nullptr);
    SelectObject(dc,pen);SelectObject(dc,GetStockObject(NULL_BRUSH));SetDCBrushColor(dc,color);
    SetBkMode(dc,TRANSPARENT);geometry(dc,icon);
    RestoreDC(dc,state);DeleteObject(pen);
    return true;
}

const std::vector<std::uint8_t>* coverage(HDC dc,Icon icon,int side)
{
    using Key=std::pair<Icon,int>;
    thread_local std::map<Key,std::vector<std::uint8_t>> masks;
    const Key key{icon,side};
    if(const auto found=masks.find(key);found!=masks.end()) return &found->second;
    const int large=side*samples;
    Dib mask(dc,large);
    if(!mask||!paintAt(mask.dc,icon,{0,0,large,large},RGB(255,255,255))) return nullptr;
    GdiFlush();
    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(side)*side);
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
        unsigned sum=0;
        for(int dy=0;dy<samples;++dy) for(int dx=0;dx<samples;++dx)
            sum+=mask.pixels[static_cast<std::size_t>(y*samples+dy)*large+x*samples+dx]&255U;
        alpha[static_cast<std::size_t>(y)*side+x]=static_cast<std::uint8_t>((sum+samples*samples/2)/(samples*samples));
    }
    // Cache pixels, never GDI handles. Bound memory when moving between monitors.
    if(masks.size()>=96) masks.erase(masks.begin());
    return &masks.emplace(key,std::move(alpha)).first->second;
}

} // namespace

bool drawIcon(HDC dc,RECT bounds,Icon icon,COLORREF color,UINT dpi)
{
    if(!dc||icon==Icon::none||static_cast<unsigned>(icon)>static_cast<unsigned>(Icon::artist)) return false;
    const auto width=static_cast<long long>(bounds.right)-bounds.left;
    const auto height=static_cast<long long>(bounds.bottom)-bounds.top;
    if(width<=2||height<=2||width>std::numeric_limits<int>::max()||height>std::numeric_limits<int>::max()) return false;
    const int requested=std::clamp(MulDiv(20,static_cast<int>(std::min(dpi?dpi:96U,768U)),96),1,128);
    const int side=std::min({requested,static_cast<int>(width)-2,static_cast<int>(height)-2});
    const int x=bounds.left+(static_cast<int>(width)-side)/2,y=bounds.top+(static_cast<int>(height)-side)/2;
    const RECT area{x,y,x+side,y+side};
    try {
        const auto* alpha=coverage(dc,icon,side);
        Dib output(dc,side);
        if(alpha&&output) {
            const unsigned red=GetRValue(color),green=GetGValue(color),blue=GetBValue(color);
            for(std::size_t index=0;index<alpha->size();++index) {
                const unsigned a=(*alpha)[index];
                output.pixels[index]=((blue*a+127)/255)|(((green*a+127)/255)<<8)|(((red*a+127)/255)<<16)|(a<<24);
            }
            const BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            if(AlphaBlend(dc,x,y,side,side,output.dc,0,0,side,side,blend)) return true;
        }
    } catch(const std::bad_alloc&) {
        // Resource pressure must not turn toolbar controls into missing glyphs.
    }
    return paintAt(dc,icon,area,color);
}

} // namespace viewery::ui
