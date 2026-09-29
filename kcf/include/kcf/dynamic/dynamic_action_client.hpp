#pragma once
#include "kcf/introspection/action_info.hpp"
#include "kcf/introspection/type_descriptor.hpp"
#include "kcf/dynamic/dynamic_payload.hpp"
#include "kcf/service/service_client.hpp"
#include <memory>
namespace kcf {
// Observational adapter to the existing Local Action Service/Topic protocol.
// Calls are caller-serialized. Close never cancels the application goal.
class DynamicActionClient {
public:
    DynamicActionClient();~DynamicActionClient();
    int Open(const ActionInfo&,const TypeDescriptor& goal,const TypeDescriptor& feedback,
             const TypeDescriptor& result,std::int32_t owner,std::uint64_t ticks,std::uint32_t timeout_ms=200);
    int SendGoal(const DynamicPayload&,std::uint64_t& goal_id,bool* execution_unknown=nullptr);
    int Cancel(std::uint64_t goal_id,bool* cancel_unknown=nullptr);
    int ReadFeedback(ActionHeader&,DynamicPayload&);
    int GetResult(std::uint64_t goal_id,ActionState&,DynamicPayload&);
    void Close();
private:
    struct Storage;std::unique_ptr<Storage> storage_;
    ServiceClient client_;ActionInfo info_{};TypeDescriptor goal_,feedback_,result_;
};
}
