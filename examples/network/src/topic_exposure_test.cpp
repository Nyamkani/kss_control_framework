#include "kcf/network/local_metadata.hpp"
#include "kcf/network/discovery_protocol.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include <cassert>
#include <iostream>
#include <memory>
#include <unistd.h>
namespace n=kcf::network;
void Policy() {
    n::MetadataConfig config;
    const auto check=[&](const std::string& name,n::EndpointRole role,n::NetworkScope expected){
        n::NetworkScope result=n::NetworkScope::REMOTE;
        assert(n::EvaluateTopicExposure(config,name,role,result)==0&&result==expected);
    };
    const auto provider=n::EndpointRole::PROVIDER,consumer=n::EndpointRole::CONSUMER;
    const auto local=n::NetworkScope::LOCAL,remote=n::NetworkScope::REMOTE;
    check("/robot/a",provider,local);
    config.remote_topics={"/explicit"};check("/explicit",provider,remote);check("/explicit",consumer,remote);
    config.topic_exposure={true,{"/robot/"},{"/robot/private"}};
    for(auto name:{"/robot","/robot/a","/robot/deep/b"})check(name,provider,remote);
    for(auto name:{"/robots/a","/robot2","/robot/private","/robot/private/x"})check(name,provider,local);
    check("/robot/a",consumer,local);
    config.remote_topics.push_back("/robot/private");check("/robot/private",provider,local);check("/robot/private",consumer,remote);
    config.topic_exposure.allow_prefixes.push_back("/robot/private");check("/robot/private",provider,local);
    config.topic_exposure.enabled=false;check("/robot/a",provider,local);check("/robot/private",provider,remote);
    for(const auto& bad:std::vector<std::string>{"","relative","//","/a//b","/a/./b","/a/../b","/a*","/a?","/a[b]","/a b",std::string("/a\0b",4),std::string(241,'/')}) {
        config.topic_exposure={true,{bad},{}};n::NetworkScope out=remote;
        assert(n::EvaluateTopicExposure(config,"/a",provider,out)==-EINVAL&&out==remote);
    }
    config.topic_exposure={true,{"/a","/a/"},{}};assert(n::ValidateTopicExposurePolicy(config.topic_exposure)==-EEXIST);
    config.topic_exposure={true,{"/a"},{"/b","/b/"}};assert(n::ValidateTopicExposurePolicy(config.topic_exposure)==-EEXIST);
    config.topic_exposure={true,{"/a"},{"/a"}};check("/a",provider,local);
    config.topic_exposure={true,{"/"},{}};check("/all/topics",provider,remote);check("/all/topics",consumer,local);
    config.topic_exposure.allow_prefixes.assign(65,"/a");assert(n::ValidateTopicExposurePolicy(config.topic_exposure)==-E2BIG);
    std::cout<<"Topic policy defaults/boundaries/publisher-only/deny precedence/validation PASS\n";
}
void Metadata() {
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    const std::string prefix="/policy_"+std::to_string(getpid());
    n::MetadataConfig config;config.host.bytes[0]=1;config.boot_id.bytes[0]=2;config.runtime_pids={getpid()};
    config.topic_exposure={true,{prefix},{prefix+"/private"}};
    std::vector<std::unique_ptr<kcf::Publisher<Value>>> pubs;
    const auto add=[&](const std::string& name){auto p=std::make_unique<kcf::Publisher<Value>>();assert(p->Create(name)==0);pubs.push_back(std::move(p));};
    add(prefix+"/value");add(prefix+"/private/value");
    kcf::Subscriber<Value> subscriber;assert(subscriber.Create(prefix+"/value",[](const Value&){})==0);
    kcf::Parameter<Value> parameter;assert(parameter.Create(prefix+"/parameter",{7,true,1.5})==0);
    std::vector<n::LocalEndpoint> rows;assert(n::CollectLocalMetadata(config,rows)==0&&rows.size()==4);
    unsigned exposed=0;
    for(const auto& e:rows)if(e.metadata.scope==n::NetworkScope::REMOTE){++exposed;assert(e.metadata.name==prefix+"/value"&&e.metadata.role==n::EndpointRole::PROVIDER&&e.metadata.kind==n::EndpointKind::TOPIC);}
    assert(exposed==1);
    add(prefix+"/new");assert(n::CollectLocalMetadata(config,rows)==0&&rows.size()==5);
    bool found=false;for(const auto& e:rows)if(e.metadata.name==prefix+"/new"){assert(e.metadata.scope==n::NetworkScope::REMOTE);found=true;}assert(found);
    assert(pubs.back()->Close()==0);assert(pubs.back()->Unlink()==0);pubs.pop_back();
    assert(n::CollectLocalMetadata(config,rows)==0&&rows.size()==4);
    // Exactly 64 is accepted, 65 fails without replacing the previous snapshot.
    for(unsigned i=0;i<60;++i)add(prefix+"/many/"+std::to_string(i));
    assert(n::CollectLocalMetadata(config,rows)==0&&rows.size()==n::DISCOVERY_MAX_ENDPOINTS);
    const auto first=rows.front().metadata.identity;
    add(prefix+"/overflow");assert(n::CollectLocalMetadata(config,rows)==-E2BIG);
    assert(rows.size()==64&&rows.front().metadata.identity==first);
    config.topic_exposure.allow_prefixes={"bad"};assert(n::CollectLocalMetadata(config,rows)==-EINVAL&&rows.size()==64);
    assert(subscriber.Close()==0);assert(parameter.Close()==0);assert(parameter.Unlink()==0);
    for(auto& p:pubs){assert(p->Close()==0);assert(p->Unlink()==0);}
    kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"Metadata dynamic add/remove, Subscriber/Parameter unchanged, 64/65 cap and atomic output PASS\n";
}
int main(int argc,char** argv){Policy();if(argc==2&&std::string(argv[1])=="--metadata")Metadata();else assert(argc==1);}
