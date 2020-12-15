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

#include "mongo/db/s/sharding_ddl_drop_collection.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {

static constexpr int kMaxNumStaleShardVersionRetries = 10;

void sendDropCollectionToEveryShard(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    const auto shardsStatus =
        catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kMajorityReadConcern);
    uassertStatusOK(shardsStatus.getStatus());

    const std::vector<ShardType> allShards = std::move(shardsStatus.getValue().value);

    ShardsvrDropCollectionParticipant dropCollectionParticipant(nss);
    dropCollectionParticipant.setDbName(nss.db());

    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    for (const auto& shardEntry : allShards) {
        bool keepTrying;
        size_t numStaleShardVersionAttempts = 0;
        do {
            const auto& shard =
                uassertStatusOK(shardRegistry->getShard(opCtx, shardEntry.getName()));

            auto swDropResult = shard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                nss.db().toString(),
                CommandHelpers::appendMajorityWriteConcern(dropCollectionParticipant.toBSON({}),
                                                           opCtx->getWriteConcern()),
                Shard::RetryPolicy::kIdempotent);

            const std::string dropCollectionErrMsg = str::stream()
                << "Error dropping collection on shard " << shardEntry.getName();

            auto dropResult = uassertStatusOKWithContext(swDropResult, dropCollectionErrMsg);
            uassertStatusOKWithContext(dropResult.writeConcernStatus, dropCollectionErrMsg);

            auto dropCommandStatus = std::move(dropResult.commandStatus);

            if (dropCommandStatus.code() == ErrorCodes::NamespaceNotFound) {
                // The dropCollection command on the shard is not idempotent, and can return
                // NamespaceNotFound. We can ignore NamespaceNotFound since we have already asserted
                // that there is no writeConcern error.
                LOGV2(4620202,
                      "Namespace not found, {ns}",
                      "Namespace not found",
                      "ns"_attr = nss.ns());
                keepTrying = false;
            } else if (ErrorCodes::isStaleShardVersionError(dropCommandStatus.code())) {
                numStaleShardVersionAttempts++;
                if (numStaleShardVersionAttempts == kMaxNumStaleShardVersionRetries) {
                    uassertStatusOKWithContext(dropCommandStatus,
                                               str::stream() << dropCollectionErrMsg
                                                             << " due to exceeded retry attempts");
                }
                // No need to refresh cache, the command was sent with ChunkVersion::IGNORED and the
                // shard is allowed to throw, which means that the drop will serialize behind a
                // refresh.
                keepTrying = true;
            } else {
                uassertStatusOKWithContext(dropCommandStatus, dropCollectionErrMsg);
                keepTrying = false;
            }
        } while (keepTrying);
    }
}
void removeChunksAndTagsForDroppedCollection(OperationContext* opCtx, const NamespaceString& nss) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    ON_BLOCK_EXIT([opCtx, nss] {
        Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss);
    });

    // Remove chunk data
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             ChunkType::ConfigNS,
                                             BSON(ChunkType::ns(nss.ns())),
                                             ShardingCatalogClient::kMajorityWriteConcern));

    // Remove tag data
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             TagsType::ConfigNS,
                                             BSON(TagsType::ns(nss.ns())),
                                             ShardingCatalogClient::kMajorityWriteConcern));

    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             CollectionType::ConfigNS,
                                             BSON(CollectionType::kNssFieldName << nss.ns()),
                                             ShardingCatalogClient::kMajorityWriteConcern));
}

ShardingDdlDropCollection::DdlDropCollOperationContext::DdlDropCollOperationContext(
    ServiceContext* serviceContext, const NamespaceString& nss)
    : _client(serviceContext),
      _operationContext(_client->makeOperationContext()),
      _dbDistLock(uassertStatusOK(Grid::get(get())->catalogClient()->getDistLockManager()->lock(
          get(), nss.db(), "dropCollection", DistLockManager::kDefaultLockTimeout))),
      _collDistLock(uassertStatusOK(Grid::get(get())->catalogClient()->getDistLockManager()->lock(
          get(), nss.ns(), "dropCollection", DistLockManager::kDefaultLockTimeout))) {}
OperationContext* ShardingDdlDropCollection::DdlDropCollOperationContext::get() {
    return _operationContext.get();
}

ShardingDdlDropCollection::ShardingDdlDropCollection(OperationContext* opCtx,
                                                     const NamespaceString& nss)
    : _serviceContext(opCtx->getServiceContext()), _nss(nss) {
    uassert(
        ErrorCodes::InvalidOptions,
        str::stream() << "_shardsvrDropCollection must be called with majority writeConcern, got "
                      << opCtx->getWriteConcern().wMode,
        opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);
}
SemiFuture<void> ShardingDdlDropCollection::run_impl(
    std::shared_ptr<executor::TaskExecutor> executor) const {
    return ExecutorFuture<void>(executor, Status::OK())
        .then([this, self = shared_from_this()]() {
            DdlDropCollOperationContext opCtx(_serviceContext, _nss);

            // Send a drop collection command to every shard
            sendDropCollectionToEveryShard(opCtx.get(), _nss);

            // Remove in the config server all entries in config.tags, config.chunks and
            // config.collections
            removeChunksAndTagsForDroppedCollection(opCtx.get(), _nss);

            logd("JAUME: done");
        })
        .onError([](const Status& status) {
            LOGV2(4620201,
                  "Error running drop collection, caused by {error}",
                  "Error running drop collection",
                  "error"_attr = redact(status));
        })
        .semi();
}
StringData ShardingDdlDropCollection::get_db() const {
    return _nss.db();
}

}  // namespace mongo
