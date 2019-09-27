/* Copyright (c) 2019 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "base/Base.h"
#include "FindPathExecutor.h"

namespace nebula {
namespace graph {

FindPathExecutor::FindPathExecutor(Sentence *sentence, ExecutionContext *exct)
        : TraverseExecutor(exct), barrier_(3) {
    sentence_ = static_cast<FindPathSentence*>(sentence);
}

Status FindPathExecutor::prepare() {
    Status status;
    expCtx_ = std::make_unique<ExpressionContext>();
    do {
        if (sentence_->from() != nullptr) {
            status = sentence_->from()->prepare(from_);
            if (!status.ok()) {
                break;
            }
        }
        if (sentence_->to() != nullptr) {
            status = sentence_->to()->prepare(to_);
            if (!status.ok()) {
                break;
            }
        }
        if (sentence_->over() != nullptr) {
            status = sentence_->over()->prepare(over_);
            if (!status.ok()) {
                break;
            }
        }
        if (sentence_->step() != nullptr) {
            status = sentence_->step()->prepare(step_);
            if (!status.ok()) {
                break;
            }
        }
        if (sentence_->where() != nullptr) {
            status = sentence_->where()->prepare(where_);
            if (!status.ok()) {
                break;
            }
        }
        shortest_ = sentence_->isShortest();
    } while (false);
    return status;
}

Status FindPathExecutor::beforeExecute() {
    Status status;
    do {
        status = checkIfGraphSpaceChosen();
        if (!status.ok()) {
            break;;
        }
        spaceId_ = ectx()->rctx()->session()->space();

        status = prepareOver();
        if (!status.ok()) {
            break;
        }

        status = setupVids();
        if (!status.ok()) {
            break;
        }
    } while (false);
    return status;
}

Status FindPathExecutor::prepareOverAll() {
    auto spaceId = ectx()->rctx()->session()->space();
    auto edgeAllStatus = ectx()->schemaManager()->getAllEdge(spaceId);

    if (!edgeAllStatus.ok()) {
        return edgeAllStatus.status();
    }

    auto allEdge = edgeAllStatus.value();
    for (auto &e : allEdge) {
        auto edgeStatus = ectx()->schemaManager()->toEdgeType(spaceId, e);
        if (!edgeStatus.ok()) {
            return edgeStatus.status();
        }

        auto v = edgeStatus.value();
        over_.edgeTypes_.emplace_back(v);
        over_.oppositeTypes_.emplace_back(-v);

        if (!expCtx_->addEdge(e, v)) {
            return Status::Error(folly::sformat("edge alias({}) was dup", e));
        }
        edgeTypeNameMap_.emplace(v, e);
    }

    return Status::OK();
}

Status FindPathExecutor::prepareOver() {
    Status status = Status::OK();

    for (auto e : over_.edges_) {
        if (e->isOverAll()) {
            expCtx_->setOverAllEdge();
            return prepareOverAll();
        }

        auto spaceId = ectx()->rctx()->session()->space();
        auto edgeStatus = ectx()->schemaManager()->toEdgeType(spaceId, *e->edge());
        if (!edgeStatus.ok()) {
            return edgeStatus.status();
        }

        auto v = edgeStatus.value();
        if (e->isReversely()) {
            over_.edgeTypes_.emplace_back(-v);
            over_.oppositeTypes_.emplace_back(v);
        } else {
            over_.edgeTypes_.emplace_back(v);
            over_.oppositeTypes_.emplace_back(-v);
        }

        if (e->alias() != nullptr) {
            if (!expCtx_->addEdge(*e->alias(), v)) {
                return Status::Error(folly::sformat("edge alias({}) was dup", *e->alias()));
            }
        } else {
            if (!expCtx_->addEdge(*e->edge(), v)) {
                return Status::Error(folly::sformat("edge alias({}) was dup", *e->edge()));
            }
        }

        edgeTypeNameMap_.emplace(v, *e->edge());
    }

    return status;
}

void FindPathExecutor::execute() {
    DCHECK(onError_);
    DCHECK(onFinish_);

    auto status = beforeExecute();
    if (!status.ok()) {
        onError_(std::move(status));
        return;
    }

    auto steps = step_.steps_ / 2 + step_.steps_ % 2;
    fromVids_ = from_.vids_;
    toVids_ = to_.vids_;
    visitedFrom_.insert(fromVids_.begin(), fromVids_.end());
    visitedTo_.insert(toVids_.begin(), toVids_.end());
    targetNotFound_.insert(toVids_.begin(), toVids_.end());
    for (auto &v : fromVids_) {
        Path path;
        pathFrom_.emplace(v, std::move(path));
    }
    for (auto &v : toVids_) {
        Path path;
        pathTo_.emplace(v, std::move(path));
    }

    while (currentStep_ < steps) {
        if (shortest_ && targetNotFound_.empty()) {
            break;
        }
        auto props = getStepOutProps(false);
        if (!props.ok()) {
            onError_(std::move(props).status());
            return;
        }
        getFromFrontiers(std::move(props).value());

        props = getStepOutProps(true);
        if (!props.ok()) {
            onError_(std::move(props).status());
            return;
        }
        getToFrontiers(std::move(props).value());
        barrier_.wait();

        findPath();

        VLOG(1) << "Current step:" << currentStep_;
        ++currentStep_;;
    }
    onFinish_();
}

void FindPathExecutor::findPath() {
    visitedFrom_.clear();
    std::multimap<VertexID, Path> pathF;
    for (auto &frontier : fromFrontiers_.second) {
        // Notice: we treat edges with different ranking
        // between two vertices as different path
        for (auto &neighbor : frontier.second) {
            auto dstId = std::get<0>(neighbor);
            VLOG(2) << "src vertex:" << frontier.first;
            VLOG(2) << "dst vertex:" << dstId;
            // if frontiers of F are neighbors of visitedByT,
            // we found an odd path
            if (visitedTo_.count(dstId) == 1) {
                meetOddPath(frontier.first, dstId, neighbor);
            }

            // update the path to frontiers
            updatePath(frontier.first, pathFrom_, neighbor, pathF, VisitedBy::FROM);
            visitedFrom_.emplace(dstId);
        }  // for `frontier'
    }  // for `neighbor'
    pathFrom_ = std::move(pathF);
    fromVids_.reserve(visitedFrom_.size());
    std::copy(visitedFrom_.begin(),
              visitedFrom_.end(), std::back_inserter(fromVids_));

    visitedTo_.clear();
    std::multimap<VertexID, Path> pathT;
    for (auto &frontier : toFrontiers_.second) {
        for (auto &neighbor : frontier.second) {
            auto dstId = std::get<0>(neighbor);
            // update the path to frontiers
            updatePath(frontier.first, pathTo_, neighbor, pathT, VisitedBy::TO);
            visitedTo_.emplace(dstId);
        }  // for `frontier'
    }  // for `neighbor'
    pathTo_ = std::move(pathT);
    toVids_.reserve(visitedTo_.size());
    std::copy(visitedTo_.begin(),
              visitedTo_.end(), std::back_inserter(toVids_));

    std::sort(fromVids_.begin(), fromVids_.end());
    std::sort(toVids_.begin(), toVids_.end());
    std::set<VertexID> intersect;
    std::set_intersection(fromVids_.begin(), fromVids_.end(),
                          toVids_.begin(), toVids_.end(),
                          std::inserter(intersect, intersect.end()));
    // if frontiersF meets frontiersT, we found an even path
    if (!intersect.empty()) {
        if (shortest_ && targetNotFound_.empty()) {
            return;
        }
        for (auto intersectId : intersect) {
            meetEvenPath(intersectId);
        }  // `intersectId'
    }
}

inline void FindPathExecutor::meetOddPath(VertexID src, VertexID dst, Neighbor &neighbor) {
    auto rangeF = pathFrom_.equal_range(src);
    for (auto i = rangeF.first; i != rangeF.second; ++i) {
        auto rangeT = pathTo_.equal_range(dst);
        for (auto j = rangeT.first; j != rangeT.second; ++j) {
            if (j->second.size() + i->second.size() > step_.steps_) {
                continue;
            }
            // Build path:
            // i->second + (src,type,ranking) + (dst, -type, ranking) + j->second
            Path path = i->second;
            auto s0 = std::make_unique<StepOut>(neighbor);
            std::get<0>(*s0) = src;
            path.emplace_back(s0.get());
            stepOutHolder_.emplace(std::move(s0));
            VLOG(1) << "PathF: " << buildPathString(path);

            auto s1 = std::make_unique<StepOut>(neighbor);
            std::get<1>(*s1) = - std::get<1>(neighbor);
            path.emplace_back(s1.get());
            stepOutHolder_.emplace(std::move(s1));
            path.insert(path.end(), j->second.begin(), j->second.end());
            VLOG(2) << "PathT: " << buildPathString(j->second);

            auto target = std::get<0>(*(path.back()));
            if (shortest_) {
                targetNotFound_.erase(target);
                if (finalPath_.count(target) > 0) {
                    // already found a shorter path
                    continue;
                } else {
                    finalPath_.emplace(target, std::move(path));
                }
            } else {
                VLOG(2) << "Found path: " << buildPathString(path);
                finalPath_.emplace(target, std::move(path));
            }
        }  // for `j'
    }  // for `i'
}

inline void FindPathExecutor::meetEvenPath(VertexID intersectId) {
    auto rangeF = pathFrom_.equal_range(intersectId);
    auto rangeT = pathTo_.equal_range(intersectId);
    for (auto i = rangeF.first; i != rangeF.second; ++i) {
        for (auto j = rangeT.first; j != rangeT.second; ++j) {
            if (j->second.size() + i->second.size() > step_.steps_) {
                continue;
            }
            // Build path:
            // i->second + (src,type,ranking) + j->second
            Path path = i->second;
            VLOG(2) << "PathF: " << buildPathString(path);
            if (j->second.size() > 0) {
                StepOut *s = j->second.front();
                auto s0 = std::make_unique<StepOut>(*s);
                std::get<0>(*s0) = intersectId;
                std::get<1>(*s0) = - std::get<1>(*s0);
                path.emplace_back(s0.get());
                stepOutHolder_.emplace(std::move(s0));
                VLOG(2) << "Joiner: " << buildPathString(path);
            } else if (i->second.size() > 0) {
                StepOut *s = i->second.back();
                auto s0 = std::make_unique<StepOut>(*s);
                std::get<0>(*s0) = intersectId;
                path.emplace_back(s0.get());
                stepOutHolder_.emplace(std::move(s0));
                VLOG(2) << "Joiner: " << buildPathString(path);
            }
            VLOG(2) << "PathT: " << buildPathString(j->second);
            path.insert(path.end(), j->second.begin(), j->second.end());
            auto target = std::get<0>(*(path.back()));
            if (shortest_) {
                if (finalPath_.count(target) > 0) {
                    // already found a shorter path
                    continue;
                } else {
                    targetNotFound_.erase(target);
                    finalPath_.emplace(target, std::move(path));
                }
            } else {
                VLOG(2) << "Found path: " << buildPathString(path);
                finalPath_.emplace(target, std::move(path));
            }
        }
    }
}

inline void FindPathExecutor::updatePath(
            VertexID &src,
            std::multimap<VertexID, Path> &pathToSrc,
            Neighbor &neighbor,
            std::multimap<VertexID, Path> &pathToNeighbor,
            VisitedBy visitedBy) {
    auto range = pathToSrc.equal_range(src);
    for (auto i = range.first; i != range.second; ++i) {
        // Build path:
        // i->second + (src,type,ranking)
        Path path = i->second;
        VLOG(2) << "Interim path before :" << buildPathString(path);
        VLOG(2) << "Interim path length before:" << path.size();
        auto s = std::make_unique<StepOut>(neighbor);
        std::get<0>(*s) = src;
        if (visitedBy == VisitedBy::FROM) {
            path.emplace_back(s.get());
        } else {
            path.emplace(path.begin(), s.get());
        }
        VLOG(2) << "Neighbor: " << std::get<0>(neighbor);
        VLOG(2) << "Interim path:" << buildPathString(path);
        VLOG(2) << "Interim path length:" << path.size();
        stepOutHolder_.emplace(std::move(s));
        pathToNeighbor.emplace(std::get<0>(neighbor), std::move(path));
    }  // for `i'
}

Status FindPathExecutor::setupVids() {
    Status status = Status::OK();
    do {
        if (sentence_->from()->isRef()) {
            status = setupVidsFromRef(from_);
            if (!status.ok()) {
                break;
            }
        }

        if (sentence_->to()->isRef()) {
            status = setupVidsFromRef(to_);
            if (!status.ok()) {
                break;
            }
        }
    } while (false);

    return status;
}

Status FindPathExecutor::setupVidsFromRef(Clause::Vertices &vertices) {
    const InterimResult *inputs;
    if (vertices.varname_ == nullptr) {
        inputs = inputs_.get();
        if (inputs == nullptr) {
            return Status::OK();
        }
    } else {
        inputs = ectx()->variableHolder()->get(*(vertices.varname_));
        if (inputs == nullptr) {
            return Status::Error("Variable `%s' not defined", vertices.varname_->c_str());
        }
    }

    auto result = inputs->getDistinctVIDs(*(vertices.colname_));
    if (!result.ok()) {
        return std::move(result).status();
    }
    vertices.vids_ = std::move(result).value();
    return Status::OK();
}

void FindPathExecutor::getFromFrontiers(
        std::vector<storage::cpp2::PropDef> props) {
    auto future = ectx()->storage()->getNeighbors(spaceId_,
                                                  std::move(fromVids_),
                                                  over_.edgeTypes_,
                                                  "",
                                                  std::move(props));
    auto *runner = ectx()->rctx()->runner();
    auto cb = [this] (auto &&result) {
        Frontiers frontiers;
        auto completeness = result.completeness();
        if (completeness == 0) {
            fStatus_ = Status::Error("Get neighbors failed.");
            barrier_.wait();
            return;
        } else if (completeness != 100) {
            LOG(INFO) << "Get neighbors partially failed: "  << completeness << "%";
            for (auto &error : result.failedParts()) {
                LOG(ERROR) << "part: " << error.first
                           << "error code: " << static_cast<int>(error.second);
            }
        }
        auto status = doFilter(std::move(result), where_.filter_, true, frontiers);
        if (!status.ok()) {
            fStatus_ = std::move(status);
            barrier_.wait();
            return;
        }
        fromFrontiers_ = std::make_pair(VisitedBy::FROM, std::move(frontiers));
        barrier_.wait();
    };
    std::move(future).via(runner, folly::Executor::HI_PRI).thenValue(cb);
}

void FindPathExecutor::getToFrontiers(
        std::vector<storage::cpp2::PropDef> props) {
    auto future = ectx()->storage()->getNeighbors(spaceId_,
                                                  std::move(toVids_),
                                                  over_.oppositeTypes_,
                                                  "",
                                                  std::move(props));
    auto *runner = ectx()->rctx()->runner();
    auto cb = [this] (auto &&result) {
        Frontiers frontiers;
        auto completeness = result.completeness();
        if (completeness == 0) {
            tStatus_ = Status::Error("Get neighbors failed.");
            barrier_.wait();
            return;
        } else if (completeness != 100) {
            LOG(INFO) << "Get neighbors partially failed: "  << completeness << "%";
            for (auto &error : result.failedParts()) {
                LOG(ERROR) << "part: " << error.first
                           << "error code: " << static_cast<int>(error.second);
            }
        }
        auto status = doFilter(std::move(result), where_.filter_, false, frontiers);
        if (!status.ok()) {
            tStatus_ = std::move(status);
            barrier_.wait();
            return;
        }
        toFrontiers_ = std::make_pair(VisitedBy::TO, std::move(frontiers));
        barrier_.wait();
    };
    std::move(future).via(runner, folly::Executor::HI_PRI).thenValue(cb);
}

Status FindPathExecutor::doFilter(
        storage::StorageRpcResponse<storage::cpp2::QueryResponse> &&result,
        Expression *filter,
        bool isOutBound,
        Frontiers &frontiers) {
    UNUSED(filter);
    UNUSED(isOutBound);

    auto &resps = result.responses();
    for (auto &resp : resps) {
        if (resp.get_vertices() == nullptr) {
            continue;
        }

        std::unordered_map<EdgeType, std::shared_ptr<ResultSchemaProvider>> edgeSchema;
        auto *eschema = resp.get_edge_schema();
        if (eschema != nullptr) {
            std::transform(eschema->cbegin(), eschema->cend(),
                           std::inserter(edgeSchema, edgeSchema.begin()), [](auto &schema) {
                               return std::make_pair(
                                   schema.first,
                                   std::make_shared<ResultSchemaProvider>(schema.second));
                           });
        }

        if (edgeSchema.empty()) {
            continue;
        }

        for (auto &vdata : resp.vertices) {
            DCHECK(vdata.__isset.edge_data);
            for (auto &edata : vdata.edge_data) {
                auto edgeType = edata.type;
                auto it = edgeSchema.find(edgeType);
                DCHECK(it != edgeSchema.end());
                RowSetReader rsReader(it->second, edata.data);
                auto iter = rsReader.begin();
                Neighbors neighbors;
                while (iter) {
                    std::vector<VariantType> temps;
                    for (auto &prop : kReserveProps_) {
                        auto res = RowReader::getPropByName(&*iter, prop);
                        if (ok(res)) {
                            temps.emplace_back(std::move(value(res)));
                        } else {
                            return Status::Error("get edge prop failed %s", prop.c_str());
                        }
                    }
                    Neighbor neighbor(
                        boost::get<int64_t>(temps[0]),
                        boost::get<int64_t>(temps[1]),
                        boost::get<int64_t>(temps[2]));
                    neighbors.emplace_back(std::move(neighbor));
                    ++iter;
                }  // while `iter'
                auto frontier = std::make_pair(vdata.get_vertex_id(), std::move(neighbors));
                frontiers.emplace_back(std::move(frontier));
            }  // `edata'
        }  // for `vdata'
    }  // for `resp'
    return Status::OK();
}

StatusOr<std::vector<storage::cpp2::PropDef>>
FindPathExecutor::getStepOutProps(bool reversely) {
    auto *edges = &over_.edgeTypes_;
    if (reversely) {
        edges = &over_.oppositeTypes_;
    }
    std::vector<storage::cpp2::PropDef> props;
    for (auto &e : *edges) {
        for (auto &prop : kReserveProps_) {
            storage::cpp2::PropDef pd;
            pd.owner = storage::cpp2::PropOwner::EDGE;
            pd.name = prop;
            pd.id.set_edge_type(e);
            props.emplace_back(std::move(pd));
        }
    }

    return props;
}

std::string FindPathExecutor::buildPathString(Path &path) {
    std::string pathStr;
    auto iter = path.begin();
    for (; iter != path.end(); ++iter) {
        auto *step = *iter;
        auto id = std::get<0>(*step);
        auto type = std::get<1>(*step);
        auto ranking = std::get<2>(*step);
        if (type < 0) {
            pathStr += folly::to<std::string>(id);
            ++iter;
            break;
        }

        pathStr += folly::stringPrintf("%ld<%d,%ld>", id, type, ranking);
    }

    for (; iter != path.end(); ++iter) {
        auto *step = *iter;
        auto id = std::get<0>(*step);
        auto type = std::get<1>(*step);
        auto ranking = std::get<2>(*step);

        pathStr += folly::stringPrintf("<%d,%ld>%ld", -type, ranking, id);
    }

    return pathStr;
}

cpp2::RowValue FindPathExecutor::buildPathRow(Path &path) {
    cpp2::RowValue rowValue;
    std::vector<cpp2::ColumnValue> row;
    cpp2::Path pathValue;
    auto entryList = pathValue.get_entry_list();
    auto iter = path.begin();
    for (; iter != path.end(); ++iter) {
        auto *step = *iter;
        auto id = std::get<0>(*step);
        auto type = std::get<1>(*step);
        auto ranking = std::get<2>(*step);
        if (type < 0) {
            entryList.emplace_back();
            cpp2::Vertex vertex;
            vertex.set_id(id);
            entryList.back().set_vertex(std::move(vertex));
            ++iter;
            break;
        }
        entryList.emplace_back();
        cpp2::Vertex vertex;
        vertex.set_id(id);
        entryList.back().set_vertex(std::move(vertex));

        entryList.emplace_back();
        cpp2::Edge edge;
        auto typeName = edgeTypeNameMap_.find(type);
        DCHECK(typeName != edgeTypeNameMap_.end()) << type;
        edge.set_type(typeName->second);
        edge.set_ranking(ranking);
        entryList.back().set_edge(std::move(edge));
    }

    for (; iter != path.end(); ++iter) {
        auto *step = *iter;
        auto id = std::get<0>(*step);
        auto type = std::get<1>(*step);
        auto ranking = std::get<2>(*step);

        entryList.emplace_back();
        cpp2::Edge edge;
        auto typeName = edgeTypeNameMap_.find(-type);
        DCHECK(typeName != edgeTypeNameMap_.end()) << type;
        edge.set_type(typeName->second);
        edge.set_ranking(ranking);
        entryList.back().set_edge(std::move(edge));

        entryList.emplace_back();
        cpp2::Vertex vertex;
        vertex.set_id(id);
        entryList.back().set_vertex(std::move(vertex));
    }

    row.emplace_back();
    pathValue.set_entry_list(std::move(entryList));
    row.back().set_path(std::move(pathValue));
    rowValue.set_columns(std::move(row));
    return rowValue;
}

void FindPathExecutor::setupResponse(cpp2::ExecutionResponse &resp) {
    std::vector<cpp2::RowValue> rows;
    for (auto &path : finalPath_) {
        auto row = buildPathRow(path.second);
        rows.emplace_back(std::move(row));
        VLOG(1) << "Path: " << buildPathString(path.second);
    }

    std::vector<std::string> colNames = {"_path_"};
    resp.set_column_names(std::move(colNames));
    resp.set_rows(std::move(rows));
}
}  // namespace graph
}  // namespace nebula
