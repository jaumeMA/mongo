/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/sharding_ddl_operation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

namespace mongo {

ShardingDdlOperation::ShardingDdlOperation(
    std::shared_ptr<detail::IShardDdlOperationImpl> operationImpl)
    : _operationImpl(std::move(operationImpl)) {}
ShardingDdlOperation::ShardingDdlOperation(ShardingDdlOperation&& other)
    : _operationImpl(std::move(other._operationImpl)) {}
SemiFuture<void> ShardingDdlOperation::run(OperationContext* opCtx) {
    invariant(_operationImpl);

    const auto dbInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, _operationImpl->get_db()));
    auto shardId = ShardingState::get(opCtx)->shardId();
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "this is not the primary shard for db " << _operationImpl->get_db()
                          << " expected: " << dbInfo.primaryId() << " shardId: " << shardId,
            dbInfo.primaryId() == shardId);
    {
        // Check DB version of incoming command with our cached version.
        // This local lock should be avoided by serializer
        Lock::DBLock dbWriteLock(opCtx, _operationImpl->get_db(), MODE_IX);
        auto dss = DatabaseShardingState::get(opCtx, _operationImpl->get_db());
        auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
        dss->checkDbVersion(opCtx, dssLock);
    }

    return _operationImpl->run_impl(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());
}

}  // namespace mongo