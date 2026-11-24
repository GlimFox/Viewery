#include "shell_selection.hpp"
#include <iostream>
#include <stdexcept>

void check(bool value,const char* label){if(!value)throw std::runtime_error(label);}
int main(){
    try{
        const auto groups=viewery::planExplorerSelection({L"C:\\Арты\\one.png",L"c:\\АРТЫ\\ONE.PNG",L"C:\\Арты\\two.gif",L"D:\\Other\\three.mp4",L"\\\\server\\media\\four.jpg"});
        check(groups.size()==3,"Selection is grouped by folder/volume/UNC location");
        check(groups[0].files.size()==2,"All selected files in one folder are revealed together");
        check(groups[1].files.size()==1&&groups[2].files.size()==1,"Different folders retain their exact selections");
        check(viewery::planExplorerSelection({}).empty(),"Empty selection does not open Explorer");
        bool rejected=false;try{viewery::planExplorerSelection({L"relative.png"});}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"Relative paths cannot change the target folder implicitly");
        std::cout<<"Explorer selection planning passed; no Shell windows were opened.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
