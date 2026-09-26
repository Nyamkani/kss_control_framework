// Calls the existing Tool backend, not a replacement decoder or GUI.
#include "kcf_tool/backend/kcf_backend.hpp"
#include <iomanip>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    const std::string prefix=argv[1];
    kcf_tool::KcfBackend backend;
    if(backend.Refresh()!=0) return 1;
    for(const auto& a:backend.GetApplications())
        std::cout<<"A "<<std::quoted(a.name)<<' '<<a.state<<' '<<a.managed_element_count<<'\n';
    for(const auto& e:backend.GetElements())
        std::cout<<"R "<<e.pid<<' '<<std::quoted(e.executable)<<' '<<e.state<<' '<<e.mode<<'\n';
    for(const auto& t:backend.GetTopics()) {
        if(t.name.rfind(prefix+"/",0)!=0 || t.role!="PUBLISHER") continue;
        kcf_tool::DataSnapshot data;
        const int opened=backend.StartTopicEcho(t.identity);
        const int result=opened ? opened : backend.ReadTopicEcho(data);
        backend.StopTopicEcho();
        std::cout<<"T "<<std::quoted(t.name)<<' '<<result<<' '<<std::quoted(data.type_name)<<' '<<data.sequence<<' '<<data.fields.size()<<' '<<t.identity.runtime.pid<<'\n';
        for(const auto& f:data.fields) {
            if(f.array_values.empty())
                std::cout<<"F "<<std::quoted(t.name)<<' '<<std::quoted(f.name)<<' '<<std::quoted(f.value)<<'\n';
            else {
                std::cout<<"V "<<std::quoted(t.name)<<' '<<std::quoted(f.name);
                for(const auto& value:f.array_values) std::cout<<' '<<std::quoted(value);
                std::cout<<'\n';
            }
        }
    }
}
