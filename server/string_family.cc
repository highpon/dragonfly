// Copyright 2021, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/string_family.h"

#include <absl/container/inlined_vector.h>

#include "base/logging.h"
#include "server/command_registry.h"
#include "server/conn_context.h"
#include "server/engine_shard_set.h"
#include "server/error.h"
#include "server/transaction.h"
#include "util/varz.h"

namespace dfly {

namespace {

using namespace std;

using CI = CommandId;
DEFINE_VARZ(VarzQps, set_qps);
DEFINE_VARZ(VarzQps, get_qps);

}  // namespace

SetCmd::SetCmd(DbSlice* db_slice) : db_slice_(db_slice) {
}

SetCmd::~SetCmd() {
}

OpResult<void> SetCmd::Set(const SetParams& params, std::string_view key, std::string_view value) {
  DCHECK_LT(params.db_index, db_slice_->db_array_size());
  DCHECK(db_slice_->IsDbValid(params.db_index));

  VLOG(2) << "Set (" << db_slice_->shard_id() << ") ";

  auto [it, expire_it] = db_slice_->FindExt(params.db_index, key);
  uint64_t at_ms = params.expire_after_ms ? params.expire_after_ms + db_slice_->Now() : 0;

  if (it != MainIterator{}) {  // existing
    if (params.how == SET_IF_NOTEXIST)
      return OpStatus::SKIPPED;

    if (params.prev_val) {
      params.prev_val->emplace(it->second.str);
    }

    return SetExisting(params.db_index, value, at_ms, it, expire_it);
  }

  if (params.how == SET_IF_EXISTS)
    return OpStatus::SKIPPED;

  db_slice_->AddNew(params.db_index, key, value, at_ms);

  return OpStatus::OK;
}

OpResult<void> SetCmd::SetExisting(DbIndex db_ind, std::string_view value, uint64_t expire_at_ms,
                                   MainIterator dest, ExpireIterator exp_it) {
  if (exp_it != ExpireIterator{} && expire_at_ms) {
    exp_it->second = expire_at_ms;
  } else {
    db_slice_->Expire(db_ind, dest, expire_at_ms);
  }

  dest->second = value;

  return OpStatus::OK;
}

void StringFamily::Set(CmdArgList args, ConnectionContext* cntx) {
  set_qps.Inc();

  std::string_view key = ArgS(args, 1);
  std::string_view value = ArgS(args, 2);
  VLOG(2) << "Set " << key << " " << value;

  SetCmd::SetParams sparams{cntx->db_index()};  // TODO: db_index.
  int64_t int_arg;

  for (size_t i = 3; i < args.size(); ++i) {
    ToUpper(&args[i]);

    std::string_view cur_arg = ArgS(args, i);

    if (cur_arg == "EX" || cur_arg == "PX") {
      bool is_ms = (cur_arg == "PX");
      ++i;
      if (i == args.size()) {
        cntx->SendError(kSyntaxErr);
      }
      std::string_view ex = ArgS(args, i);
      if (!absl::SimpleAtoi(ex, &int_arg)) {
        return cntx->SendError(kInvalidIntErr);
      }
      if (int_arg <= 0 || (!is_ms && int_arg >= 500000000)) {
        return cntx->SendError("invalid expire time in set");
      }
      if (!is_ms) {
        int_arg *= 1000;
      }
      sparams.expire_after_ms = int_arg;
    } else if (cur_arg == "NX") {
      sparams.how = SetCmd::SET_IF_NOTEXIST;
    } else if (cur_arg == "XX") {
      sparams.how = SetCmd::SET_IF_EXISTS;
    } else if (cur_arg == "KEEPTTL") {
      sparams.keep_expire = true;
    } else {
      return cntx->SendError(kSyntaxErr);
    }
  }

  DCHECK(cntx->transaction);

  auto cb = [&](Transaction* t, EngineShard* shard) {
    SetCmd sg(&shard->db_slice());
    auto status = sg.Set(sparams, key, value).status();
    return status;
  };
  OpResult<void> result = cntx->transaction->ScheduleSingleHop(std::move(cb));

  if (result == OpStatus::OK) {
    return cntx->SendStored();
  }

  CHECK_EQ(result, OpStatus::SKIPPED);  // in case of NX option
  return cntx->SendNull();
}

void StringFamily::Get(CmdArgList args, ConnectionContext* cntx) {
  get_qps.Inc();

  std::string_view key = ArgS(args, 1);

  auto cb = [&](Transaction* t, EngineShard* shard) -> OpResult<string> {
    OpResult<MainIterator> it_res = shard->db_slice().Find(cntx->db_index(), key);
    if (!it_res.ok())
      return it_res.status();

    string val = it_res.value()->second.str;

    return val;
  };

  DVLOG(1) << "Before Get::ScheduleSingleHopT " << key;
  Transaction* trans = cntx->transaction;
  OpResult<string> result = trans->ScheduleSingleHopT(std::move(cb));

  if (result) {
    DVLOG(1) << "GET " << trans->DebugId() << ": " << key << " " << result.value();
    cntx->SendGetReply(key, 0, result.value());
  } else {
    DVLOG(1) << "GET " << key << " nil";
    cntx->SendGetNotFound();
  }
}

void StringFamily::GetSet(CmdArgList args, ConnectionContext* cntx) {
  std::string_view key = ArgS(args, 1);
  std::string_view value = ArgS(args, 2);
  std::optional<string> prev_val;

  SetCmd::SetParams sparams{cntx->db_index()};
  sparams.prev_val = &prev_val;

  ShardId sid = Shard(key, cntx->shard_set->size());
  OpResult<void> result = cntx->shard_set->Await(sid, [&] {
    EngineShard* es = EngineShard::tlocal();
    SetCmd cmd(&es->db_slice());

    return cmd.Set(sparams, key, value);
  });

  if (!result) {
    cntx->SendError(result.status());
    return;
  }

  if (prev_val) {
    cntx->SendGetReply(key, 0, *prev_val);
    return;
  }
  return cntx->SendNull();
}

void StringFamily::MGet(CmdArgList args, ConnectionContext* cntx) {
  DCHECK_GT(args.size(), 1U);

  Transaction* transaction = cntx->transaction;
  unsigned shard_count = transaction->shard_set()->size();
  std::vector<MGetResponse> mget_resp(shard_count);

  auto cb = [&](Transaction* t, EngineShard* shard) {
    ShardId sid = shard->shard_id();
    mget_resp[sid] = OpMGet(t, shard);
    return OpStatus::OK;
  };

  // MGet requires locking as well. For example, if coordinator A applied W(x) and then W(y)
  // it necessarily means that whoever observed y, must observe x.
  // Without locking, mget x y could read stale x but latest y.
  OpStatus result = transaction->ScheduleSingleHop(std::move(cb));
  CHECK_EQ(OpStatus::OK, result);

  // reorder the responses back according to the order of their corresponding keys.
  vector<std::optional<std::string_view>> res(args.size() - 1);
  for (ShardId sid = 0; sid < shard_count; ++sid) {
    if (!transaction->IsActive(sid))
      continue;
    auto& values = mget_resp[sid];
    ArgSlice slice = transaction->ShardArgsInShard(sid);
    DCHECK(!slice.empty());
    DCHECK_EQ(slice.size(), values.size());
    for (size_t j = 0; j < slice.size(); ++j) {
      uint32_t indx = transaction->ReverseArgIndex(sid, j);
      res[indx] = values[j];
    }
  }

  return cntx->SendMGetResponse(res.data(), res.size());
}

void StringFamily::MSet(CmdArgList args, ConnectionContext* cntx) {
  Transaction* transaction = cntx->transaction;

  if (VLOG_IS_ON(2)) {
    string str;
    for (size_t i = 1; i < args.size(); ++i) {
      absl::StrAppend(&str, " ", ArgS(args, i));
    }
    LOG(INFO) << "MSET/" << transaction->unique_shard_cnt() << str;
  }

  OpStatus status = transaction->ScheduleSingleHop(&OpMSet);
  CHECK_EQ(OpStatus::OK, status);

  DVLOG(2) << "MSet run  " << transaction->DebugId();

  return cntx->SendOk();
}

auto StringFamily::OpMGet(const Transaction* t, EngineShard* shard) -> MGetResponse {
  auto args = t->ShardArgsInShard(shard->shard_id());
  DCHECK(!args.empty());

  MGetResponse response(args.size());

  auto& db_slice = shard->db_slice();
  for (size_t i = 0; i < args.size(); ++i) {
    OpResult<MainIterator> de_res = db_slice.Find(0, args[i]);
    if (de_res.ok()) {
      response[i] = de_res.value()->second.str;
    }
  }

  return response;
}

OpStatus StringFamily::OpMSet(const Transaction* t, EngineShard* es) {
  ArgSlice largs = t->ShardArgsInShard(es->shard_id());
  CHECK(!largs.empty() && largs.size() % 2 == 0);

  SetCmd::SetParams params{0};
  SetCmd sg(&es->db_slice());
  for (size_t i = 0; i < largs.size(); i += 2) {
    DVLOG(1) << "MSet " << largs[i] << ":" << largs[i + 1];
    auto res = sg.Set(params, largs[i], largs[i + 1]);
    CHECK(res.ok()) << res << " " << largs[i];  // TODO - handle OOM etc.
  }

  return OpStatus::OK;
}

void StringFamily::Init(util::ProactorPool* pp) {
  set_qps.Init(pp);
  get_qps.Init(pp);
}

void StringFamily::Shutdown() {
  set_qps.Shutdown();
  get_qps.Shutdown();
}

#define HFUNC(x) SetHandler(&StringFamily::x)

void StringFamily::Register(CommandRegistry* registry) {
  *registry << CI{"SET", CO::WRITE | CO::DENYOOM, -3, 1, 1, 1}.HFUNC(Set)
            << CI{"GET", CO::READONLY | CO::FAST, 2, 1, 1, 1}.HFUNC(Get)
            << CI{"GETSET", CO::WRITE | CO::DENYOOM | CO::FAST, 3, 1, 1, 1}.HFUNC(GetSet)
            << CI{"MGET", CO::READONLY | CO::FAST, -2, 1, -1, 1}.HFUNC(MGet)
            << CI{"MSET", CO::WRITE | CO::DENYOOM, -3, 1, -1, 2}.HFUNC(MSet);
}

}  // namespace dfly
