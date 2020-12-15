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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/drop_database_gen.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

class ShardsvrDropCollectionParticipantCommand final
    : public TypedCommand<ShardsvrDropCollectionParticipantCommand> {
public:
    bool acceptsAnyApiVersionParameters() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command, which is exported by secondary sharding servers. Do not call "
               "directly. Participates in droping a collection.";
    }

    using Request = ShardsvrDropCollectionParticipant;
    using Response = DropShardCollectionReply;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrDropCollectionParticipant can only be run on shard servers",
                    serverGlobalParams.clusterRole == ClusterRole::ShardServer);
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "_shardsvrDropCollectionParticipant must be called with "
                                     "majority writeConcern, got "
                                  << opCtx->getWriteConcern().wMode,
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            // Check shard version
            {
                AutoGetCollection db(opCtx, ns(), MODE_IS);
                auto* csr = CollectionShardingRuntime::get(opCtx, ns());
                csr->checkShardVersionOrThrow(opCtx);
            }

            BSONObjBuilder result;

            uassertStatusOK(
                dropCollection(opCtx,
                               ns(),
                               result,
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));

            // Delete entries in config.cache.collections

            // Delete entries in config.rangeDeletions

            // Delete csr entries
            {
                AutoGetCollection db(opCtx, ns(), MODE_IS);
                auto* csr = CollectionShardingRuntime::get(opCtx, ns());
                csr->clearFilteringMetadata(opCtx);
            }

            logd("JAUME: collection dropped on participant");

            return Response::parse(
                IDLParserErrorContext("_shardsvrDropCollectionParticipant-reply"), result.done());
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext*) const override {}

        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };
} sharsvrdDropCollectionParticipantCommand;

}  // namespace
}  // namespace mongo