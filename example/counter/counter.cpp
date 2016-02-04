/*
 * =====================================================================================
 *
 *       Filename:  counter.cpp
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  201510月23日 16时40分11秒
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  WangYao (fisherman), wangyao02@baidu.com
 *        Company:  Baidu, Inc
 *
 * =====================================================================================
 */

#include <gflags/gflags.h>
#include "baidu/rpc/closure_guard.h"
#include "counter.pb.h"
#include "counter.h"
#include "raft/protobuf_file.h"
#include "raft/storage.h"
#include "bthread_unstable.h"

namespace counter {

DEFINE_int64(max_duplicated_request_cache, 300000, "duplicated request cache maxsize");
DEFINE_bool(reject_duplicated_request, true, "reject duplicated request");

Counter::Counter(const raft::GroupId& group_id, const raft::PeerId& peer_id)
    : example::CommonStateMachine(group_id, peer_id), _value(0),
    _applied_index(0), _is_leader(false), 
    _duplicated_request_cache(FLAGS_max_duplicated_request_cache) {
}

Counter::~Counter() {
}

void Counter::fetch_and_add(int32_t ip, int32_t pid, int64_t req_id,
                            int64_t value, FetchAndAddDone* done) {
    FetchAndAddRequest request;
    request.set_ip(ip);
    request.set_pid(pid);
    request.set_req_id(req_id);
    request.set_value(value);
    base::IOBuf data;
    base::IOBufAsZeroCopyOutputStream wrapper(&data);
    request.SerializeToZeroCopyStream(&wrapper);
    raft::Task task;
    task.data = &data;
    task.done = done;
    _node.apply(task);
}

int Counter::get(int64_t* value_ptr, const int64_t index) {
    if (_is_leader.load(boost::memory_order_relaxed)) {
        *value_ptr = _value.load(boost::memory_order_relaxed);
        return 0;
    } else if (index <= _applied_index.load(boost::memory_order_acquire)) {
        *value_ptr = _value.load(boost::memory_order_relaxed);
        return 0;
    } else {
        return EPERM;
    }
}

void Counter::on_apply(const int64_t index, const raft::Task& task) {
    raft::Closure* done = task.done;
    baidu::rpc::ClosureGuard done_guard(done);

    FetchAndAddRequest request;
    base::IOBufAsZeroCopyInputStream wrapper(*task.data);
    request.ParseFromZeroCopyStream(&wrapper);
    if (FLAGS_reject_duplicated_request) {
        ClientRequestId client_req_id(request.ip(), request.pid(), request.req_id());
        CounterDuplicatedRequestCache::iterator iter = _duplicated_request_cache.Get(client_req_id);
        if (iter != _duplicated_request_cache.end()) {
            FetchAndAddResult& result = iter->second;
            LOG(WARNING) << "find duplicated request from cache, ip: " 
                << base::EndPoint(base::int2ip(request.ip()), 0)
                << " pid: " << request.pid() << " req_id: " << request.req_id() << " value: " 
                << request.value()
                << " return " << result.value << " at index: " << result.index;
            if (done) {
                ((FetchAndAddDone*)done)->set_result(result.value, result.index);
            }
            return;
        }
    }
    const int64_t prev_value = _value.load(boost::memory_order_relaxed);
    // fetch
    if (done) {
        //FetchAndAddDone* fetch_and_add_done = dynamic_cast<FetchAndAddDone*>(done);
        FetchAndAddDone* fetch_and_add_done = (FetchAndAddDone*)done;
        fetch_and_add_done->set_result(prev_value, index);
    }
    // add
    _value.store(prev_value + request.value(), boost::memory_order_relaxed);
    _applied_index.store(index, boost::memory_order_release);
    if (FLAGS_reject_duplicated_request) {
        ClientRequestId client_req_id(request.ip(), request.pid(), request.req_id());
        _duplicated_request_cache.Put(client_req_id, 
                FetchAndAddResult(prev_value + request.value(),index));
    }
    if (done) {
        return raft::run_closure_in_bthread_nosig(done_guard.release());
    }
}

void Counter::on_apply_in_batch(const int64_t first_index, 
                               const raft::Task tasks[],
                               size_t size) {
    for (size_t i = 0; i < size; ++i) {
        on_apply(first_index + i, tasks[i]);
    }
    bthread_flush();
}

void Counter::on_shutdown() {
    //TODO:
    LOG(ERROR) << "on_shutdown";
    exit(1);
    //delete this;
}

void Counter::on_snapshot_save(raft::SnapshotWriter* writer, raft::Closure* done) {
    baidu::rpc::ClosureGuard done_guard(done);
    std::string snapshot_path = raft::fileuri2path(writer->get_uri(base::EndPoint()));
    snapshot_path.append("/data");
    LOG(INFO) << "on_snapshot_save to " << snapshot_path;
    SnapshotInfo info;
    info.set_value(_value.load());
    raft::ProtoBufFile pb_file(snapshot_path);
    if (pb_file.save(&info, true) != 0)  {
        done->set_error(EIO, "Fail to save pb_file");
    }
}

int Counter::on_snapshot_load(raft::SnapshotReader* reader) {
    std::string snapshot_path = raft::fileuri2path(reader->get_uri(base::EndPoint()));
    snapshot_path.append("/data");

    LOG(INFO) << "on_snapshot_load from " << snapshot_path;
    raft::ProtoBufFile pb_file(snapshot_path);
    SnapshotInfo info;
    int ret = pb_file.load(&info);
    if (0 == ret) {
        _value.store(info.value());
    }
    return ret;
}

void Counter::on_leader_start() {
    LOG(INFO) << "on_leader_start, can accept get";
    _is_leader = true;
}

void Counter::on_leader_stop() {
    LOG(INFO) << "on_leader_stop, can't accept get";
    _is_leader = false;
}

}
