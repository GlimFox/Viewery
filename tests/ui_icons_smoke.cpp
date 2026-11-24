#include "ui_icons.hpp"
#include "frame_image.hpp"

#include <objbase.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace viewery;
using ui::Icon;
namespace fs=std::filesystem;
namespace {
constexpr COLORREF foreground=RGB(241,242,247);
int checks=0;
void require(bool condition,const char* message) {
    ++checks;std::cout<<(condition?"OK ":"FAIL ")<<message<<std::endl;
    if(!condition)throw std::runtime_error(message);
}
struct NamedIcon {Icon icon;const wchar_t* name;};
constexpr std::array<NamedIcon,29> icons={{
    NamedIcon{Icon::folder,L"Folder"},{Icon::catalog,L"Catalog"},{Icon::info,L"Info"},{Icon::exportFile,L"Export"},
    {Icon::more,L"More"},{Icon::first,L"First file"},{Icon::chevronLeft,L"Previous frame"},{Icon::play,L"Play"},
    {Icon::pause,L"Pause"},{Icon::chevronRight,L"Next frame"},{Icon::last,L"Last file"},{Icon::fit,L"Fit"},
    {Icon::fullscreen,L"Fullscreen"},{Icon::restore,L"Restore"},{Icon::pin,L"Pin"},{Icon::openWindow,L"Open window"},
    {Icon::close,L"Close"},{Icon::catalogBottom,L"Bottom catalog"},{Icon::catalogLeft,L"Left catalog"},
    {Icon::volume,L"Volume"},{Icon::muted,L"Muted"},{Icon::camera,L"Capture frame"},{Icon::timestamp,L"Timeline time"},
    {Icon::rename,L"Rename"},{Icon::settings,L"Settings"},{Icon::checkbox,L"Select files"},
    {Icon::checkboxChecked,L"Selected files"},{Icon::revealFile,L"Reveal file"},{Icon::artist,L"Artist registry"}
}};
struct Canvas {
    HDC dc=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=nullptr;
    HGDIOBJ old=nullptr;
    std::uint32_t* pixels=nullptr;
    int width,height;
    Canvas(int w,int h):width(w),height(h) {
        if(!dc)throw std::runtime_error("Cannot create test DC");
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
        if(!bitmap) {DeleteDC(dc);throw std::runtime_error("Cannot create test DIB");}
        old=SelectObject(dc,bitmap);clear();
    }
    ~Canvas(){SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);}
    Canvas(const Canvas&)=delete;
    Canvas& operator=(const Canvas&)=delete;
    std::size_t size()const{return static_cast<std::size_t>(width)*height;}
    void clear() {GdiFlush();std::fill(pixels,pixels+size(),0xff171a21U);}
    std::vector<std::uint32_t> copy()const {GdiFlush();return {pixels,pixels+size()};}
    bool save(const fs::path& path)const {
        GdiFlush();CapturedFrame frame;frame.width=width;frame.height=height;
        const auto* first=reinterpret_cast<const std::uint8_t*>(pixels);frame.bgra.assign(first,first+size()*4);
        for(std::size_t at=3;at<frame.bgra.size();at+=4)frame.bgra[at]=255;
        return saveFramePng(path,frame);
    }
};
bool inside(RECT area,int x,int y){return x>=area.left&&x<area.right&&y>=area.top&&y<area.bottom;}
bool outsideUntouched(const Canvas& canvas,const std::vector<std::uint32_t>& before,RECT area) {
    GdiFlush();for(int y=0;y<canvas.height;++y)for(int x=0;x<canvas.width;++x)
        if(!inside(area,x,y)&&canvas.pixels[static_cast<std::size_t>(y)*canvas.width+x]!=before[static_cast<std::size_t>(y)*canvas.width+x])return false;
    return true;
}
std::size_t changedPixels(const Canvas& canvas,const std::vector<std::uint32_t>& before) {
    GdiFlush();std::size_t changed=0;for(std::size_t at=0;at<canvas.size();++at)if((canvas.pixels[at]^before[at])&0x00ffffffU)++changed;return changed;
}
std::vector<std::uint32_t> rendered(Icon icon,UINT dpi,HGDIOBJ font=GetStockObject(DEFAULT_GUI_FONT)) {
    const int side=MulDiv(64,static_cast<int>(dpi),96);Canvas canvas(side,side);
    SelectObject(canvas.dc,font);const RECT area{side/6,side/7,side-side/6,side-side/7};
    if(!ui::drawIcon(canvas.dc,area,icon,foreground,dpi))throw std::runtime_error("Renderer rejected a supported icon");
    return canvas.copy();
}
void checkStatePairs(UINT dpi) {
    for(const auto [first,second]:{std::pair{Icon::play,Icon::pause},std::pair{Icon::volume,Icon::muted},
        std::pair{Icon::fullscreen,Icon::restore},std::pair{Icon::checkbox,Icon::checkboxChecked},
        std::pair{Icon::catalogBottom,Icon::catalogLeft},std::pair{Icon::chevronLeft,Icon::chevronRight},
        std::pair{Icon::first,Icon::last}})
        require(rendered(first,dpi)!=rendered(second,dpi),"dynamic or directional icon states have distinct pixels");
}
void checkEveryIcon(UINT dpi) {
    const int scale=static_cast<int>(dpi);Canvas canvas(MulDiv(78,scale,96),MulDiv(66,scale,96));
    const RECT area{MulDiv(11,scale,96),MulDiv(9,scale,96),MulDiv(65,scale,96),MulDiv(56,scale,96)};
    for(const auto& item:icons) {
        canvas.clear();const auto before=canvas.copy();const auto font=GetCurrentObject(canvas.dc,OBJ_FONT);
        const auto pen=GetCurrentObject(canvas.dc,OBJ_PEN),brush=GetCurrentObject(canvas.dc,OBJ_BRUSH),bitmap=GetCurrentObject(canvas.dc,OBJ_BITMAP);
        const int savedBkMode=SetBkMode(canvas.dc,OPAQUE),savedFill=SetPolyFillMode(canvas.dc,WINDING);
        const auto savedColor=SetTextColor(canvas.dc,RGB(220,30,150));
        require(ui::drawIcon(canvas.dc,area,item.icon,foreground,dpi),"supported icon renders successfully");
        require(changedPixels(canvas,before)>2,"supported icon produces visible marks");
        require(outsideUntouched(canvas,before,area),"icon changes no destination pixels outside its button bounds");
        require(GetCurrentObject(canvas.dc,OBJ_FONT)==font&&GetCurrentObject(canvas.dc,OBJ_PEN)==pen&&GetCurrentObject(canvas.dc,OBJ_BRUSH)==brush&&
            GetCurrentObject(canvas.dc,OBJ_BITMAP)==bitmap&&GetBkMode(canvas.dc)==OPAQUE&&GetPolyFillMode(canvas.dc)==WINDING&&GetTextColor(canvas.dc)==RGB(220,30,150),
            "rendering preserves caller GDI objects and drawing state");
        SetBkMode(canvas.dc,savedBkMode);SetPolyFillMode(canvas.dc,savedFill);SetTextColor(canvas.dc,savedColor);
        const auto baseline=rendered(item.icon,dpi);
        require(baseline==rendered(item.icon,dpi,GetStockObject(ANSI_FIXED_FONT))&&baseline==rendered(item.icon,dpi,GetStockObject(SYSTEM_FONT)),
            "icon pixels are independent of the selected non-symbol font");

        canvas.clear();const auto unclipped=canvas.copy();const RECT clip{area.left+(area.right-area.left)/3,area.top+2,area.right-3,area.bottom-2};
        const int saved=SaveDC(canvas.dc);IntersectClipRect(canvas.dc,clip.left,clip.top,clip.right,clip.bottom);
        require(ui::drawIcon(canvas.dc,area,item.icon,foreground,dpi)&&outsideUntouched(canvas,unclipped,clip),"rendering obeys the caller's existing clipping region");RestoreDC(canvas.dc,saved);
    }
    checkStatePairs(dpi);
}
void contactSheet(UINT dpi,const fs::path& folder) {
    constexpr int columns=6,cellWidth=130,cellHeight=90;
    const int scale=static_cast<int>(dpi),rows=(static_cast<int>(icons.size())+columns-1)/columns;
    Canvas canvas(MulDiv(columns*cellWidth,scale,96),MulDiv(rows*cellHeight,scale,96));
    HFONT font=CreateFontW(-MulDiv(12,scale,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
    const auto old=SelectObject(canvas.dc,font);SetTextColor(canvas.dc,foreground);SetBkMode(canvas.dc,TRANSPARENT);
    for(std::size_t at=0;at<icons.size();++at) {
        const int x=static_cast<int>(at)%columns*cellWidth,y=static_cast<int>(at)/columns*cellHeight;
        RECT bounds{MulDiv(x+43,scale,96),MulDiv(y+8,scale,96),MulDiv(x+87,scale,96),MulDiv(y+52,scale,96)};
        require(ui::drawIcon(canvas.dc,bounds,icons[at].icon,at%3==0?RGB(155,140,255):foreground,dpi),"contact sheet icon renders");
        RECT caption{MulDiv(x+2,scale,96),MulDiv(y+59,scale,96),MulDiv(x+cellWidth-2,scale,96),MulDiv(y+80,scale,96)};
        DrawTextW(canvas.dc,icons[at].name,-1,&caption,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
    }
    SelectObject(canvas.dc,old);DeleteObject(font);
    require(canvas.save(folder/(L"ui-icons-"+std::to_wstring(dpi)+L".png")),"font-independent icon contact sheet saves as PNG");
}
void checkInvalidAndColor() {
    Canvas canvas(70,62);const RECT area{8,8,62,54};const auto before=canvas.copy();
    require(!ui::drawIcon(canvas.dc,area,Icon::none,foreground)&&canvas.copy()==before,"none is an unpainted text-only control");
    require(!ui::drawIcon(nullptr,area,Icon::play,foreground),"null destination is rejected");
    require(!ui::drawIcon(canvas.dc,RECT{20,20,20,30},Icon::play,foreground)&&canvas.copy()==before,"empty bounds are rejected without painting");
    require(!ui::drawIcon(canvas.dc,RECT{30,20,10,30},Icon::play,foreground)&&canvas.copy()==before,"inverted bounds are rejected without painting");
    require(ui::drawIcon(canvas.dc,area,Icon::play,RGB(230,30,40)),"requested colored icon renders");GdiFlush();
    std::uint64_t redIncrease=0,blueIncrease=0;
    for(std::size_t at=0;at<canvas.size();++at) {
        const auto rgb=canvas.pixels[at];redIncrease+=std::max(0,static_cast<int>((rgb>>16)&255)-23);blueIncrease+=std::max(0,static_cast<int>(rgb&255)-33);
    }
    require(redIncrease>blueIncrease*4&&redIncrease>0,"COLORREF foreground channels are preserved through alpha blending");
}
void checkRepeatedPaints() {
    Canvas canvas(72,64);const RECT area{8,8,64,56};
    for(const auto& item:icons)ui::drawIcon(canvas.dc,area,item.icon,foreground,144);
    GdiFlush();const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int loop=0;loop<100;++loop)for(const auto& item:icons) {
        canvas.clear();if(!ui::drawIcon(canvas.dc,area,item.icon,foreground,144))throw std::runtime_error("Repeated icon paint failed");
    }
    GdiFlush();const DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    std::cout<<"GDI resources before="<<before<<" after="<<after<<std::endl;
    require(before!=0&&after==before,"repeated icon paints retain a stable GDI object count");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(com))return 3;
    int result=0;
    try {
        const auto folder=fs::absolute(argv[1])/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));fs::create_directories(folder);
        checkInvalidAndColor();for(const UINT dpi:{96U,120U,144U,192U}) {checkEveryIcon(dpi);contactSheet(dpi,folder);}
        checkRepeatedPaints();std::cout<<"Native vector icon checks passed: "<<checks<<std::endl;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;result=1;}
    CoUninitialize();return result;
}
