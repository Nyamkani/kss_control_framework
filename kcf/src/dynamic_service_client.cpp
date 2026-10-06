#include "kcf/dynamic/dynamic_service_client.hpp"
#include <cerrno>
#include <new>
namespace kcf {
int DynamicServiceClient::Open(std::uint16_t port,std::uint16_t id,const TypeDescriptor& request,
                              const TypeDescriptor& response,std::uint32_t timeout,std::uint32_t retries) {
    if(opened_)return -EBUSY;
    if(!ValidateTypeDescriptor(request)||!ValidateTypeDescriptor(response))return -EINVAL;
    if(request.payload_size>SERVICE_MAX_PAYLOAD||response.payload_size>SERVICE_MAX_PAYLOAD)return -EMSGSIZE;
    const int result=client_.Create(port,timeout,retries);if(result)return result;
    request_identity_={request.type_id,detail::LayoutId(request)};
    response_identity_={response.type_id,detail::LayoutId(response)};
    request_size_=request.payload_size;response_size_=response.payload_size;service_id_=id;opened_=true;return 0;
}
int DynamicServiceClient::Call(const DynamicPayload& request,DynamicPayload& response) {
    if(!opened_)return -EBADF;
    if(request.type_id!=request_identity_.type_id)return -EPROTOTYPE;
    if(request.bytes.size()!=request_size_)return -EMSGSIZE;
    try{
        DynamicPayload value;value.type_id=response_identity_.type_id;value.bytes.resize(response_size_);
        const int result=client_.CallRaw(service_id_,request.bytes.data(),request_size_,value.bytes.data(),response_size_,request_identity_,response_identity_);
        if(result)return result;
        response=std::move(value);return 0;
    }catch(const std::bad_alloc&){return -ENOMEM;}catch(...){return -EIO;}
}
void DynamicServiceClient::Close(){client_.Close();opened_=false;}
}
