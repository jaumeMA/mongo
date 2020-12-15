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

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/config/namespace_serializer.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/future.h"
#include <boost/enable_shared_from_this.hpp>

namespace mongo {
namespace detail {

class IShardDdlOperationImpl : public std::enable_shared_from_this<IShardDdlOperationImpl> {
public:
    virtual ~IShardDdlOperationImpl() = default;

    virtual SemiFuture<void> run_impl(std::shared_ptr<executor::TaskExecutor>) const = 0;
    virtual StringData get_db() const = 0;
};

}  // namespace detail

class ShardingDdlOperation {
public:
    ShardingDdlOperation(std::shared_ptr<detail::IShardDdlOperationImpl> operationImpl);
    ShardingDdlOperation(const ShardingDdlOperation&) = delete;
    ShardingDdlOperation(ShardingDdlOperation&& other);

    ShardingDdlOperation& operator=(const ShardingDdlOperation&) = delete;

    SemiFuture<void> run(OperationContext* opCtx);

private:
    std::shared_ptr<detail::IShardDdlOperationImpl> _operationImpl;
};

template <typename T, typename... Args>
inline ShardingDdlOperation make_ddl_operation(Args&&... i_args) {
    static_assert(std::is_base_of<detail::IShardDdlOperationImpl, T>::value,
                  "Provided operation type shall implement IShardDdlOperationImpl interface");

    return {std::make_shared<T>(std::forward<Args>(i_args)...)};
}

}  // namespace mongo